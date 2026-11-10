#include "core/log.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "platform/file_launcher.h"
#include "test_check.h"

#include <algorithm>
#include <print>
#include <set>
#include <stdexcept>
#include <string_view>

namespace {
  files::ApplicationList query(files::FileLauncher& launcher, bool all, std::string search = {}) {
    std::optional<files::ApplicationList> result;
    launcher.applications("text/plain", [&](auto value) { result = std::move(value); }, all, std::move(search));
    const auto deadline = g_get_monotonic_time() + 5000000;
    while (launcher.busy()) {
      (void)g_main_context_iteration(nullptr, false);
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(result.has_value());
    return std::move(*result);
  }

  files::ApplicationChoice testCatalog(files::FileLauncher& launcher, bool large) {
    TEST_CHECK(std::string_view(g_get_user_config_dir()) == g_getenv("XDG_CONFIG_HOME"));
    TEST_CHECK(std::string_view(g_get_user_data_dir()) == g_getenv("XDG_DATA_HOME"));
    const auto suggested = query(launcher, false);
    TEST_CHECK(
        !suggested.choices.empty()
        && suggested.choices.front().id == "fixture.desktop"
        && suggested.choices.front().suggested
        && suggested.choices.front().isDefault
    );
    TEST_CHECK(std::ranges::none_of(suggested.choices, [](const auto& app) { return app.id == "other.desktop"; }));
    TEST_CHECK(query(launcher, false, "NOCTALIA LAUNCH FIXTURE CAFÉ").choices.empty());
    const auto other = query(launcher, true, "NOCTALIA LAUNCH FIXTURE CAFE\u0301");
    TEST_CHECK(
        other.choices.size() == 1
        && other.choices.front().id == "other.desktop"
        && !other.choices.front().suggested
        && !other.choices.front().isDefault
    );
    const auto all = query(launcher, true);
    std::set<std::string> ids;
    for (const auto& app : all.choices) {
      TEST_CHECK(app.id != "hidden.desktop" && app.id != "no-files.desktop" && ids.insert(app.id).second);
    }
    TEST_CHECK(all.choices.front().isDefault);
    if (large) {
      TEST_CHECK(all.truncated && all.choices.size() == 128);
      const auto last = query(launcher, true, "lastneedle");
      TEST_CHECK(!last.truncated && last.choices.size() == 1 && last.choices.front().id == "bulk-149.desktop");
    }
    return other.choices.front();
  }

  void testDefault(files::FileLauncher& launcher, const files::ApplicationChoice& app, bool failure) {
    const auto set = [&](const std::string& mime) {
      std::optional<std::string> error;
      launcher.setDefault(mime, app, [&](std::string value) { error = std::move(value); });
      const auto deadline = g_get_monotonic_time() + 5000000;
      while (launcher.busy()) {
        (void)g_main_context_iteration(nullptr, false);
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
      TEST_CHECK(error.has_value());
      return *error;
    };
    TEST_CHECK(!set("").empty());
    const auto error = set("text/plain");
    TEST_CHECK(error.empty() != failure);
    if (!failure) {
      const files::GioPtr<GAppInfo> current(g_app_info_get_default_for_type("text/plain", false));
      TEST_CHECK(current && std::string_view(g_app_info_get_id(current.get())) == app.id);
      const auto listed = query(launcher, false);
      TEST_CHECK(!listed.choices.empty() && listed.choices.front().id == app.id && listed.choices.front().isDefault);
    }
  }
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 3) {
      throw std::invalid_argument("expected mode and native fixture paths");
    }
    initLogLevelFromEnvironment();
    GlibPollSource glib;
    auto launcher = std::make_unique<files::FileLauncher>();
    const bool destroy = std::string_view(argv[1]) == "destroy";
    const bool expectError = std::string_view(argv[1]) == "error";
    const bool sequential = std::string_view(argv[1]) == "sequential";
    const std::string_view mode(argv[1]);
    const bool catalog = mode == "catalog", changeDefault = mode == "default", defaultError = mode == "default-error";
    const bool other = mode == "other";
    const bool with = mode == "with" || other;
    int callbacks = 0, errors = 0;
    launcher->setChangedCallback([&](const std::string& error) {
      ++callbacks;
      if (!error.empty()) {
        ++errors;
      }
    });
    const auto deadline = g_get_monotonic_time() + 5000000;
    std::optional<files::ApplicationChoice> choice;
    bool staleChoice = false;
    if (other || catalog || changeDefault || defaultError) {
      choice = testCatalog(*launcher, catalog);
      if (changeDefault || defaultError) {
        testDefault(*launcher, *choice, defaultError);
      }
      if (catalog || defaultError) {
        TEST_CHECK(callbacks == 0);
        std::println("application catalog/default checks passed");
        return 0;
      }
    }
    if (with && !other) {
      launcher->applications("text/plain", [&](const auto&) { staleChoice = true; });
      launcher->applications("text/plain", [&](auto apps) {
        for (auto& app : apps.choices) {
          if (app.id == "fixture.desktop") {
            choice = std::move(app);
          }
        }
      });
      while (launcher->busy()) {
        (void)g_main_context_iteration(nullptr, false);
        if (g_get_monotonic_time() >= deadline) {
          throw std::runtime_error("application lookup timed out");
        }
        g_usleep(1000);
      }
      if (staleChoice || !choice) {
        throw std::runtime_error("application choice missing or superseded query published");
      }
    }
    for (int index = 2; index < argc; ++index) {
      if (with) {
        launcher->openWith(files::Location::fromPath(argv[index]), *choice);
      } else {
        launcher->open(files::Location::fromPath(argv[index]));
      }
      while (sequential && launcher->busy()) {
        if (g_get_monotonic_time() >= deadline) {
          throw std::runtime_error("sequential launch timed out");
        }
        std::vector<pollfd> fds;
        const auto first = glib.addPollFds(fds);
        const int requested = glib.pollTimeoutMs();
        if (::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) < 0) {
          throw std::runtime_error("poll failed");
        }
        glib.dispatch(fds, first);
      }
    }
    if (destroy) {
      launcher.reset();
    }
    bool drained = false;
    guint drainTimer = 0;
    if (destroy) {
      drainTimer = g_timeout_add(
          100,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &drained
      );
    }
    while (destroy ? !drained : launcher->busy()) {
      if (g_get_monotonic_time() >= deadline) {
        if (drainTimer) {
          g_source_remove(drainTimer);
        }
        throw std::runtime_error("file launcher timed out");
      }
      std::vector<pollfd> fds;
      const auto first = glib.addPollFds(fds);
      const int requested = glib.pollTimeoutMs();
      if (::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) < 0) {
        throw std::runtime_error("poll failed");
      }
      glib.dispatch(fds, first);
    }
    if (callbacks != (destroy ? 0 : argc - 2) || errors != (expectError ? argc - 2 : 0)) {
      throw std::runtime_error("unexpected launch callback/error count");
    }
    if (other) {
      const files::GioPtr<GAppInfo> current(g_app_info_get_default_for_type("text/plain", false));
      TEST_CHECK(current && std::string_view(g_app_info_get_id(current.get())) == "fixture.desktop");
    }
    std::println("file launcher checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "file launcher test: {}", error.what());
    return 1;
  }
}
