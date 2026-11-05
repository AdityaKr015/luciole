#include "platform/terminal_launcher.h"

struct files::TerminalLauncher::State {
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  GString helper{g_find_program_in_path("xdg-terminal-exec")};
  bool active = true;
};
files::TerminalLauncher::TerminalLauncher() : m_state(std::make_shared<State>()) {}
files::TerminalLauncher::~TerminalLauncher() {
  m_state->active = false;
  g_cancellable_cancel(m_state->cancellable.get());
}
bool files::TerminalLauncher::available() const { return m_state->helper != nullptr; }
void files::TerminalLauncher::open(const Location& folder, std::function<void(std::string)> completed) {
  struct Request {
    std::shared_ptr<State> state;
    Location folder;
    std::function<void(std::string)> completed;
  };
  GTask* task = g_task_new(
      nullptr, m_state->cancellable.get(),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& request = *static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
        GError* error = nullptr;
        GioPtr<GSubprocess> process(static_cast<GSubprocess*>(g_task_propagate_pointer(G_TASK(result), &error)));
        if (!request.state->active) {
          g_clear_error(&error);
          return;
        }
        if (!process) {
          std::string message = error ? error->message : "Could not launch the terminal.";
          g_clear_error(&error);
          request.completed(std::move(message));
          return;
        }
        auto* wait = new Request(request);
        g_subprocess_wait_check_async(
            process.get(), request.state->cancellable.get(),
            [](GObject* source, GAsyncResult* result, gpointer data) {
              const std::unique_ptr<Request> request(static_cast<Request*>(data));
              GError* error = nullptr;
              const bool ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);
              std::string message = ok ? "" : error ? error->message : "The terminal exited unsuccessfully.";
              g_clear_error(&error);
              if (request->state->active) {
                request->completed(std::move(message));
              }
            },
            wait
        );
      },
      nullptr
  );
  g_task_set_task_data(task, new Request{m_state, folder, std::move(completed)}, [](gpointer data) {
    delete static_cast<Request*>(data);
  });
  g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
    const auto& request = *static_cast<Request*>(data);
    const auto path = request.folder.nativePath();
    if (!path || !request.state->helper) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "Open Terminal Here requires a native folder and xdg-terminal-exec."
      );
      return;
    }
    GError* error = nullptr;
    const GioPtr<GFileInfo> info(g_file_query_info(
        request.folder.file().get(), G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
        G_FILE_QUERY_INFO_NONE, cancellable, &error
    ));
    if (!info) {
      g_task_return_error(task, error);
      return;
    }
    if (g_file_info_get_file_type(info.get()) != G_FILE_TYPE_DIRECTORY
        || (g_file_info_has_attribute(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)
            && !g_file_info_get_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))) {
      g_task_return_new_error(
          task, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "The terminal location is not an accessible folder."
      );
      return;
    }
    if (g_task_return_error_if_cancelled(task)) {
      return;
    }
    const GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE));
    g_subprocess_launcher_set_cwd(launcher.get(), path->c_str());
    const std::string directory = "--dir=" + *path;
    const char* arguments[]{request.state->helper.get(), directory.c_str(), nullptr};
    auto* process = g_subprocess_launcher_spawnv(launcher.get(), arguments, &error);
    if (!process) {
      g_task_return_error(task, error);
    } else {
      g_task_return_pointer(task, process, g_object_unref);
    }
  });
  g_object_unref(task);
}
