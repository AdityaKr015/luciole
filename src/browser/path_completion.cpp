#include "browser/path_completion.h"

#include <algorithm>
#include <exception>
#include <stdexcept>

struct files::PathCompletion::State {
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::uint64_t revision = 0;
  bool active = true;
};
files::PathCompletion::PathCompletion() : m_state(std::make_shared<State>()) {}
files::PathCompletion::~PathCompletion() {
  m_state->active = false;
  cancel();
}
void files::PathCompletion::cancel() {
  ++m_state->revision;
  g_cancellable_cancel(m_state->cancellable.get());
  m_state->cancellable.reset(g_cancellable_new());
}
void files::PathCompletion::complete(
    const std::string& text, const Location& base, bool showHidden, std::function<void(PathSuggestions)> completed
) {
  try {
    if (text.empty()) {
      cancel();
      completed({});
      return;
    }
    if (text.ends_with('/') || text == "~") {
      enumerate(resolveLocationText(text, base), {}, showHidden, std::move(completed));
      return;
    }
    const auto slash = text.find_last_of('/');
    const auto parent = slash == std::string::npos ? base : resolveLocationText(text.substr(0, slash + 1), base);
    std::string prefix = slash == std::string::npos ? text : text.substr(slash + 1);
    if (const GString scheme(g_uri_parse_scheme(text.c_str())); scheme) {
      const GString decoded(g_uri_unescape_string(prefix.c_str(), "/"));
      if (!decoded)
        throw std::invalid_argument("Invalid URI filename prefix.");
      prefix = decoded.get();
    }
    enumerate(parent, prefix, showHidden || prefix.starts_with('.'), std::move(completed));
  } catch (const std::exception& error) {
    cancel();
    completed({.items = {}, .error = error.what()});
  }
}
void files::PathCompletion::children(
    const Location& parent, bool showHidden, std::function<void(PathSuggestions)> completed
) {
  enumerate(parent, {}, showHidden, std::move(completed));
}
void files::PathCompletion::enumerate(
    const Location& parent, std::string prefix, bool showHidden, std::function<void(PathSuggestions)> completed
) {
  cancel();
  struct Request {
    std::shared_ptr<State> state;
    std::uint64_t revision;
    Location parent;
    std::string prefix;
    bool showHidden;
    std::function<void(PathSuggestions)> completed;
    PathSuggestions result;
  };
  GTask* task = g_task_new(
      nullptr, m_state->cancellable.get(),
      [](GObject*, GAsyncResult* result, gpointer) {
        auto& request = *static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
        if (request.state->active && request.state->revision == request.revision)
          request.completed(std::move(request.result));
      },
      nullptr
  );
  g_task_set_task_data(
      task, new Request{m_state, m_state->revision, parent, std::move(prefix), showHidden, std::move(completed), {}},
      [](gpointer data) { delete static_cast<Request*>(data); }
  );
  g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer data, GCancellable* cancellable) {
    auto& request = *static_cast<Request*>(data);
    GError* error = nullptr;
    const GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children(
        request.parent.file().get(),
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
                                       "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
        G_FILE_QUERY_INFO_NONE, cancellable, &error
    ));
    constexpr std::size_t limit = 256;
    while (enumerator && !g_cancellable_is_cancelled(cancellable)) {
      const GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), cancellable, &error));
      if (!info)
        break;
      const char* name = g_file_info_get_name(info.get());
      if (g_file_info_get_file_type(info.get()) != G_FILE_TYPE_DIRECTORY
          || (!request.showHidden && g_file_info_get_is_hidden(info.get()))
          || !std::string_view(name).starts_with(request.prefix))
        continue;
      if (request.result.items.size() == limit) {
        request.result.truncated = true;
        break;
      }
      const GioPtr<GFile> child(g_file_get_child(request.parent.file().get(), name));
      const GString uri(g_file_get_uri(child.get()));
      auto location = Location::fromUri(uri.get());
      const char* display = g_file_info_get_display_name(info.get());
      auto text = locationEditText(location);
      if (!text.ends_with('/'))
        text += '/';
      request.result.items.push_back({std::move(location), display ? display : name, std::move(text)});
    }
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      request.result.error = error->message;
    g_clear_error(&error);
    if (enumerator)
      g_file_enumerator_close(enumerator.get(), cancellable, nullptr);
    std::ranges::sort(request.result.items, [](const auto& a, const auto& b) {
      return g_utf8_collate(a.label.c_str(), b.label.c_str()) < 0;
    });
    g_task_return_boolean(task, true);
  });
  g_object_unref(task);
}
