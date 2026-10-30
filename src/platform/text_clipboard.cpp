#include "platform/text_clipboard.h"

#include "fs/location.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
  constexpr std::size_t kTextLimit = 1024 * 1024;
  struct PipeTransfer {
    int fd;
    std::string text;
    bool sending;
    bool valid = true;
    ~PipeTransfer() {
      if (fd >= 0)
        ::close(fd);
    }
  };

  void transfer(GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
    auto& pipe = *static_cast<PipeTransfer*>(data);
    sigset_t blocked{}, previous{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    sigset_t pending{};
    sigpending(&pending);
    const bool pendingPipe = sigismember(&pending, SIGPIPE) == 1;
    const int flags = fcntl(pipe.fd, F_GETFL);
    if (flags < 0 || fcntl(pipe.fd, F_SETFL, flags | O_NONBLOCK) < 0)
      pipe.valid = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::size_t offset = 0;
    std::array<char, 8192> buffer{};
    while (pipe.valid && !g_cancellable_is_cancelled(cancellable)) {
      if (pipe.sending && offset == pipe.text.size())
        break;
      if (std::chrono::steady_clock::now() >= deadline) {
        pipe.valid = false;
        break;
      }
      pollfd fd{pipe.fd, static_cast<short>(pipe.sending ? POLLOUT : POLLIN), 0};
      const int ready = ::poll(&fd, 1, 50);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0) {
        pipe.valid = false;
        break;
      }
      if (ready == 0)
        continue;
      const auto count = pipe.sending ? ::write(pipe.fd, pipe.text.data() + offset, pipe.text.size() - offset)
                                      : ::read(pipe.fd, buffer.data(), buffer.size());
      if (count < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      if (count < 0) {
        pipe.valid = false;
        break;
      }
      if (count == 0)
        break;
      if (pipe.sending)
        offset += static_cast<std::size_t>(count);
      else {
        pipe.text.append(buffer.data(), static_cast<std::size_t>(count));
        if (pipe.text.size() > kTextLimit)
          pipe.valid = false;
      }
    }
    if (!pendingPipe) {
      const timespec noWait{};
      (void)sigtimedwait(&blocked, nullptr, &noWait);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    pipe.valid = pipe.valid && !g_cancellable_is_cancelled(cancellable);
    if (!pipe.sending)
      pipe.valid = pipe.valid
          && !pipe.text.contains('\0')
          && g_utf8_validate(pipe.text.data(), static_cast<gssize>(pipe.text.size()), nullptr);
    // Closing before notification also gives receiving clients their EOF promptly.
    ::close(std::exchange(pipe.fd, -1));
    g_task_return_boolean(task, pipe.valid);
  }
} // namespace

struct files::WaylandTextClipboard::State : std::enable_shared_from_this<State> {
  wl_data_device_manager* manager = nullptr;
  wl_data_device* device = nullptr;
  wl_data_offer* selected = nullptr;
  wl_data_offer* dragging = nullptr;
  std::function<std::uint32_t()> serial;
  std::unordered_map<wl_data_offer*, std::unordered_set<std::string>> offers;
  std::unordered_map<wl_data_source*, std::string> sources;
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  GioPtr<GCancellable> reading{g_cancellable_new()};
  std::optional<std::string> text;
  bool pending = false;
  bool active = true;
  std::uint64_t epoch = 0;
  std::vector<std::function<void(std::optional<std::string>)>> callbacks;

  void discard(wl_data_offer* offer) {
    if (offer && offers.erase(offer))
      wl_data_offer_destroy(offer);
  }
  void received() {
    auto waiting = std::exchange(callbacks, {});
    const auto value = text;
    for (const auto& callback : waiting)
      callback(value);
  }
  void selection(wl_data_offer* offer) {
    const auto retained = shared_from_this();
    ++epoch;
    g_cancellable_cancel(reading.get());
    reading.reset(g_cancellable_new());
    pending = false;
    text.reset();
    if (selected != offer)
      discard(selected);
    selected = offer;
    // Retire paste requests from the previous selection rather than retargeting them.
    received();
    if (!retained->active)
      return;
    if (!offer || !offers.contains(offer))
      return;
    const auto& mimeTypes = offers.at(offer);
    const char* mime = mimeTypes.contains("text/plain;charset=utf-8") ? "text/plain;charset=utf-8"
        : mimeTypes.contains("text/plain")                            ? "text/plain"
                                                                      : nullptr;
    if (!mime)
      return;
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0)
      return;
    wl_data_offer_receive(offer, mime, fds[1]);
    ::close(fds[1]);
    pending = true;
    struct Read {
      std::shared_ptr<State> state;
      std::uint64_t epoch;
    };
    GTask* task = g_task_new(
        nullptr, reading.get(),
        [](GObject*, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Read> read(static_cast<Read*>(data));
          auto& state = *read->state;
          if (!state.active || read->epoch != state.epoch)
            return;
          auto* pipe = static_cast<PipeTransfer*>(g_task_get_task_data(G_TASK(result)));
          state.pending = false;
          if (pipe->valid)
            state.text = pipe->text;
          state.received();
        },
        new Read{shared_from_this(), epoch}
    );
    g_task_set_task_data(task, new PipeTransfer{fds[0], {}, false}, [](gpointer data) {
      delete static_cast<PipeTransfer*>(data);
    });
    g_task_run_in_thread(task, transfer);
    g_object_unref(task);
  }
};

files::WaylandTextClipboard::WaylandTextClipboard(
    wl_data_device_manager* manager, wl_seat* seat, std::function<std::uint32_t()> serial
)
    : m_state(std::make_shared<State>()) {
  auto& state = *m_state;
  state.manager = manager;
  state.serial = std::move(serial);
  if (!manager || !seat)
    return;
  state.device = wl_data_device_manager_get_data_device(manager, seat);
  static const wl_data_device_listener listener{
      .data_offer =
          [](void* data, wl_data_device*, wl_data_offer* offer) {
            auto& state = *static_cast<State*>(data);
            state.offers.try_emplace(offer);
            static const wl_data_offer_listener offers{
                .offer = [](void* data, wl_data_offer* offer,
                            const char* mime) { static_cast<State*>(data)->offers[offer].insert(mime); },
                .source_actions = [](void*, wl_data_offer*, std::uint32_t) {},
                .action = [](void*, wl_data_offer*, std::uint32_t) {}
            };
            wl_data_offer_add_listener(offer, &offers, &state);
          },
      .enter = [](void* data, wl_data_device*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
                  wl_data_offer* offer) { static_cast<State*>(data)->dragging = offer; },
      .leave =
          [](void* data, wl_data_device*) {
            auto& state = *static_cast<State*>(data);
            state.discard(state.dragging);
            state.dragging = nullptr;
          },
      .motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
      .drop = [](void*, wl_data_device*) {},
      .selection = [](void* data, wl_data_device*,
                      wl_data_offer* offer) { static_cast<State*>(data)->selection(offer); }
  };
  wl_data_device_add_listener(state.device, &listener, &state);
}
files::WaylandTextClipboard::~WaylandTextClipboard() {
  auto& state = *m_state;
  state.active = false;
  state.callbacks.clear();
  g_cancellable_cancel(state.reading.get());
  g_cancellable_cancel(state.cancellable.get());
  for (const auto& [offer, unused] : state.offers)
    wl_data_offer_destroy(offer);
  for (const auto& [source, unused] : state.sources)
    wl_data_source_destroy(source);
  if (state.device) {
    if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(state.device)) >= 2)
      wl_data_device_release(state.device);
    else
      wl_data_device_destroy(state.device);
  }
}
bool files::WaylandTextClipboard::available() const { return m_state->device && m_state->serial() != 0; }
std::optional<std::string> files::WaylandTextClipboard::clipboardText() { return m_state->text; }
void files::WaylandTextClipboard::requestClipboardText(std::function<void(std::optional<std::string>)> callback) {
  if (m_state->pending)
    m_state->callbacks.push_back(std::move(callback));
  else
    callback(m_state->text);
}
void files::WaylandTextClipboard::setClipboardText(std::string text) {
  if (!available()
      || text.size() > kTextLimit
      || text.contains('\0')
      || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr))
    return;
  auto& state = *m_state;
  auto* source = wl_data_device_manager_create_data_source(state.manager);
  state.sources.emplace(source, text);
  static const wl_data_source_listener listener{
      .target = [](void*, wl_data_source*, const char*) {},
      .send =
          [](void* data, wl_data_source* source, const char*, int fd) {
            auto& state = *static_cast<State*>(data);
            const auto found = state.sources.find(source);
            if (found == state.sources.end()) {
              ::close(fd);
              return;
            }
            GTask* task = g_task_new(nullptr, state.cancellable.get(), nullptr, nullptr);
            g_task_set_task_data(task, new PipeTransfer{fd, found->second, true}, [](gpointer data) {
              delete static_cast<PipeTransfer*>(data);
            });
            g_task_run_in_thread(task, transfer);
            g_object_unref(task);
          },
      .cancelled =
          [](void* data, wl_data_source* source) {
            static_cast<State*>(data)->sources.erase(source);
            wl_data_source_destroy(source);
          },
      .dnd_drop_performed = [](void*, wl_data_source*) {},
      .dnd_finished = [](void*, wl_data_source*) {},
      .action = [](void*, wl_data_source*, std::uint32_t) {}
  };
  wl_data_source_add_listener(source, &listener, &state);
  wl_data_source_offer(source, "text/plain;charset=utf-8");
  wl_data_source_offer(source, "text/plain");
  wl_data_device_set_selection(state.device, source, state.serial());
  ++state.epoch;
  g_cancellable_cancel(state.reading.get());
  state.pending = false;
  state.text = std::move(text);
  state.received();
}
