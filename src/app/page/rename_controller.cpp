#include "app/page/rename_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/places.h"
#include "core/ui_phase.h"
#include "fs/filename.h"
#include "fs/gio_ptr.h"
#include "operations/rename_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/file_error_text.h"
#include "ui/controls/input.h"
#include "ui/controls/name_editor.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace {
  std::string renameErrorText(const std::optional<files::FileError>& error) {
    return error ? files::nameErrorText(*error) : std::string{};
  }
} // namespace

RenameController::RenameController(PageContext& page, files::RenameService& renamer, files::PinService& pins)
    : m_page(page), m_renamer(renamer), m_pins(pins) {}

bool RenameController::canFileRename(const files::DirectoryEntry& entry) const {
  return m_page.actionState(files::BrowserAction::Rename, m_page.requestFor({entry})).enabled;
}

void RenameController::openFileRename(files::DirectoryEntry target) {
  if (!canFileRename(target) || !m_page.targetsCurrent({target})) {
    m_page.actionError("This item cannot be renamed right now.");
    return;
  }
  auto& surface = m_page.surface();
  const float width = std::min(420.0F, static_cast<float>(surface.width()) - 32.0F);
  const bool validText = g_utf8_validate(target.name.data(), static_cast<gssize>(target.name.size()), nullptr);
  auto editor = std::make_unique<NameEditor>(
      width, target.type == files::FileType::Directory && !target.symlink ? "Rename folder" : "Rename file",
      validText ? target.name : "", validText ? "" : "Enter a new UTF-8 name for this item.", files::filenameProblem,
      [this](std::string name) { submitFileRename(std::move(name)); }, [this] { m_page.dismissMenu(); }
  );
  auto* control = editor.get();
  UiPhaseScope phase(UiPhase::Layout);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = editor->measure(surface.renderTarget().renderer(), constraints).height;
  const LayoutRect anchor{(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1};
  m_page.showPopup(std::move(editor), nullptr, anchor, width, height, false);
  if (m_page.menuSurface() != nullptr) {
    m_page.activeDialog().bind(ActiveDialog::Kind::FileRename, [this] {
      if (m_fileRenameTarget) {
        m_renamer.cancel();
        m_page.focusFileList();
      }
      m_fileRenameEditor = nullptr;
      m_fileRenameTarget.reset();
      m_fileRenamePreparing = false;
    });
    m_fileRenameEditor = control;
    m_fileRenameTarget = std::move(target);
    m_fileRenamePreparing = true;
    control->setSaving(true);
    if (!m_renamer.prepare(m_fileRenameTarget->location)) {
      control->showError("Another rename is still finishing.");
    }
  }
}

void RenameController::submitFileRename(std::string name) {
  if (m_fileRenameEditor == nullptr || !m_fileRenameTarget) {
    return;
  }
  if (!m_page.targetsCurrent({*m_fileRenameTarget})) {
    m_fileRenameEditor->showError("The item changed. Close this editor and try again.");
    return;
  }
  if (!m_renamer.rename(std::move(name))) {
    m_fileRenameEditor->showError(
        m_renamer.snapshot().error ? files::nameErrorText(*m_renamer.snapshot().error)
                                   : "The item is unavailable. Reopen Rename."
    );
  } else {
    m_fileRenameEditor->setSaving(true);
  }
}

void RenameController::onFrame() {
  if (std::exchange(m_dirty, false)) {
    syncFileRename();
  }
}

void RenameController::syncFileRename() {
  const auto& snapshot = m_renamer.snapshot();
  if (m_renamer.busy()) {
    return;
  }
  if (snapshot.status == files::RenameStatus::Succeeded) {
    m_page.setActionMessage({});
    auto& browser = m_page.browser();
    const auto& current = browser.model().snapshot().location;
    if (current && snapshot.source && snapshot.source->isInside(*current)) {
      std::vector<files::Location> reveal;
      if (snapshot.destination) {
        reveal.push_back(*snapshot.destination);
      }
      m_page.revealOnReload(std::move(reveal));
      browser.refresh();
    }
    if (m_fileRenameEditor != nullptr) {
      m_fileRenameEditor = nullptr;
      m_page.dismissMenu();
    }
  } else if (m_fileRenameEditor == nullptr) {
    if (snapshot.status == files::RenameStatus::Error) {
      m_page.actionError("Could not rename: " + renameErrorText(snapshot.error));
    }
  } else if (snapshot.status == files::RenameStatus::Ready || snapshot.status == files::RenameStatus::Error) {
    m_fileRenameEditor->setSaving(false);
    m_fileRenameEditor->showError(renameErrorText(snapshot.error));
    if (std::exchange(m_fileRenamePreparing, false)) {
      m_page.menuDispatcher().setFocus(m_fileRenameEditor->input()->inputArea());
      const auto& value = m_fileRenameEditor->input()->value();
      const bool directory = m_fileRenameTarget->type == files::FileType::Directory && !m_fileRenameTarget->symlink;
      m_fileRenameEditor->input()->selectRange(0, files::filenameStemLength(value, directory));
    }
    m_page.menuSurface()->requestLayout();
  }
}

void RenameController::openPlaceRename(const files::Bookmark& bookmark) {
  if (bookmark.location.isTrash()) {
    return;
  }
  const auto found = std::ranges::find(m_pins.bookmarks(), bookmark.location, &files::Bookmark::location);
  if (!m_pins.editable() || found == m_pins.bookmarks().end()) {
    m_page.actionError("This place is unavailable for editing. Reload Places and try again.");
    return;
  }
  const auto current = *found; // NOLINT(performance-unnecessary-copy-initialization): own the editing snapshot.
  auto& surface = m_page.surface();
  const float width = std::min(440.0F, static_cast<float>(surface.width()) - 32.0F);
  auto editor = std::make_unique<NameEditor>(
      width, "Rename place", current.label, "Changes the sidebar label only.",
      [](const std::string& label) -> std::optional<std::string> {
        if (const auto error = files::bookmarkLabelProblem(label)) {
          return error;
        }
        if (label.find_first_not_of(' ') == std::string::npos) {
          return "Enter a label.";
        }
        return std::nullopt;
      },
      [this](std::string label) { submitPlaceRename(std::move(label)); }, [this] { m_page.dismissMenu(); }
  );
  auto* control = editor.get();
  UiPhaseScope phase(UiPhase::Layout);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = std::min(
      editor->measure(surface.renderTarget().renderer(), constraints).height,
      static_cast<float>(surface.height()) - 64.0F
  );
  m_page.showPopup(
      std::move(editor), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false
  );
  if (m_page.menuSurface() != nullptr) {
    m_page.activeDialog().bind(ActiveDialog::Kind::PlaceRename, [this] {
      if (m_placeRenameTarget) {
        m_page.restorePlaceFocus(m_placeRenameTarget->location);
      }
      m_placeEditor = nullptr;
      m_placeRenameTarget.reset();
      m_pendingPlaceLabel.reset();
    });
    m_placeEditor = control;
    m_placeRenameTarget = current;
    m_page.menuDispatcher().setFocus(control->input()->inputArea());
    control->input()->selectAll();
  }
}

void RenameController::submitPlaceRename(std::string label) {
  if (m_placeEditor == nullptr || !m_placeRenameTarget) {
    return;
  }
  m_pendingPlaceLabel = label;
  m_placeEditor->setSaving(true);
  if (!m_pins.rename(*m_placeRenameTarget, std::move(label))) {
    m_pendingPlaceLabel.reset();
    m_placeEditor->setSaving(false);
    m_placeEditor->showError(
        m_pins.busy() ? "Places are being saved. Try again shortly."
                      : "This place changed. Close this dialog and try again."
    );
    return;
  }
  syncPlaceRename();
}

void RenameController::syncPlaceRename() {
  if (m_placeEditor == nullptr || !m_pendingPlaceLabel || m_pins.busy()) {
    return;
  }
  const auto found = std::ranges::find(m_pins.bookmarks(), m_placeRenameTarget->location, &files::Bookmark::location);
  if (found != m_pins.bookmarks().end() && found->label == *m_pendingPlaceLabel) {
    m_page.dismissMenu();
  } else {
    m_placeEditor->setSaving(false);
    m_placeEditor->showError(
        m_pins.error().empty() ? "This place changed. Close this dialog and try again."
                               : "Could not save the label. Close this dialog and try again."
    );
  }
  m_pendingPlaceLabel.reset();
}
