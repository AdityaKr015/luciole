#include "app/page/create_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "fs/filename.h"
#include "platform/desktop_integration.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/file_error_text.h"
#include "ui/controls/input.h"
#include "ui/controls/link_dialog.h"
#include "ui/controls/name_editor.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {
  std::string createErrorText(const std::optional<files::FileError>& error) {
    return error ? files::nameErrorText(*error, "The destination folder is no longer available.") : std::string{};
  }
} // namespace

CreateController::CreateController(
    PageContext& page, files::TemplateCatalog& templates, WaylandConnection& connection, RenderContext& renderer,
    AnimationManager& animations
)
    : m_page(page), m_templates(templates), m_connection(connection), m_renderer(renderer), m_animations(animations) {}

bool CreateController::canCreateItem() const {
  return m_page.actionState(files::BrowserAction::CreateFolder, m_page.requestFor({})).enabled;
}

void CreateController::closeDocumentMenu() {
  if (auto* menu = m_page.contextMenu()) {
    menu->setSubmenuDirection(ContextSubmenuDirection::Right);
  }
  ++m_documentGeneration;
  m_closeDocumentPending = false;
  m_templates.cancel();
  m_documentDispatcher.setSceneRoot(nullptr);
  if (m_documentSurface) {
    m_documentSurface->setSceneRoot(nullptr);
  }
  m_documentRoot.reset();
  m_documentMenu = nullptr;
  m_documentScroll = nullptr;
  m_documentSurface.reset();
}

void CreateController::menuClosed() {
  closeDocumentMenu();
  m_documentRow.reset();
}

void CreateController::dismissDocumentMenu() {
  m_closeDocumentPending = true;
  const auto generation = m_documentGeneration;
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, lifetime, generation] {
    if (!lifetime.expired() && generation == m_documentGeneration && m_closeDocumentPending) {
      closeDocumentMenu();
    }
  });
}

void CreateController::fillDocumentMenu(
    const files::ActionRequest& request, files::TemplateListing listing, bool loading
) {
  if (!m_documentSurface || !m_page.actionState(files::BrowserAction::NewDocument, request).enabled) {
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
  const float contentHeight = ContextMenuControl::preferredHeight(entries, entries.size());
  const float height = std::min(contentHeight, static_cast<float>(m_page.surface().height()) - 32.0F);
  const float width = 300.0F;
  const bool overflowing = contentHeight > height;
  m_documentMenu->setMaxVisible(entries.size());
  m_documentMenu->setEntries(std::move(entries));
  m_documentMenu->setMenuWidth(width - (overflowing ? Style::scrollbarWidth + Style::scrollbarGap : 0.0F));
  m_documentMenu->setHighlightedIndex(m_documentKeyboard ? 0 : m_documentMenu->entryCount());
  m_documentMenu->setOnActivate([this, request, items](const ContextMenuControlEntry& entry) {
    const auto id = entry.id;
    const std::weak_ptr<int> lifetime = m_lifetime;
    m_page.dismissMenu();
    DeferredCall::callLater([this, lifetime, request, items, id] {
      if (lifetime.expired() || !m_page.actionState(files::BrowserAction::NewDocument, request).enabled) {
        return;
      }
      if (id == -1) {
        openCreateItem(*request.folder, files::CreateKind::EmptyFile);
      } else if (id >= 0 && static_cast<std::size_t>(id) < items->size()) {
        openCreateItem(*request.folder, files::CreateKind::Document, (*items)[static_cast<std::size_t>(id)]);
      }
    });
  });
  m_documentScroll->setReserveScrollbarGutter(overflowing);
  m_documentScroll->setSize(width, height);
  (void)m_documentSurface->resize(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(std::ceil(height)), false
  );
  m_documentSurface->requestLayout();
}

void CreateController::attachDocumentMenu(std::size_t row, const files::ActionRequest& request) {
  m_documentRow = row;
  m_page.contextMenu()->setOnSubmenuOpen([this, request](const ContextMenuControlEntry&, float) {
    openDocumentMenu(request);
  });
}

void CreateController::openDocumentMenu(const files::ActionRequest& request) {
  m_closeDocumentPending = false;
  auto* menuSurface = m_page.menuSurface();
  auto* contextMenu = m_page.contextMenu();
  if (m_documentSurface
      || !menuSurface
      || !contextMenu
      || !m_documentRow
      || !m_page.actionState(files::BrowserAction::NewDocument, request).enabled) {
    return;
  }
  const float top = contextMenu->rowTop(*m_documentRow);
  const float bottom = contextMenu->rowBottom(*m_documentRow);
  float menuX = 0, menuY = 0;
  Node::absolutePosition(contextMenu, menuX, menuY);
  m_documentSurface = std::make_unique<PopupSurface>(m_connection);
  m_documentSurface->setRenderContext(&m_renderer);
  m_documentSurface->setAnimationManager(&m_animations);
  m_documentSurface->setDismissedCallback([this] { dismissDocumentMenu(); });
  auto menu = std::make_unique<ContextMenuControl>();
  m_documentMenu = menu.get();
  auto scroll =
      ui::scrollView({.out = &m_documentScroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .flexGrow = 1.0F});
  scroll->content()->addChild(std::move(menu));
  m_documentRoot = ui::column(
      {.fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh), .radius = Style::radiusLg}, std::move(scroll)
  );
  m_documentRoot->setAnimationManager(&m_animations);
  m_documentSurface->setSceneRoot(m_documentRoot.get());
  m_documentDispatcher.setSceneRoot(m_documentRoot.get());
  m_documentDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_connection.extension<files::DesktopIntegration>().setCursorShape(serial, shape);
  });
  m_documentSurface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (auto* menu = m_page.contextMenu()) {
      menu->setSubmenuDirection(
          m_documentSurface->configuredX() < 0 ? ContextSubmenuDirection::Left : ContextSubmenuDirection::Right
      );
      m_page.menuSurface()->requestLayout();
    }
    m_documentSurface->requestLayout();
  });
  m_documentSurface->setPrepareFrameCallback([this](bool, bool) {
    if (m_documentSurface && m_renderer.makeCurrent(m_documentSurface->renderTarget())) {
      UiPhaseScope phase(UiPhase::Layout);
      m_documentRoot->arrange(
          m_documentSurface->renderTarget().renderer(),
          {0, 0, static_cast<float>(m_documentSurface->width()), static_cast<float>(m_documentSurface->height())}
      );
    }
  });
  const PopupSurfaceConfig config{
      .anchorX = 0,
      .anchorY = static_cast<int>(std::floor(menuY + top)),
      .anchorWidth = static_cast<int>(menuSurface->width()),
      .anchorHeight = std::max(1, static_cast<int>(bottom - top)),
      .width = 300,
      .height = 84,
      .anchor = XDG_POSITIONER_ANCHOR_TOP_RIGHT,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
      .serial = m_page.popupSerial()
  };
  if (!m_documentSurface->initializeAsChild(
          menuSurface->xdgSurface(), m_connection.outputForSurface(m_page.surface().wlSurface()), config
      )) {
    closeDocumentMenu();
    return;
  }
  fillDocumentMenu(request, {}, true);
  const auto generation = ++m_documentGeneration;
  m_templates.load([this, request, generation](files::TemplateListing listing) {
    if (generation == m_documentGeneration && m_documentSurface) {
      fillDocumentMenu(request, std::move(listing));
    }
  });
}

bool CreateController::documentOwns(const wl_surface* surface) const {
  return m_documentSurface && m_documentSurface->wlSurface() == surface;
}

void CreateController::outputChanged(wl_output* output) {
  if (m_documentSurface && m_connection.outputForSurface(m_documentSurface->wlSurface()) == output) {
    m_documentSurface->onSurfaceOutputEnter(m_documentSurface->wlSurface(), output);
  }
}

bool CreateController::documentKey(const KeyboardEvent& event) {
  if (!m_documentSurface) {
    return false;
  }
  if (!event.pressed) {
    return true;
  }
  if (KeySymbol::isEscape(event.sym) || KeySymbol::isLeft(event.sym)) {
    dismissDocumentMenu();
  } else if (KeySymbol::isEnter(event.sym)) {
    if (m_documentMenu->highlightedIndex() >= m_documentMenu->entryCount()) {
      (void)m_documentMenu->moveHighlight(1);
    }
    (void)m_documentMenu->activateHighlighted();
  } else if (KeySymbol::isUp(event.sym) || KeySymbol::isDown(event.sym)) {
    (void)m_documentMenu->moveHighlight(KeySymbol::isUp(event.sym) ? -1 : 1);
    const auto index = m_documentMenu->highlightedIndex();
    const float top = m_documentMenu->rowTop(index), bottom = m_documentMenu->rowBottom(index);
    const float offset = m_documentScroll->scrollOffset();
    if (top < offset) {
      m_documentScroll->requestScrollToOffset(top);
    } else if (bottom > offset + m_documentScroll->height()) {
      m_documentScroll->requestScrollToOffset(bottom - m_documentScroll->height());
    }
    m_documentSurface->requestLayout();
  }
  return true;
}

void CreateController::menuHover() {
  auto* menu = m_page.contextMenu();
  if (m_documentRow && menu && menu->highlightedIndex() == *m_documentRow) {
    m_documentKeyboard = false;
    (void)menu->activateHighlighted();
  } else if (m_documentSurface) {
    dismissDocumentMenu();
  }
}

void CreateController::menuScrolled() {
  if (m_documentSurface) {
    dismissDocumentMenu();
  }
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
  auto editor = std::make_unique<NameEditor>(
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
