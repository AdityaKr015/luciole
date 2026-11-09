#pragma once

#include "fs/location.h"
#include "render/scene/node.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace files {
  class FileIconCache;
  class FileLauncher;
  struct DirectoryEntry;
} // namespace files

struct KeyboardEvent;
class ArchiveView;
class InputDispatcher;
class OperationJob;
class PageContext;
class Renderer;
class WaylandConnection;

// Archives: the compress and extract dialogs, extracting next to the archive, and the full-page archive browser.
class ArchiveController {
public:
  ArchiveController(
      PageContext& page, files::FileLauncher& launcher, files::FileIconCache& icons, WaylandConnection& connection,
      InputDispatcher& dispatcher
  );

  void compress(const std::vector<files::DirectoryEntry>& targets);
  void extractTo(const files::Location& archive);
  void extractHere(const files::Location& archive);
  void browse(const files::Location& archive);
  // Owned archive job change: opens an extracted member, reports a finished operation and syncs the archive
  // browser on the next frame.
  void changed(OperationJob& job);
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // Removes the archive browser now.
  void close();
  // True while the archive browser overlay is shown.
  bool open() const { return m_view != nullptr; }
  // Keyboard hook in BrowserPage::keyboard(): handles every key while the archive browser is shown.
  bool key(const KeyboardEvent& event);
  // Layout hook in BrowserPage::prepareFrame().
  void layout(Renderer& renderer, LayoutRect bounds);

private:
  static files::Location destination(const files::Location& source);
  void openDialog(std::vector<files::Location> sources, std::vector<std::string> selected, bool compress);
  bool extract(const files::Location& source, const files::Location& destination, std::vector<std::string> selected);
  // The job the archive browser shows: the newest running archive job of the page, else its own.
  OperationJob* shown() const;
  void deferClose();
  void sync();
  void syncPassword();
  struct PasswordDialog {
    std::uint64_t job, request;
  };
  std::optional<PasswordDialog> m_passwordDialog;

  PageContext& m_page;
  // The archive browser's job: reads the catalog and opens members. Compress and extract jobs are separate.
  std::shared_ptr<OperationJob> m_browser;
  files::FileLauncher& m_launcher;
  files::FileIconCache& m_icons;
  WaylandConnection& m_connection;
  InputDispatcher& m_dispatcher;
  ArchiveView* m_view = nullptr;
  std::optional<files::Location> m_source;
  bool m_wanted = false;
  bool m_dirty = false;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
