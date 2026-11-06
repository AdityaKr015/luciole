#include "operations/template_catalog.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

files::TemplateRevision files::templateRevision(const struct stat& info) {
  if (!S_ISREG(info.st_mode) || info.st_size < 0) {
    throw std::runtime_error("The template is not a regular file.");
  }
  return {
      static_cast<std::uint64_t>(info.st_dev),
      static_cast<std::uint64_t>(info.st_ino),
      static_cast<std::uint64_t>(info.st_size),
      info.st_mtim.tv_sec,
      info.st_mtim.tv_nsec,
      info.st_ctim.tv_sec,
      info.st_ctim.tv_nsec
  };
}
struct files::TemplateCatalog::State {
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::uint64_t revision = 0;
  bool active = true;
};
files::TemplateCatalog::TemplateCatalog() : m_state(std::make_shared<State>()) {}
files::TemplateCatalog::~TemplateCatalog() {
  m_state->active = false;
  cancel();
}
void files::TemplateCatalog::cancel() {
  ++m_state->revision;
  g_cancellable_cancel(m_state->cancellable.get());
  m_state->cancellable.reset(g_cancellable_new());
}
void files::TemplateCatalog::load(std::function<void(TemplateListing)> completed) {
  cancel();
  struct Request {
    std::shared_ptr<State> owner;
    std::uint64_t revision;
    std::function<void(TemplateListing)> completed;
    TemplateListing result;
  };
  const GioPtr<GTask> task(g_task_new(
      nullptr, m_state->cancellable.get(),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& request = *static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
        if (request.owner->active && request.owner->revision == request.revision) {
          request.completed(std::move(request.result));
        }
      },
      nullptr
  ));
  g_task_set_task_data(
      task.get(), new Request{m_state, m_state->revision, std::move(completed), {}},
      [](gpointer data) { delete static_cast<Request*>(data); }
  );
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
    auto& result = static_cast<Request*>(data)->result;
    try {
      const char* configured = g_get_user_special_dir(G_USER_DIRECTORY_TEMPLATES);
      if (!configured || !g_path_is_absolute(configured)) {
        throw std::runtime_error("No Templates folder is configured.");
      }
      struct stat root{}, home{};
      if (::stat(configured, &root) != 0 || !S_ISDIR(root.st_mode)) {
        throw std::runtime_error("The Templates folder is unavailable.");
      }
      if (::stat(g_get_home_dir(), &home) == 0 && root.st_dev == home.st_dev && root.st_ino == home.st_ino) {
        throw std::runtime_error("The Templates folder is disabled.");
      }
      namespace fs = std::filesystem;
      const fs::path base(configured);
      std::error_code error;
      fs::recursive_directory_iterator it(base, error), end;
      std::size_t visited = 0;
      while (!error && it != end && !g_cancellable_is_cancelled(cancellable)) {
        if (++visited > 4096 || result.items.size() >= 256) {
          result.truncated = true;
          break;
        }
        const auto path = it->path();
        const auto name = path.filename().native();
        if (name.starts_with('.')) {
          it.disable_recursion_pending();
        } else {
          struct stat info{};
          if (::lstat(path.c_str(), &info) != 0) {
            throw std::runtime_error("Some templates could not be read.");
          }
          if (S_ISDIR(info.st_mode) && it.depth() >= 8) {
            it.disable_recursion_pending();
            result.truncated = true;
          } else if (S_ISREG(info.st_mode)) {
            const GString label(g_filename_display_name(path.lexically_relative(base).c_str()));
            result.items.push_back({Location::fromPath(path.native()), name, label.get(), templateRevision(info)});
          }
        }
        it.increment(error);
      }
      if (error) {
        result.error = "Some templates could not be read: " + error.message();
      }
      std::ranges::sort(result.items, [](const auto& a, const auto& b) {
        const int order = g_utf8_collate(a.label.c_str(), b.label.c_str());
        return order != 0 ? order < 0 : a.location.uri() < b.location.uri();
      });
    } catch (const std::exception& error) {
      result.error = error.what();
    }
    g_task_return_boolean(task, true);
  });
}
