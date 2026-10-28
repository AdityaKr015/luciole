#include "platform/file_launcher.h"

struct files::FileLauncher::State {
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::function<void(const std::string&)> changed;
  std::size_t active = 0;
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
  ++m_state->active;
  g_app_info_launch_default_for_uri_async(
      location.uri().c_str(), nullptr, m_state->cancellable.get(),
      [](GObject*, GAsyncResult* result, gpointer data) {
        const std::unique_ptr<std::shared_ptr<State>> retained(static_cast<std::shared_ptr<State>*>(data));
        const auto& state = *retained;
        GError* error = nullptr;
        const bool launched = g_app_info_launch_default_for_uri_finish(result, &error);
        --state->active;
        const auto callback = state->changed;
        if (callback && !g_cancellable_is_cancelled(state->cancellable.get())) {
          callback(error != nullptr ? error->message : (launched ? "" : "Could not open this file."));
        }
        g_clear_error(&error);
      },
      new std::shared_ptr<State>(m_state)
  );
}
