#include "platform/file_launcher.h"

#include "core/log.h"
#include "fs/gio_interop.h"
#include "fs/name_matching.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>

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
    double elapsedMs() const { return static_cast<double>(g_get_monotonic_time() - started) / 1000.0; }
  };
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::function<void(const std::string&)> changed;
  std::size_t active = 0;
  std::uint64_t nextSerial = 0;
  std::uint64_t querySerial = 0;
  GioPtr<GCancellable> queryCancel;
};

files::FileLauncher::FileLauncher() : m_state(std::make_shared<State>()) {}
files::FileLauncher::~FileLauncher() {
  cancelApplications();
  m_state->changed = {};
  g_cancellable_cancel(m_state->cancellable.get());
}
void files::FileLauncher::cancelApplications() {
  ++m_state->querySerial;
  if (m_state->queryCancel) {
    g_cancellable_cancel(m_state->queryCancel.get());
  }
}
void files::FileLauncher::applications(
    std::string mime, std::function<void(ApplicationList)> callback, bool includeOther, std::string search
) {
  cancelApplications();
  m_state->queryCancel.reset(g_cancellable_new());
  struct Query {
    std::shared_ptr<State> state;
    std::uint64_t serial;
    std::string mime;
    std::function<void(ApplicationList)> callback;
    bool includeOther;
    std::string search;
  };
  ++m_state->active;
  const GioPtr<GTask> task(g_task_new(
      nullptr, m_state->queryCancel.get(),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& query = *static_cast<Query*>(g_task_get_task_data(G_TASK(result)));
        --query.state->active;
        const std::unique_ptr<ApplicationList> choices(
            static_cast<ApplicationList*>(g_task_propagate_pointer(G_TASK(result), nullptr))
        );
        if (choices && query.serial == query.state->querySerial) {
          query.callback(std::move(*choices));
        }
      },
      nullptr
  ));
  g_task_set_task_data(
      task.get(),
      new Query{m_state, m_state->querySerial, std::move(mime), std::move(callback), includeOther, std::move(search)},
      [](gpointer data) { delete static_cast<Query*>(data); }
  );
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
    const auto& query = *static_cast<Query*>(data);
    auto result = std::make_unique<ApplicationList>();
    const auto search = foldedDisplayText(query.search);
    const GioPtr<GAppInfo> defaultApp(g_app_info_get_default_for_type(query.mime.c_str(), false));
    std::set<std::string> seen;
    const auto append = [&](GList* applications, bool suggested) {
      for (auto* item = applications; item && !g_cancellable_is_cancelled(cancel); item = item->next) {
        auto* app = G_APP_INFO(item->data);
        const char* id = g_app_info_get_id(app);
        const char* name = g_app_info_get_display_name(app);
        if (!id
            || !name
            || !g_app_info_should_show(app)
            || (!g_app_info_supports_files(app) && !g_app_info_supports_uris(app))
            || !seen.emplace(id).second) {
          continue;
        }
        if (!search.empty() && !foldedDisplayText(std::string(name) + " " + id).contains(search)) {
          continue;
        }
        result->choices.push_back(
            {id, name, readIconSource(g_app_info_get_icon(app)),
             std::shared_ptr<GAppInfo>(G_APP_INFO(g_object_ref(app)), ObjectUnref{}), suggested,
             defaultApp && g_app_info_equal(app, defaultApp.get())}
        );
      }
      g_list_free_full(applications, g_object_unref);
    };
    append(g_app_info_get_all_for_type(query.mime.c_str()), true);
    if (query.includeOther && !g_cancellable_is_cancelled(cancel)) {
      append(g_app_info_get_all(), false);
    }
    std::ranges::sort(result->choices, [](const auto& left, const auto& right) {
      if (left.isDefault != right.isDefault) {
        return left.isDefault;
      }
      if (left.suggested != right.suggested) {
        return left.suggested;
      }
      const auto compared = g_utf8_collate(left.name.c_str(), right.name.c_str());
      return compared == 0 ? left.id < right.id : compared < 0;
    });
    // Search is applied before this UI bound, so every installed application remains discoverable.
    if (result->choices.size() > 128) {
      result->truncated = true;
      result->choices.resize(128);
    }
    g_task_return_pointer(task, result.release(), [](gpointer p) { delete static_cast<ApplicationList*>(p); });
  });
}

void files::FileLauncher::setDefault(
    std::string mime, ApplicationChoice application, std::function<void(std::string)> callback
) {
  struct Change {
    std::shared_ptr<State> state;
    ApplicationChoice application;
    std::string mime;
    std::function<void(std::string)> callback;
  };
  ++m_state->active;
  const GioPtr<GTask> task(g_task_new(
      nullptr, m_state->cancellable.get(),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& change = *static_cast<Change*>(g_task_get_task_data(G_TASK(result)));
        --change.state->active;
        GError* error = nullptr;
        const bool success = g_task_propagate_boolean(G_TASK(result), &error);
        std::string message = error ? error->message : success ? "" : "Could not change the default application.";
        g_clear_error(&error);
        if (!g_cancellable_is_cancelled(change.state->cancellable.get())) {
          change.callback(std::move(message));
        }
      },
      nullptr
  ));
  g_task_set_task_data(
      task.get(), new Change{m_state, std::move(application), std::move(mime), std::move(callback)},
      [](gpointer data) { delete static_cast<Change*>(data); }
  );
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
    const auto& change = *static_cast<Change*>(data);
    if (g_task_return_error_if_cancelled(task)) {
      return;
    }
    if (!change.application.app
        || change.mime.empty()
        || change.mime.contains('\0')
        || !g_utf8_validate(change.mime.c_str(), -1, nullptr)
        || g_content_type_is_unknown(change.mime.c_str())) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "This file type has no editable default.");
      return;
    }
    GError* error = nullptr;
    if (!g_app_info_set_as_default_for_type(change.application.app.get(), change.mime.c_str(), &error)) {
      if (error) {
        g_task_return_error(task, error);
      } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not change the default application.");
      }
      return;
    }
    g_task_return_boolean(task, true);
  });
}

void files::FileLauncher::openWith(const Location& location, const ApplicationChoice& application) {
  if (!application.app) {
    return;
  }
  struct LaunchWith {
    std::shared_ptr<State> state;
    std::shared_ptr<GAppInfo> app;
    GioPtr<GAppLaunchContext> context{g_app_launch_context_new()};
    std::string uri;
  };
  auto* request =
      new LaunchWith{m_state, application.app, GioPtr<GAppLaunchContext>(g_app_launch_context_new()), location.uri()};
  GList uris{request->uri.data(), nullptr, nullptr};
  ++m_state->active;
  g_app_info_launch_uris_async(
      request->app.get(), &uris, request->context.get(), m_state->cancellable.get(),
      [](GObject* app, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<LaunchWith> request(static_cast<LaunchWith*>(data));
        GError* error = nullptr;
        const bool success = g_app_info_launch_uris_finish(G_APP_INFO(app), result, &error);
        kLog.debug("Open With GIO completion ({})", success ? "success" : "failed");
        --request->state->active;
        const std::string message = error ? error->message : success ? "" : "Could not open this file.";
        g_clear_error(&error);
        if (request->state->changed && !g_cancellable_is_cancelled(request->state->cancellable.get())) {
          request->state->changed(message);
        }
      },
      request
  );
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
        if (platform) {
          (void)g_variant_lookup(platform, "pid", "i", &pid);
        }
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
        if (callback && !cancelled) {
          callback(message);
        }
      },
      launch
  );
  kLog.debug(
      "request {}: GIO call returned at {:.2f} ms", serial,
      static_cast<double>(g_get_monotonic_time() - started) / 1000.0
  );
}
