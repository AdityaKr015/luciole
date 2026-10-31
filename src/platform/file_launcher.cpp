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
  if (m_state->queryCancel)
    g_cancellable_cancel(m_state->queryCancel.get());
}
void files::FileLauncher::applications(std::string mime, std::function<void(std::vector<ApplicationChoice>)> callback) {
  cancelApplications();
  m_state->queryCancel.reset(g_cancellable_new());
  struct Query {
    std::shared_ptr<State> state;
    std::uint64_t serial;
    std::string mime;
    std::function<void(std::vector<ApplicationChoice>)> callback;
  };
  ++m_state->active;
  const GioPtr<GTask> task(g_task_new(
      nullptr, m_state->queryCancel.get(),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& query = *static_cast<Query*>(g_task_get_task_data(G_TASK(result)));
        --query.state->active;
        const std::unique_ptr<std::vector<ApplicationChoice>> choices(
            static_cast<std::vector<ApplicationChoice>*>(g_task_propagate_pointer(G_TASK(result), nullptr))
        );
        if (choices && query.serial == query.state->querySerial)
          query.callback(std::move(*choices));
      },
      nullptr
  ));
  g_task_set_task_data(
      task.get(), new Query{m_state, m_state->querySerial, std::move(mime), std::move(callback)},
      [](gpointer data) { delete static_cast<Query*>(data); }
  );
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
    auto result = std::make_unique<std::vector<ApplicationChoice>>();
    GList* applications = g_app_info_get_all_for_type(static_cast<Query*>(data)->mime.c_str());
    for (auto* item = applications; item && result->size() < 128 && !g_cancellable_is_cancelled(cancel);
         item = item->next) {
      auto* app = G_APP_INFO(item->data);
      const char* id = g_app_info_get_id(app);
      const char* name = g_app_info_get_display_name(app);
      if (id && name && g_app_info_should_show(app))
        result->push_back({id, name, std::shared_ptr<GAppInfo>(G_APP_INFO(g_object_ref(app)), ObjectUnref{})});
    }
    g_list_free_full(applications, g_object_unref);
    g_task_return_pointer(task, result.release(), [](gpointer p) {
      delete static_cast<std::vector<ApplicationChoice>*>(p);
    });
  });
}
void files::FileLauncher::openWith(const Location& location, const ApplicationChoice& application) {
  if (!application.app)
    return;
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
        if (request->state->changed && !g_cancellable_is_cancelled(request->state->cancellable.get()))
          request->state->changed(message);
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
