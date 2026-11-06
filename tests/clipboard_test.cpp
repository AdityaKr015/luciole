#include "platform/text_clipboard.h"
#include "test_check.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <wayland-server.h>

namespace {
  using namespace files;
  using namespace std::chrono_literals;
  using MimeData = std::unordered_map<std::string, std::string>;

  // A private wire peer, without a compositor, desktop clipboard or mock UI.
  class ClipboardPeer {
    struct Source {
      ClipboardPeer* peer;
      std::unordered_set<std::string> types;
    };
    struct Offer {
      ClipboardPeer* peer;
      wl_resource* source;
      MimeData data;
    };
    wl_display* m_server = wl_display_create();
    wl_display* m_client = nullptr;
    wl_registry* m_registry = nullptr;
    wl_seat* m_seat = nullptr;
    wl_data_device_manager* m_manager = nullptr;
    wl_resource* m_device = nullptr;
    wl_resource* m_selection = nullptr;
    std::unordered_set<wl_resource*> m_offers;
    MimeData m_external;
    std::unique_ptr<WaylandTextClipboard> m_clipboard;
    bool m_focused = true;
    std::uint32_t m_serial = 10;
    std::size_t m_writes = 0, m_invalidWrites = 0;
    std::uint32_t m_writtenSerial = 0;

    void offer() {
      auto* resource = wl_resource_create(wl_resource_get_client(m_device), &wl_data_offer_interface, 3, 0);
      auto* data = new Offer{this, m_selection, m_external};
      m_offers.insert(resource);
      static const struct wl_data_offer_interface implementation{
          .accept = [](wl_client*, wl_resource*, std::uint32_t, const char*) {},
          .receive =
              [](wl_client*, wl_resource* resource, const char* mime, int fd) {
                const auto& offer = *static_cast<Offer*>(wl_resource_get_user_data(resource));
                if (offer.source) {
                  wl_data_source_send_send(offer.source, mime, fd);
                } else if (const auto found = offer.data.find(mime); found != offer.data.end()) {
                  std::size_t offset = 0;
                  while (offset < found->second.size()) {
                    const auto written = ::write(fd, found->second.data() + offset, found->second.size() - offset);
                    if (written < 0 && errno == EINTR) {
                      continue;
                    }
                    if (written <= 0) {
                      break;
                    }
                    offset += static_cast<std::size_t>(written);
                  }
                }
                ::close(fd);
              },
          .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
          .finish = [](wl_client*, wl_resource*) {},
          .set_actions = [](wl_client*, wl_resource*, std::uint32_t, std::uint32_t) {}
      };
      wl_resource_set_implementation(resource, &implementation, data, [](wl_resource* resource) {
        const std::unique_ptr<Offer> data(static_cast<Offer*>(wl_resource_get_user_data(resource)));
        data->peer->m_offers.erase(resource);
      });
      wl_data_device_send_data_offer(m_device, resource);
      if (m_selection) {
        for (const auto& type : static_cast<Source*>(wl_resource_get_user_data(m_selection))->types) {
          wl_data_offer_send_offer(resource, type.c_str());
        }
      } else {
        for (const auto& [type, unused] : m_external) {
          wl_data_offer_send_offer(resource, type.c_str());
        }
      }
      wl_data_device_send_selection(m_device, resource);
    }
    static void bindManager(wl_client* client, void* data, std::uint32_t, std::uint32_t id) {
      auto* resource = wl_resource_create(client, &wl_data_device_manager_interface, 3, id);
      static const struct wl_data_device_manager_interface implementation{
          .create_data_source =
              [](wl_client* client, wl_resource* manager, std::uint32_t id) {
                auto* peer = static_cast<ClipboardPeer*>(wl_resource_get_user_data(manager));
                auto* resource = wl_resource_create(client, &wl_data_source_interface, 3, id);
                static const struct wl_data_source_interface implementation{
                    .offer = [](
                                 wl_client*, wl_resource* resource, const char* type
                             ) { static_cast<Source*>(wl_resource_get_user_data(resource))->types.insert(type); },
                    .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
                    .set_actions = [](wl_client*, wl_resource*, std::uint32_t) {}
                };
                wl_resource_set_implementation(
                    resource, &implementation, new Source{peer, {}}, [](wl_resource* resource) {
                      const std::unique_ptr<Source> data(static_cast<Source*>(wl_resource_get_user_data(resource)));
                      if (data->peer->m_selection == resource) {
                        data->peer->m_selection = nullptr;
                      }
                      for (auto* offer : data->peer->m_offers) {
                        auto* value = static_cast<Offer*>(wl_resource_get_user_data(offer));
                        if (value->source == resource) {
                          value->source = nullptr;
                        }
                      }
                    }
                );
              },
          .get_data_device =
              [](wl_client* client, wl_resource* manager, std::uint32_t id, wl_resource*) {
                auto& peer = *static_cast<ClipboardPeer*>(wl_resource_get_user_data(manager));
                peer.m_device = wl_resource_create(client, &wl_data_device_interface, 3, id);
                static const struct wl_data_device_interface implementation{
                    .start_drag = [](wl_client*, wl_resource*, wl_resource*, wl_resource*, wl_resource*,
                                     std::uint32_t) {},
                    .set_selection =
                        [](wl_client*, wl_resource* resource, wl_resource* source, std::uint32_t serial) {
                          auto& peer = *static_cast<ClipboardPeer*>(wl_resource_get_user_data(resource));
                          if (!peer.m_focused || serial != peer.m_serial) {
                            ++peer.m_invalidWrites;
                            return;
                          }
                          ++peer.m_writes;
                          peer.m_writtenSerial = serial;
                          if (peer.m_selection && peer.m_selection != source) {
                            wl_data_source_send_cancelled(peer.m_selection);
                          }
                          peer.m_selection = source;
                          peer.m_external.clear();
                          if (source) {
                            peer.offer();
                          } else {
                            wl_data_device_send_selection(resource, nullptr);
                          }
                        },
                    .release = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
                };
                wl_resource_set_implementation(peer.m_device, &implementation, &peer, nullptr);
              },
          .release = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
      };
      wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }

  public:
    ClipboardPeer() {
      TEST_CHECK(m_server != nullptr);
      wl_global_create(m_server, &wl_data_device_manager_interface, 3, this, &bindManager);
      wl_global_create(
          m_server, &wl_seat_interface, 1, nullptr, [](wl_client* client, void*, std::uint32_t, std::uint32_t id) {
            auto* resource = wl_resource_create(client, &wl_seat_interface, 1, id);
            wl_resource_set_implementation(resource, nullptr, nullptr, nullptr);
            wl_seat_send_capabilities(resource, 0);
          }
      );
      int fds[2];
      TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
      TEST_CHECK(wl_client_create(m_server, fds[0]) != nullptr);
      m_client = wl_display_connect_to_fd(fds[1]);
      TEST_CHECK(m_client != nullptr);
      m_registry = wl_display_get_registry(m_client);
      static const wl_registry_listener registry{
          .global =
              [](void* data, wl_registry* registry, std::uint32_t id, const char* type, std::uint32_t) {
                auto& peer = *static_cast<ClipboardPeer*>(data);
                if (std::string_view(type) == wl_data_device_manager_interface.name) {
                  peer.m_manager = static_cast<wl_data_device_manager*>(
                      wl_registry_bind(registry, id, &wl_data_device_manager_interface, 3)
                  );
                } else if (std::string_view(type) == wl_seat_interface.name) {
                  peer.m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, id, &wl_seat_interface, 1));
                }
              },
          .global_remove = [](void*, wl_registry*, std::uint32_t) {}
      };
      wl_registry_add_listener(m_registry, &registry, this);
      until([this] { return m_manager && m_seat; });
      m_clipboard = std::make_unique<WaylandTextClipboard>(
          m_client, m_manager, m_seat, [this] { return m_focused ? m_serial : 0; }, nullptr, nullptr
      );
      until([this] { return m_device != nullptr; });
    }
    ~ClipboardPeer() {
      m_clipboard.reset();
      wl_data_device_manager_destroy(m_manager);
      wl_seat_destroy(m_seat);
      wl_registry_destroy(m_registry);
      wl_display_flush(m_client);
      wl_display_disconnect(m_client);
      wl_display_destroy_clients(m_server);
      wl_display_destroy(m_server);
      while (g_main_context_iteration(nullptr, false)) {
      }
    }
    void pump() {
      TEST_CHECK(wl_display_flush(m_client) >= 0);
      wl_event_loop_dispatch(wl_display_get_event_loop(m_server), 0);
      wl_display_flush_clients(m_server);
      pollfd fd{wl_display_get_fd(m_client), POLLIN, 0};
      if (::poll(&fd, 1, 0) > 0) {
        TEST_CHECK(wl_display_dispatch(m_client) >= 0);
      }
      while (g_main_context_iteration(nullptr, false)) {
      }
    }
    template <typename Predicate> void until(Predicate condition) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (!condition() && std::chrono::steady_clock::now() < deadline) {
        pump();
        pollfd fd{wl_display_get_fd(m_client), POLLIN, 0};
        ::poll(&fd, 1, 1);
      }
      TEST_CHECK(condition());
      TEST_CHECK(m_invalidWrites == 0);
    }
    WaylandTextClipboard& clipboard() { return *m_clipboard; }
    void blur() { m_focused = false; }
    void enter() {
      m_focused = true;
      m_serial += 10;
      offer();
      // Selection is introduced before keyboard enter, even for the same owner.
      for (int i = 0; i < 3; ++i) {
        pump();
      }
      m_clipboard->keyboardFocusEntered();
      pump();
    }
    void replace(MimeData data) {
      if (m_selection) {
        wl_data_source_send_cancelled(std::exchange(m_selection, nullptr));
      }
      m_external = std::move(data);
      if (m_focused) {
        offer();
      }
      for (int i = 0; i < 3; ++i) {
        pump();
      }
    }
    void replaceDuringEnter(MimeData data) {
      m_focused = true;
      m_serial += 10;
      auto* previous = std::exchange(m_selection, nullptr);
      m_external = std::move(data);
      offer();
      for (int i = 0; i < 3; ++i) {
        pump();
      }
      m_clipboard->keyboardFocusEntered();
      // The source cancellation arrives after the keyboard-enter callback,
      // before the compositor processes this client's next request.
      wl_data_source_send_cancelled(previous);
      for (int i = 0; i < 4; ++i) {
        pump();
      }
    }
    ClipboardFiles capture() {
      std::optional<ClipboardFiles> selection;
      bool received = false;
      m_clipboard->requestFiles([&](auto value) {
        selection = std::move(value);
        received = true;
      });
      until([&] { return received; });
      TEST_CHECK(selection.has_value());
      return *selection;
    }
    std::string read(const char* mime) {
      if (!m_selection) {
        return m_external.contains(mime) ? m_external.at(mime) : "";
      }
      int fds[2];
      TEST_CHECK(pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0);
      wl_data_source_send_send(m_selection, mime, fds[1]);
      ::close(fds[1]);
      std::string bytes;
      bool eof = false;
      until([&] {
        char buffer[4096];
        const auto count = ::read(fds[0], buffer, sizeof(buffer));
        if (count > 0) {
          bytes.append(buffer, static_cast<std::size_t>(count));
        } else if (count == 0) {
          eof = true;
        }
        return eof;
      });
      ::close(fds[0]);
      return bytes;
    }
    std::size_t writes() const { return m_writes; }
    std::uint32_t writtenSerial() const { return m_writtenSerial; }
    std::uint32_t serial() const { return m_serial; }
  };

  FilePayload payload() {
    return {{Location::fromPath("/tmp/clipboard-wire/first"), Location::fromPath("/tmp/clipboard-wire/second")}, true};
  }
  void testDeferredCompletion(bool partial) {
    ClipboardPeer peer;
    auto& clipboard = peer.clipboard();
    const auto original = payload();
    TEST_CHECK(clipboard.setFiles(original));
    peer.until([&] { return peer.writes() == 1; });
    const auto selection = peer.capture();
    TEST_CHECK(clipboard.claimMove(selection.revision));
    peer.blur();
    const std::vector<TransferResult> results{
        {original.locations[0], TransferOutcome::Moved, {}},
        {original.locations[1], partial ? TransferOutcome::Skipped : TransferOutcome::Moved, {}}
    };
    clipboard.finishMove(selection, results);
    peer.pump();
    TEST_CHECK(peer.writes() == 1);
    TEST_CHECK(!clipboard.isCut(original.locations[0]));
    TEST_CHECK(clipboard.isCut(original.locations[1]) == partial);
    TEST_CHECK(clipboard.hasFiles() == partial);
    TEST_CHECK(!clipboard.claimMove(selection.revision));
    TEST_CHECK(peer.read(kGnomeFilesMime.data()) == *encodeFilePayload(original, kGnomeFilesMime));
    peer.enter();
    peer.until([&] { return peer.writes() == 2; });
    TEST_CHECK(peer.writtenSerial() == peer.serial());
    if (partial) {
      const auto current = peer.capture();
      TEST_CHECK(current.payload.locations == std::vector{original.locations[1]});
      TEST_CHECK(current.revision != selection.revision);
      TEST_CHECK(peer.read(kGnomeFilesMime.data()) == *encodeFilePayload(current.payload, kGnomeFilesMime));
    } else {
      TEST_CHECK(!clipboard.hasFiles());
      TEST_CHECK(peer.read(kGnomeFilesMime.data()).empty());
    }
    clipboard.keyboardFocusEntered();
    clipboard.finishMove(selection, results);
    peer.pump();
    TEST_CHECK(peer.writes() == 2);
  }
  void testReplacement(bool duringEnter) {
    ClipboardPeer peer;
    auto& clipboard = peer.clipboard();
    const auto original = payload();
    clipboard.setFiles(original);
    peer.until([&] { return peer.writes() == 1; });
    const auto selection = peer.capture();
    clipboard.claimMove(selection.revision);
    peer.blur();
    clipboard.finishMove(selection, {{original.locations[0], TransferOutcome::Moved, {}}});
    if (duringEnter) {
      peer.replaceDuringEnter({{"text/plain", "new external selection"}});
    } else {
      peer.replace({{"text/plain", "new external selection"}});
      peer.enter();
    }
    TEST_CHECK(peer.writes() == 1);
    TEST_CHECK(peer.read("text/plain") == "new external selection");
    TEST_CHECK(!clipboard.isCut(original.locations[1]));
  }
  void testForeignOfferIdentity() {
    ClipboardPeer peer;
    auto& clipboard = peer.clipboard();
    const auto original = payload();
    const MimeData bytes{{std::string(kGnomeFilesMime), *encodeFilePayload(original, kGnomeFilesMime)}};
    peer.replace(bytes);
    const auto selection = peer.capture();
    TEST_CHECK(clipboard.claimMove(selection.revision));
    peer.blur();
    clipboard.finishMove(selection, {{original.locations[0], TransferOutcome::Moved, {}}});
    // Equal bytes do not establish identity: this is a fresh external intent.
    peer.replace(bytes);
    peer.enter();
    TEST_CHECK(peer.writes() == 0);
    TEST_CHECK(peer.capture().payload.locations == original.locations);
  }
} // namespace
int main() {
  try {
    testDeferredCompletion(false);
    testDeferredCompletion(true);
    testReplacement(false);
    testReplacement(true);
    testForeignOfferIdentity();
    std::println("clipboard wire tests passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "{}", error.what());
    return 1;
  }
}
