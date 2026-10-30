#include "platform/file_launcher.h"

#include "core/log.h"

#include <cstdint>

namespace {
  constexpr Logger kLog("file-launch");
}

struct files::FileLauncher::State {
  struct Launch {
    std::shared_ptr<State> state;
    GioPtr<GAppLaunchContext> context{g_app_launch_context_new()};
    std::uint64_t serial;
    gint64 started;
    ~Launch() { g_signal_handlers_disconnect_by_data(context.get(), this); }
    [[nodiscard]] double elapsedMs() const { return static_cast<double>(g_get_monotonic_time() - started) / 1000.0; }
  };
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::function<void(const std::string&)> changed;
  std::size_t active = 0;
  std::uint64_t nextSerial = 0;
};

files::FileLauncher::FileLauncher() : m_state(std::make_shared<State>()) {}
files::FileLauncher::~FileLauncher() {
  m_state->changed = {};
  g_cancellable_cancel(m_state->cancellable.get());
}
void files::FileLauncher::setChangedCallback(std::function<void(const std::string&)> callback) {
  m_state->changed = std::move(callback);
}
bool files::FileLauncher::busy() const { return m_state->active != 0; }
void files::FileLauncher::open(const Location& location) {
  const auto started = g_get_monotonic_time();
  const auto serial = ++m_state->nextSerial;
  kLog.debug("request {}: begin", serial);
  ++m_state->active;
  auto* launch = new State::Launch{m_state, GioPtr<GAppLaunchContext>(g_app_launch_context_new()), serial, started};
  kLog.debug("request {}: context ready at {:.2f} ms", serial, launch->elapsedMs());
  g_signal_connect(
      launch->context.get(), "launch-started",
      G_CALLBACK(+[](GAppLaunchContext*, GAppInfo* app, GVariant*, gpointer data) {
        const auto& request = *static_cast<State::Launch*>(data);
        const char* handler = g_app_info_get_id(app);
        kLog.debug(
            "request {}: handler={} at {:.2f} ms", request.serial, handler ? handler : "unknown", request.elapsedMs()
        );
      }),
      launch
  );
  g_signal_connect(
      launch->context.get(), "launched",
      G_CALLBACK(+[](GAppLaunchContext*, GAppInfo*, GVariant* platform, gpointer data) {
        const auto& request = *static_cast<State::Launch*>(data);
        gint32 pid = 0;
        if (platform)
          (void)g_variant_lookup(platform, "pid", "i", &pid);
        kLog.debug(
            "request {}: desktop handoff at {:.2f} ms (pid={}, not viewer readiness)", request.serial,
            request.elapsedMs(), pid
        );
      }),
      launch
  );
  g_app_info_launch_default_for_uri_async(
      location.uri().c_str(), launch->context.get(), m_state->cancellable.get(),
      [](GObject*, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<State::Launch> request(static_cast<State::Launch*>(data));
        const auto& state = request->state;
        GError* error = nullptr;
        const bool launched = g_app_info_launch_default_for_uri_finish(result, &error);
        --state->active;
        const bool cancelled = g_cancellable_is_cancelled(state->cancellable.get());
        kLog.debug(
            "request {}: GIO completion at {:.2f} ms ({})", request->serial, request->elapsedMs(),
            cancelled      ? "cancelled"
                : launched ? "success"
                           : "failed"
        );
        const auto callback = state->changed;
        const std::string message = error != nullptr ? error->message : (launched ? "" : "Could not open this file.");
        g_clear_error(&error);
        if (callback && !cancelled)
          callback(message);
      },
      launch
  );
  kLog.debug(
      "request {}: GIO call returned at {:.2f} ms", serial,
      static_cast<double>(g_get_monotonic_time() - started) / 1000.0
  );
}
