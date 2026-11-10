#include "app/page/create_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "app/page/submenu_controller.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "core/ui_phase.h"
#include "fs/filename.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/file_error_text.h"
#include "ui/controls/input.h"
#include "ui/dialogs/link_dialog.h"
#include "ui/dialogs/name_dialog.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
  std::string createErrorText(const std::optional<files::FileError>& error) {
    return error ? files::nameErrorText(*error, "The destination folder is no longer available.") : std::string{};
  }
} // namespace

CreateController::CreateController(PageContext& page, files::TemplateCatalog& templates, SubmenuController& submenus)
    : m_page(page), m_templates(templates), m_submenus(submenus) {}

bool CreateController::canCreateItem() const {
  return m_page.actionState(files::BrowserAction::CreateFolder, m_page.requestFor({})).enabled;
}

void CreateController::showDocumentMenu(
    const files::ActionRequest& request, files::TemplateListing listing, bool loading
) {
  if (!m_page.actionState(files::BrowserAction::NewDocument, request).enabled) {
    return;
  }
  auto items = std::make_shared<std::vector<files::DocumentTemplate>>(std::move(listing.items));
  std::vector<ContextMenuControlEntry> entries{{.id = -1, .label = "Empty File…"}, {.label = "", .separator = true}};
  for (std::size_t index = 0; index < items->size(); ++index) {
    entries.push_back({.id = static_cast<int>(index), .label = (*items)[index].label});
  }
  if (loading || items->empty() || listing.error) {
    entries.push_back(
        {.label = loading    ? "Loading templates…"
             : listing.error ? files::fileErrorText(*listing.error)
                             : "No document templates",
         .enabled = false}
    );
  }
  if (listing.truncated) {
    entries.push_back({.label = "Template list limit reached", .enabled = false});
  }
  m_submenus.show(
      std::move(entries),
      [this, request, items](int id) {
        if (!m_page.actionState(files::BrowserAction::NewDocument, request).enabled) {
          return;
        }
        if (id == -1) {
          openCreateItem(*request.folder, files::CreateKind::EmptyFile);
        } else if (id >= 0 && static_cast<std::size_t>(id) < items->size()) {
          openCreateItem(*request.folder, files::CreateKind::Document, (*items)[static_cast<std::size_t>(id)]);
        }
      },
      300.0F
  );
}

void CreateController::openDocumentMenu(const files::ActionRequest& request) {
  if (!m_page.actionState(files::BrowserAction::NewDocument, request).enabled) {
    return;
  }
  showDocumentMenu(request, {}, true);
  const auto generation = m_submenus.generation();
  m_templates.load([this, request, generation](files::TemplateListing listing) {
    if (generation == m_submenus.generation()) {
      showDocumentMenu(request, std::move(listing));
    }
  });
}

void CreateController::openCreateItem(
    files::Location parent, files::CreateKind kind, std::optional<files::DocumentTemplate> source
) {
  if (!canCreateItem() || m_page.browser().model().snapshot().location != parent) {
    m_page.actionError("Open an available writable folder to create a file or folder.");
    return;
  }
  auto& surface = m_page.surface();
  const float width = std::min(420.0F, static_cast<float>(surface.width()) - 32.0F);
  const bool document = kind == files::CreateKind::Document;
  const auto display = source ? files::filenameDisplayText(source->name) : std::string{};
  auto editor = std::make_unique<NameDialog>(
      width,
      kind == files::CreateKind::Folder ? "New folder"
          : document                    ? "New document"
                                        : "New file",
      kind == files::CreateKind::Folder ? "folder-plus" : "file-plus",
      source                                  ? display
          : kind == files::CreateKind::Folder ? "New Folder"
                                              : "New File",
      source ? "From " + source->label : "", files::filenameProblem,
      [this](std::string name) { submitCreateItem(std::move(name)); }, [this] { m_page.dismissMenu(); }, "Create",
      "Creating…"
  );
  auto* control = editor.get();
  UiPhaseScope phase(UiPhase::Layout);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = editor->measure(surface.renderTarget().renderer(), constraints).height;
  m_page.showPopup(
      std::move(editor), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false
  );
  if (m_page.menuSurface() != nullptr) {
    m_page.activeDialog().bind(ActiveDialog::Kind::CreateItem, [this] { closeCreation(); });
    m_createEditor = control;
    m_createParent = std::move(parent);
    m_creationPreparing = true;
    control->setSaving(true);
    m_job = m_page.jobs().create<files::CreateService>(JobScope::Page);
    auto& creator = m_job->service<files::CreateService>();
    if (!(source ? creator.prepareTemplate(*m_createParent, std::move(*source))
                 : creator.prepare(*m_createParent, kind))) {
      control->showError("Another creation is still finishing.");
    }
  }
}

void CreateController::submitCreateItem(std::string name) {
  if (m_createEditor == nullptr || !m_createParent || !m_job) {
    return;
  }
  auto& browser = m_page.browser();
  if (browser.model().snapshot().location != m_createParent || !browser.searchQuery().empty()) {
    m_createEditor->showError("The destination changed. Close this dialog and try again.");
    return;
  }
  auto& creator = m_job->service<files::CreateService>();
  if (creator.create(std::move(name))) {
    m_page.jobs().follow(*m_job);
    m_createEditor->setSaving(true);
  } else {
    m_createEditor->showError(
        creator.snapshot().error ? createErrorText(creator.snapshot().error)
                                 : "The destination is unavailable. Close this dialog and try again."
    );
  }
}

void CreateController::openLinkDialog(const files::DirectoryEntry& target) {
  if (!m_page.actionState(files::BrowserAction::CreateLink, m_page.requestFor({target})).enabled
      || !m_page.targetsCurrent({target})) {
    m_page.actionError("This item cannot be linked right now.");
    return;
  }
  const auto parent = target.location.parent();
  if (!parent) {
    m_page.actionError("The source has no containing folder.");
    return;
  }
  const auto& base = *parent;
  auto& surface = m_page.surface();
  const float width = std::min(480.0F, static_cast<float>(surface.width()) - 32.0F);
  auto root = std::make_unique<LinkDialog>(
      width, files::locationEditText(target.location), "Link to " + target.displayName, files::locationEditText(base),
      target.type == files::FileType::Regular && !target.symlink,
      [this, base](std::string name, const std::string& path, files::CreateKind kind) {
        if (!m_linkDialog || !m_job) {
          return;
        }
        try {
          const auto parent = files::resolveLocationText(path, base);
          auto& creator = m_job->service<files::CreateService>();
          if (creator.createLink(parent, std::move(name), kind)) {
            m_page.jobs().follow(*m_job);
            m_linkDialog->setSaving(true);
          } else {
            m_linkDialog->showError(
                creator.snapshot().error ? createErrorText(creator.snapshot().error)
                                         : "The source is unavailable. Close this dialog and try again."
            );
          }
        } catch (const std::exception& error) {
          m_linkDialog->showError(error.what());
        }
        m_page.menuSurface()->requestLayout();
      },
      [this] { m_page.dismissMenu(); }
  );
  auto* dialog = root.get();
  UiPhaseScope phase(UiPhase::Layout);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = std::min(
      root->measure(surface.renderTarget().renderer(), constraints).height, static_cast<float>(surface.height()) - 64.0F
  );
  m_page.showPopup(
      std::move(root), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false
  );
  if (m_page.menuSurface()) {
    m_page.activeDialog().bind(ActiveDialog::Kind::Link, [this] { closeCreation(); });
    m_linkDialog = dialog;
    m_linkPreparing = true;
    m_page.setDialogSize(width, height);
    m_page.menuDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
      if (m_linkDialog) {
        m_linkDialog->reveal(focused);
        m_page.menuSurface()->requestLayout();
      }
    });
    dialog->setSaving(true);
    m_job = m_page.jobs().create<files::CreateService>(JobScope::Page);
    if (!m_job->service<files::CreateService>().prepareLink(target)) {
      dialog->setSaving(false);
      dialog->showError("Another creation is still finishing.");
    }
  }
}

void CreateController::changed(OperationJob& job) {
  if (&job == m_job.get()) {
    m_dirty = true;
  } else if (!job.running() && job.takeResult()) {
    // A job whose dialog closed.
    sync(job);
  }
}

void CreateController::onFrame() {
  if (std::exchange(m_dirty, false) && m_job) {
    sync(*m_job);
  }
}

void CreateController::sync(const OperationJob& job) {
  const auto& creator = job.service<files::CreateService>();
  if (creator.busy()) {
    return;
  }
  const bool dialog = &job == m_job.get();
  auto* createEditor = dialog ? m_createEditor : nullptr;
  auto* linkDialog = dialog ? m_linkDialog : nullptr;
  const auto& snapshot = creator.snapshot();
  if (snapshot.status == files::CreateStatus::Succeeded || snapshot.status == files::CreateStatus::Partial) {
    auto message = createErrorText(snapshot.error);
    if (linkDialog && !snapshot.error && snapshot.parent) {
      message = "Link created in " + files::locationEditText(*snapshot.parent);
    }
    m_page.setActionMessage(std::move(message));
    auto& browser = m_page.browser();
    if (browser.model().snapshot().location == snapshot.parent) {
      std::vector<files::Location> reveal;
      if (snapshot.destination) {
        reveal.push_back(*snapshot.destination);
      }
      m_page.revealOnReload(std::move(reveal));
      browser.refresh();
    }
    if (createEditor != nullptr || linkDialog != nullptr) {
      m_createEditor = nullptr;
      m_page.dismissMenu();
    }
  } else if (linkDialog) {
    linkDialog->setSaving(false);
    linkDialog->showError(createErrorText(snapshot.error));
    if (std::exchange(m_linkPreparing, false)) {
      m_page.menuDispatcher().setFocus(linkDialog->nameInput()->inputArea());
      linkDialog->nameInput()->selectAll();
    }
    m_page.menuSurface()->requestLayout();
  } else if (createEditor == nullptr) {
    if (snapshot.status == files::CreateStatus::Error) {
      m_page.actionError("Could not create: " + createErrorText(snapshot.error));
    }
  } else if (snapshot.status == files::CreateStatus::Ready || snapshot.status == files::CreateStatus::Error) {
    createEditor->setSaving(false);
    createEditor->showError(createErrorText(snapshot.error));
    if (std::exchange(m_creationPreparing, false)) {
      m_page.menuDispatcher().setFocus(createEditor->input()->inputArea());
      if (snapshot.kind == files::CreateKind::Document) {
        const auto& value = createEditor->input()->value();
        createEditor->input()->selectRange(0, files::filenameStemLength(value, false));
      } else {
        createEditor->input()->selectAll();
      }
    }
    m_page.menuSurface()->requestLayout();
  }
}

void CreateController::closeCreation() {
  if (m_createParent || m_linkDialog) {
    // Creating goes on for the page that takes the job over once this page left its window.
    if (m_job && (m_page.jobs().attached() || !m_job->running())) {
      m_job->cancel();
    }
    m_page.focusFileList();
  }
  m_job.reset();
  m_createEditor = nullptr;
  m_linkDialog = nullptr;
  m_linkPreparing = false;
  m_createParent.reset();
  m_creationPreparing = false;
}
