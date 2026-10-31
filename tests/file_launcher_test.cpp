#include "app/glib_poll_source.h"
#include "core/log.h"
#include "platform/file_launcher.h"

#include <algorithm>
#include <print>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
  try {
    if (argc < 3)
      throw std::invalid_argument("expected mode and native fixture paths");
    initLogLevelFromEnvironment();
    GlibPollSource glib;
    auto launcher = std::make_unique<files::FileLauncher>();
    const bool destroy = std::string_view(argv[1]) == "destroy";
    const bool expectError = std::string_view(argv[1]) == "error";
    const bool sequential = std::string_view(argv[1]) == "sequential";
    const bool with = std::string_view(argv[1]) == "with";
    int callbacks = 0, errors = 0;
    launcher->setChangedCallback([&](const std::string& error) {
      ++callbacks;
      if (!error.empty())
        ++errors;
    });
    const auto deadline = g_get_monotonic_time() + 5000000;
    std::optional<files::ApplicationChoice> choice;
    bool staleChoice = false;
    if (with) {
      launcher->applications("text/plain", [&](const auto&) { staleChoice = true; });
      launcher->applications("text/plain", [&](auto apps) {
        for (auto& app : apps)
          if (app.id == "fixture.desktop")
            choice = std::move(app);
      });
      while (launcher->busy()) {
        (void)g_main_context_iteration(nullptr, false);
        if (g_get_monotonic_time() >= deadline)
          throw std::runtime_error("application lookup timed out");
        g_usleep(1000);
      }
      if (staleChoice || !choice)
        throw std::runtime_error("application choice missing or superseded query published");
    }
    for (int index = 2; index < argc; ++index) {
      if (with)
        launcher->openWith(files::Location::fromPath(argv[index]), *choice);
      else
        launcher->open(files::Location::fromPath(argv[index]));
      while (sequential && launcher->busy()) {
        if (g_get_monotonic_time() >= deadline)
          throw std::runtime_error("sequential launch timed out");
        std::vector<pollfd> fds;
        const auto first = glib.addPollFds(fds);
        const int requested = glib.pollTimeoutMs();
        if (::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) < 0)
          throw std::runtime_error("poll failed");
        glib.dispatch(fds, first);
      }
    }
    if (destroy)
      launcher.reset();
    bool drained = false;
    guint drainTimer = 0;
    if (destroy)
      drainTimer = g_timeout_add(
          100,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &drained
      );
    while (destroy ? !drained : launcher->busy()) {
      if (g_get_monotonic_time() >= deadline) {
        if (drainTimer)
          g_source_remove(drainTimer);
        throw std::runtime_error("file launcher timed out");
      }
      std::vector<pollfd> fds;
      const auto first = glib.addPollFds(fds);
      const int requested = glib.pollTimeoutMs();
      if (::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) < 0)
        throw std::runtime_error("poll failed");
      glib.dispatch(fds, first);
    }
    if (callbacks != (destroy ? 0 : argc - 2) || errors != (expectError ? argc - 2 : 0))
      throw std::runtime_error("unexpected launch callback/error count");
    std::println("file launcher checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "file launcher test: {}", error.what());
    return 1;
  }
}
