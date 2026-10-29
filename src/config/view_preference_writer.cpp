#include "config/view_preference_writer.h"

#include "fs/gio_ptr.h"

#include <algorithm>
#include <gio/gio.h>
#include <utility>

struct files::ViewPreferenceWriter::State : std::enable_shared_from_this<State> {
  struct Request {
    std::shared_ptr<State> owner;
    std::vector<OverrideEdit> edits;
  };
  ConfigPaths paths;
  std::vector<OverrideEdit> pending;
  std::function<void(const std::string&)> changed;
  bool active = false;

  void pump() {
    if (active || pending.empty()) {
      return;
    }
    auto edits = std::exchange(pending, {});
    active = true;
    GioPtr<GTask> task(g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) {
          auto* task = G_TASK(result);
          const auto owner = static_cast<Request*>(g_task_get_task_data(task))->owner;
          const GString message(static_cast<char*>(g_task_propagate_pointer(task, nullptr)));
          owner->active = false;
          owner->pump();
          const auto changed = owner->changed;
          if (changed) {
            changed(message ? message.get() : "");
          }
        },
        nullptr
    ));
    g_task_set_task_data(task.get(), new Request{shared_from_this(), std::move(edits)}, [](gpointer data) {
      delete static_cast<Request*>(data);
    });
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
      const auto& request = *static_cast<Request*>(data);
      std::string message;
      try {
        ConfigService service(request.owner->paths);
        const auto outcome = service.saveOverrides(request.edits);
        if (outcome == AtomicWriteState::NOT_COMMITTED || !service.issues().empty()) {
          message = service.issues().empty() ? "Cannot save the view preference." : service.issues().front().format();
        }
      } catch (const std::exception& error) {
        message = error.what();
      }
      g_task_return_pointer(task, g_strdup(message.c_str()), g_free);
    });
  }

  void queue(OverrideEdit edit) {
    std::erase_if(pending, [&](const auto& previous) { return previous.preference == edit.preference; });
    pending.push_back(edit);
    pump();
  }
};

files::ViewPreferenceWriter::ViewPreferenceWriter(ConfigPaths paths) : m_state(std::make_shared<State>()) {
  m_state->paths = std::move(paths);
}
files::ViewPreferenceWriter::~ViewPreferenceWriter() { m_state->changed = {}; }
void files::ViewPreferenceWriter::save(ViewMode mode) { m_state->queue({BrowserPreference::VIEW_MODE, mode}); }
void files::ViewPreferenceWriter::saveGridIconSize(int size) {
  m_state->queue({BrowserPreference::GRID_ICON_SIZE, size});
}
void files::ViewPreferenceWriter::saveSidebarWidth(int width) {
  m_state->queue({BrowserPreference::SIDEBAR_WIDTH, width});
}
void files::ViewPreferenceWriter::saveShowSidebar(bool visible) {
  m_state->queue({BrowserPreference::SHOW_SIDEBAR, visible});
}
void files::ViewPreferenceWriter::saveShowHidden(bool visible) {
  m_state->queue({BrowserPreference::SHOW_HIDDEN, visible});
}
bool files::ViewPreferenceWriter::busy() const { return m_state->active || !m_state->pending.empty(); }
void files::ViewPreferenceWriter::setChangedCallback(std::function<void(const std::string&)> callback) {
  m_state->changed = std::move(callback);
}
