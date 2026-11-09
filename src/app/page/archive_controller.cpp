#include "app/page/archive_controller.h"

#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "fs/archive_type.h"
#include "fs/filename.h"
#include "operations/archive_service.h"
#include "platform/desktop_integration.h"
#include "platform/file_launcher.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/archive_view.h"
#include "ui/controls/file_error_text.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <utility>

ArchiveController::ArchiveController(
    PageContext& page, files::ArchiveService& archives, files::FileLauncher& launcher, files::FileIconCache& icons,
    WaylandConnection& connection, InputDispatcher& dispatcher
)
    : m_page(page), m_archives(archives), m_launcher(launcher), m_icons(icons), m_connection(connection),
      m_dispatcher(dispatcher) {}

void ArchiveController::compress(const std::vector<files::DirectoryEntry>& targets) {
  std::vector<files::Location> sources;
  sources.reserve(targets.size());
  for (const auto& target : targets) {
    sources.push_back(target.location);
  }
  openDialog(std::move(sources), {}, true);
}

void ArchiveController::extractTo(const files::Location& archive) { openDialog({archive}, {}, false); }

void ArchiveController::extractHere(const files::Location& archive) {
  if (!m_archives.extract(archive, destination(archive))) {
    m_page.actionError("Could not start extraction.");
  }
}

void ArchiveController::browse(const files::Location& archive) {
  if (m_archives.inspect(archive)) {
    m_source = archive;
    m_wanted = true;
    m_dirty = true;
  } else {
    m_page.actionError("Could not read this archive.");
  }
}

void ArchiveController::changed() {
  m_dirty = true;
  if (auto file = m_archives.takeOpenedFile()) {
    m_launcher.open(*file);
  }
  if (!m_archives.busy()) {
    std::string message = files::fileErrorText(m_archives.error());
    const bool finished = !m_archives.results().empty();
    if (finished && message.empty()) {
      message = "Archive operation complete.";
    }
    m_page.setActionMessage(std::move(message));
    if (finished) {
      m_page.browser().refresh();
    }
  }
}

void ArchiveController::onFrame() {
  if (std::exchange(m_dirty, false)) {
    sync();
  }
}

files::Location ArchiveController::destination(const files::Location& source) {
  return source.parent().value_or(source).child(files::archiveFolderName(source));
}

void ArchiveController::openDialog(
    std::vector<files::Location> sources, std::vector<std::string> selected, bool compress
) {
  if (sources.empty() || m_archives.busy()) {
    return;
  }
  const auto formats = files::archiveFormats();
  if (compress && formats.empty()) {
    m_page.actionError("No archive writer is available.");
    return;
  }
  const auto source = sources.front();
  const auto parent = source.parent();
  if (!parent) {
    m_page.actionError("The source has no destination parent.");
    return;
  }
  const auto& base = *parent;
  const std::string name = (sources.size() == 1 ? source.displayName().value_or("Archive") : "Archive")
      + (formats.empty() ? ".zip" : std::string(formats.front().suffix));
  auto root = std::make_unique<ArchiveDialog>(
      compress, name, files::locationEditText(compress ? base : destination(source)),
      [this, sources = std::move(sources), selected = std::move(selected), base,
       compress](const std::string& name, const std::string& path, files::ArchiveFormat format) -> std::string {
        if (path.empty()) {
          return "Enter a destination folder.";
        }
        auto destination = files::resolveLocationText(path, base);
        if (!destination.nativePath()) {
          return "Choose a native destination folder.";
        }
        if (compress) {
          if (auto problem = files::filenameProblem(name)) {
            return *problem;
          }
          const auto formats = files::archiveFormats();
          const auto found = std::ranges::find(formats, format, &files::ArchiveFormatInfo::format);
          if (found == formats.end() || !name.ends_with(found->suffix)) {
            return "Use the filename extension for the selected format.";
          }
          destination = destination.child(name);
        }
        const bool started = compress ? m_archives.compress(sources, destination, format)
                                      : m_archives.extract(sources.front(), destination, selected);
        if (!started) {
          return "Could not start the archive operation.";
        }
        m_page.dismissMenu();
        return {};
      },
      [this] { m_page.dismissMenu(); }
  );
  auto* dialog = root.get();
  auto& surface = m_page.surface();
  const float width = std::min(520.0F, static_cast<float>(surface.width()) - 32.0F);
  UiPhaseScope phase(UiPhase::Layout);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = std::min(
      root->measure(surface.renderTarget().renderer(), constraints).height, static_cast<float>(surface.height()) - 48.0F
  );
  m_page.showPopup(
      std::move(root), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false
  );
  if (m_page.menuSurface()) {
    // The dialog keeps the page's default dialog width.
    m_page.setDialogSize(588.0F, height);
    m_page.menuDispatcher().setFocus(dialog->focusArea());
  }
}

void ArchiveController::deferClose() {
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, lifetime] {
    if (!lifetime.expired()) {
      close();
    }
  });
}

void ArchiveController::close() {
  m_wanted = false;
  if (!m_view) {
    return;
  }
  m_page.closeMenu();
  m_dispatcher.cancelPointerCapture();
  m_page.focusFileList();
  auto* view = std::exchange(m_view, nullptr);
  auto removed = m_page.sceneRoot().removeChild(view);
  m_page.surface().requestLayout();
}

void ArchiveController::sync() {
  UiPhaseScope phase(UiPhase::Layout);
  if (m_wanted && !m_view) {
    m_page.prepareOverlay();
    auto root = std::make_unique<ArchiveView>(
        m_icons, [this] { return m_connection.keyboardModifiers(); }, [this] { deferClose(); },
        [this](std::vector<std::string> selected) {
          if (m_archives.catalog()) {
            openDialog({m_archives.catalog()->source}, std::move(selected), false);
          }
        },
        [this](const std::string& path) { (void)m_archives.openMember(path); },
        [this] {
          if (m_source) {
            (void)m_archives.inspect(*m_source);
          }
        },
        [this] { m_archives.cancel(); }
    );
    m_view = root.get();
    root->setParticipatesInLayout(false);
    root->setZIndex(1000);
    m_page.sceneRoot().addChild(std::move(root));
    m_dispatcher.cancelPointerCapture();
    m_dispatcher.setFocus(m_view->focusArea());
  }
  if (m_view) {
    m_view->sync(m_archives.catalog(), m_archives.phase(), m_archives.items(), m_archives.bytes(), m_archives.error());
    m_page.surface().requestLayout();
  }
}

bool ArchiveController::key(const KeyboardEvent& event) {
  if (!m_view) {
    return false;
  }
  if (event.pressed && KeySymbol::isEscape(event.sym)) {
    deferClose();
  } else if (event.pressed && event.sym == XKB_KEY_q && event.modifiers == KeyMod::Ctrl) {
    m_page.surface().close();
  } else if (event.pressed && KeySymbol::isTab(event.sym)) {
    (void)m_dispatcher.cycleTabFocusInSubtree(m_view, (event.modifiers & KeyMod::Shift) != 0);
  } else if (
      !event.pressed
      || event.preedit
      || m_dispatcher.focusedArea() != m_view->focusArea()
      || !m_view->key(event.sym, event.modifiers)
  ) {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  }
  m_page.surface().requestLayout();
  return true;
}

void ArchiveController::layout(Renderer& renderer, LayoutRect bounds) {
  if (m_view) {
    m_view->arrange(renderer, bounds);
  }
}
