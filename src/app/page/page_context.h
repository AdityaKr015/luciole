#pragma once

#include "browser/action_registry.h"
#include "render/scene/node.h"

#include <memory>
#include <string>
#include <vector>

namespace files {
  class BrowserController;
  class WaylandTextClipboard;
  class WindowSurface;
  struct DirectoryEntry;
} // namespace files

class ActiveDialog;
class ContextMenuControl;
class Flex;
class InputDispatcher;
class PopupSurface;

// The part of BrowserPage that feature controllers use. BrowserPage implements it.
//
// A controller (`page/<feature>_controller.{h,cpp}`) owns one feature of the page:
// - its widget pointers, dirty flag and feature state;
// - references to the services it drives, which BrowserPage still owns and passes to its constructor;
// - its open*/submit* entry points and sync(), which applies the service snapshot to its widgets.
//
// Frame order: the service's changed callback (BrowserPage constructor) calls the controller's markDirty().
// BrowserPage::prepareFrame() calls onFrame() at the feature's fixed slot in the dirty -> sync sequence, and
// onFrame() runs sync() only when the flag was set. Keep that slot so syncs run in the same order every frame.
//
// Dialogs: show the root with showPopup(). If menuSurface() exists afterwards, keep the widget pointers and
// call activeDialog().bind(kind, onClose). onClose is the only teardown path: closeMenu() runs it whatever
// closed the popup, and it cancels the job and drops the pointers.
//
// Input, layout and scene hooks that the page cannot route generically yet are small controller methods the
// page calls at the original spot.
class PageContext {
public:
  virtual files::BrowserController& browser() = 0;
  virtual files::ActionRequest requestFor(const std::vector<files::DirectoryEntry>& entries) const = 0;
  virtual files::ActionState actionState(files::BrowserAction action, const files::ActionRequest& request) const = 0;
  // True while every target still matches the current folder listing.
  virtual bool targetsCurrent(const std::vector<files::DirectoryEntry>& targets) = 0;
  // Shows `message` in the status bar.
  virtual void actionError(std::string message) = 0;
  virtual files::WaylandTextClipboard& clipboard() = 0;
  virtual files::WindowSurface& surface() = 0;
  // Gives keyboard focus back to the file list, if there is one.
  virtual void focusFileList() = 0;

  // Replaces the active dialog with `root` on a new popup surface anchored at `anchor`.
  virtual void showPopup(
      std::unique_ptr<Flex> root, ContextMenuControl* menu, LayoutRect anchor, float width, float height, bool alignLeft
  ) = 0;
  // Re-measures and re-centers the popup on every frame, at most `width` wide; `height` is the current height.
  virtual void setDialogSize(float width, float height) = 0;
  // Closes the active dialog on the next main loop iteration.
  virtual void dismissMenu() = 0;
  // Null while no popup is shown.
  virtual PopupSurface* menuSurface() = 0;
  virtual InputDispatcher& menuDispatcher() = 0;
  virtual ActiveDialog& activeDialog() = 0;

protected:
  PageContext() = default;
  PageContext(const PageContext&) = delete;
  PageContext& operator=(const PageContext&) = delete;
  ~PageContext() = default;
};
