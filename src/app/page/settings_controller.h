#pragma once

#include "config/settings.h"
#include "render/scene/node.h"
#include "ui/controls/select_popup_context.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace files {
  class ThumbnailService;
  class ViewPreferenceWriter;
} // namespace files

struct KeyboardEvent;
class ArchiveController;
class BrowserSettingsView;
class InputArea;
class InputDispatcher;
class MenuController;
class PageContext;
class Renderer;

// Settings: the full-page settings view, its dropdowns, and applying saved preference changes to the page.
class SettingsController final : public SelectPopupContext {
public:
  SettingsController(
      PageContext& page, files::ViewPreferenceWriter& preferences, files::ThumbnailService& thumbnails,
      InputDispatcher& dispatcher, ArchiveController& archive, MenuController& menus,
      std::function<void()> clearThumbnails
  );

  // Opens Settings, or closes it when it is already open.
  void toggle();
  // Removes the settings view now.
  void close();
  // True while the settings view is shown.
  bool open() const { return m_view != nullptr; }
  // Preferences were saved or reloaded; `error` is empty when that worked.
  void preferencesChanged(const std::string& error);
  // Thumbnail cache state changed: sync on the next frame.
  void markDirty() { m_dirty = true; }
  // The last preference load or save error; empty when there is none.
  const std::string& error() const { return m_error; }
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // Keyboard hook in BrowserPage::keyboard(): handles every key while Settings is shown.
  bool key(const KeyboardEvent& event);
  // Layout hook in BrowserPage::prepareFrame().
  void layout(Renderer& renderer, LayoutRect bounds, bool compact);
  // Focus hook of the page dispatcher: scrolls the focused control into view.
  void revealFocus(InputArea* focused);

  void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override;
  void closeSelectDropdown() override;
  bool isSelectDropdownOpen() const override;

private:
  void deferClose();
  void sync();

  PageContext& m_page;
  files::ViewPreferenceWriter& m_preferences;
  files::ThumbnailService& m_thumbnails;
  InputDispatcher& m_dispatcher;
  ArchiveController& m_archive;
  MenuController& m_menus;
  std::function<void()> m_clearThumbnails;
  BrowserSettingsView* m_view = nullptr;
  std::optional<files::Settings> m_pending;
  std::string m_error;
  bool m_dirty = false;
  // Lives while the view is shown; dropdown callbacks check it.
  std::shared_ptr<int> m_viewLifetime;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
