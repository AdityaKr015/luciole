#pragma once

#include "browser/chooser_policy.h"
#include "fs/file_filter.h"
#include "platform/file_chooser_portal.h"
#include "ui/controls/select_dropdown_popup.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class BrowserPage;
class Button;
class Label;
class Node;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;

namespace luciole {
  class WindowSurface;
}

// One FileChooser portal request shown in a chooser window: the page's chooser hooks, its filters and the footer
// with the application's choices and the Cancel and accept buttons. The call is answered exactly once, by
// accepting, cancelling or closing the window.
class FileChooserSession {
public:
  FileChooserSession(
      luciole::FileChooserRequest request, std::shared_ptr<luciole::FileChooserCall> call,
      WaylandConnection& connection, RenderContext& renderer
  );
  ~FileChooserSession();
  FileChooserSession(const FileChooserSession&) = delete;
  FileChooserSession& operator=(const FileChooserSession&) = delete;

  const luciole::FileChooserRequest& request() const { return m_request; }
  std::string windowTitle() const;
  // Installs the page hooks and filters and returns the footer; `close` closes the window.
  std::unique_ptr<Node> attach(BrowserPage& page, luciole::WindowSurface& surface, std::function<void()> close);
  // Once per frame: the accept button's state and the reason it is unavailable.
  void sync();
  // True when the event belonged to the open filter or choice dropdown.
  bool pointer(const PointerEvent& event);
  bool keyboard(const KeyboardEvent& event);
  // The window closed without an answer: the request is cancelled.
  void windowClosed();

private:
  luciole::ChooserMode mode() const;
  luciole::ChooserDecision decision(const std::vector<luciole::DirectoryEntry>& entries) const;
  void selectFilter(std::size_t index);
  void apply(const luciole::ChooserDecision& decision);
  void cancel();

  luciole::FileChooserRequest m_request;
  std::shared_ptr<luciole::FileChooserCall> m_call;
  WaylandConnection& m_connection;
  SelectDropdownPopup m_dropdown;
  wl_output* m_dropdownOutput = nullptr;
  BrowserPage* m_page = nullptr;
  luciole::WindowSurface* m_surface = nullptr;
  std::function<void()> m_close;
  std::vector<luciole::FileChooserFilter> m_filters;
  std::size_t m_activeFilter = 0;
  std::optional<luciole::FileFilter> m_filter;
  // Choice ID and current value, in request order.
  std::vector<std::pair<std::string, std::string>> m_choices;
  Button* m_accept = nullptr;
  Label* m_reason = nullptr;
};
