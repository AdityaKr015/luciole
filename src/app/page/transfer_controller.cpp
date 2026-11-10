#include "app/page/transfer_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "core/ui_phase.h"
#include "fs/gio_ptr.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/file_error_text.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace {
  bool arrived(const files::TransferResult& result) {
    return result.outcome == files::TransferOutcome::Moved
        || result.outcome == files::TransferOutcome::Copied
        || result.outcome == files::TransferOutcome::Linked;
  }
} // namespace

TransferController::TransferController(PageContext& page, std::function<void()> progressTick)
    : m_page(page), m_progressTick(std::move(progressTick)) {}

void TransferController::paste(const files::ActionRequest& request) {
  if (request.folder) {
    paste(request, files::ActionScope::Location, *request.folder);
  }
}

void TransferController::pasteInto(const files::ActionRequest& request) {
  if (request.targets.size() == 1) {
    paste(request, files::ActionScope::Targets, request.targets.front().location);
  }
}

void TransferController::paste(
    const files::ActionRequest& request, files::ActionScope scope, files::Location destination
) {
  m_pendingFilePaste = true;
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_page.clipboard().requestFiles([this, lifetime, request, scope,
                                   destination =
                                       std::move(destination)](std::optional<files::ClipboardFiles> selection) {
    if (lifetime.expired()) {
      return;
    }
    const bool pending = std::exchange(m_pendingFilePaste, false);
    if (!pending || !m_page.visible() || !m_page.requestCurrent(request, scope)) {
      return;
    }
    if (!selection) {
      m_page.actionError("The clipboard changed or its file data could not be read.");
      return;
    }
    if (m_page.operationsBusy()) {
      return;
    }
    const bool cut = selection->payload.cut;
    if (cut && !m_page.clipboard().claimMove(selection->revision)) {
      return;
    }
    const auto job = startPayloadTransfer(
        selection->payload, destination, cut ? files::TransferAction::Move : files::TransferAction::Copy
    );
    if (cut) {
      if (job) {
        finishMove(*job, std::move(*selection));
      } else {
        m_page.clipboard().finishMove(*selection, {});
      }
    }
  });
}

void TransferController::transferTo(
    const files::ActionRequest& request, const files::Location& destination, bool copy
) {
  if (!m_page.requestCurrent(request, files::ActionScope::Targets)) {
    return;
  }
  std::vector<files::Location> sources;
  sources.reserve(request.targets.size());
  for (const auto& target : request.targets) {
    sources.push_back(target.location);
  }
  if (!sources.empty()) {
    transfer(std::move(sources), destination, copy ? files::TransferAction::Copy : files::TransferAction::Move);
  }
}

void TransferController::transfer(
    std::vector<files::Location> sources, const files::Location& destination, files::TransferAction action
) {
  (void)start({std::move(sources), destination, action});
}

std::shared_ptr<OperationJob> TransferController::start(files::TransferRequest request) {
  auto job = m_page.jobs().create<files::TransferService>();
  if (!job->service<files::TransferService>().start(std::move(request))) {
    return nullptr;
  }
  return job;
}

void TransferController::finishMove(OperationJob& job, files::ClipboardFiles selection) {
  job.whenIdle([&clipboard = m_page.clipboard(), selection = std::move(selection)](OperationJob& done) {
    clipboard.finishMove(selection, done.service<files::TransferService>().results());
  });
}

void TransferController::receiveDrop(
    files::FilePayload payload, const files::Location& destination, files::DropOptions options,
    files::DropCompletion finish
) {
  if (options.action == files::DragAction::Ask) {
    chooseDrop(std::move(payload), destination, std::move(options), std::move(finish));
    return;
  }
  const auto action =
      options.action == files::DragAction::Move ? files::TransferAction::Move : files::TransferAction::Copy;
  if (!options.select || !options.select(action)) {
    finish(false);
    return;
  }
  resolvedDrop(std::move(payload), destination, action, std::move(finish));
}

void TransferController::resolvedDrop(
    files::FilePayload payload, const files::Location& destination, files::TransferAction action,
    files::DropCompletion finish
) {
  const auto job = startPayloadTransfer(std::move(payload), destination, action);
  if (!job) {
    finish(false);
    return;
  }
  job->whenIdle([finish = std::move(finish)](OperationJob& done) {
    const auto& results = done.service<files::TransferService>().results();
    finish(!results.empty() && std::ranges::all_of(results, arrived));
  });
}

void TransferController::chooseDrop(
    files::FilePayload payload, const files::Location& destination, files::DropOptions options,
    files::DropCompletion finish
) {
  if (payload.locations.empty()
      || !m_page.visible()
      || m_page.menuSurface()
      || m_page.operationsBusy()
      || !m_page.popupAvailable()) {
    finish(false);
    return;
  }
  if (!files::transferLocationSupported(destination) || std::ranges::any_of(payload.locations, [](const auto& item) {
        return !files::transferLocationSupported(item);
      })) {
    m_page.actionError("File transfers support local and mounted SFTP folders outside Trash.");
    finish(false);
    return;
  }
  struct Pending {
    files::FilePayload payload;
    files::Location destination;
    files::DropOptions options;
    files::DropCompletion finish;
    bool chosen = false;
    ~Pending() {
      if (!chosen) {
        finish(false);
      }
    }
    void cancel() {
      if (!std::exchange(chosen, true)) {
        finish(false);
      }
    }
  };
  const auto pending =
      std::make_shared<Pending>(std::move(payload), destination, std::move(options), std::move(finish));
  const auto context = m_page.browser().contextSerial();
  const std::weak_ptr<int> lifetime = m_lifetime;
  const auto choose = [this, lifetime, pending, context](files::TransferAction action) {
    if (pending->chosen) {
      return;
    }
    if (lifetime.expired()) {
      pending->cancel();
      return;
    }
    if (!m_page.visible()
        || m_page.operationsBusy()
        || context != m_page.browser().contextSerial()
        || !pending->options.select
        || !pending->options.select(action)) {
      pending->cancel();
      m_page.dismissMenu();
      return;
    }
    pending->chosen = true;
    m_page.dismissMenu();
    resolvedDrop(std::move(pending->payload), pending->destination, action, std::move(pending->finish));
  };
  auto root = ui::column({.align = FlexAlign::Stretch});
  files::applyDialogStyle(*root);
  root->addChild(files::dialogHeader("Choose Drop Action", "drag-drop", [this] { m_page.dismissMenu(); }));
  Button* focus = nullptr;
  const auto add = [&](const char* label, const char* glyph, files::TransferAction action, bool enabled) {
    auto button = ui::button(
        {.text = label,
         .glyph = glyph,
         .contentAlign = ButtonContentAlign::Start,
         .variant = ButtonVariant::Ghost,
         .onClick = [choose, action] { choose(action); }}
    );
    button->setEnabled(enabled);
    if (enabled && !focus) {
      focus = button.get();
    }
    root->addChild(std::move(button));
  };
  add("Copy Here", "copy", files::TransferAction::Copy, pending->options.canCopy);
  add("Move Here", "arrows-move", files::TransferAction::Move, pending->options.canMove);
  add(pending->payload.locations.size() == 1 ? "Create Link Here" : "Create Links Here", "link",
      files::TransferAction::Link,
      pending->options.canCopy
          && destination.isNative()
          && std::ranges::all_of(pending->payload.locations, [](const auto& source) { return source.isNative(); }));
  root->addChild(ui::separator({}));
  root->addChild(ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = [this] {
                               m_page.dismissMenu();
                             }}));
  m_page.showCenteredDialog(std::move(root), 360.0F);
  if (m_page.menuSurface()) {
    m_page.activeDialog().bind(ActiveDialog::Kind::DropAction, [pending] { pending->cancel(); });
    if (focus) {
      m_page.menuDispatcher().setFocus(focus->inputArea());
    }
  }
}

void TransferController::transferChanged(OperationJob& job) {
  const auto& transfers = job.service<files::TransferService>();
  if (transfers.busy() || !job.takeResult()) {
    return;
  }
  const auto& results = transfers.results();
  const auto done = std::ranges::count_if(results, arrived);
  std::string message = std::format(
      "{} item{} {}", done, done == 1 ? "" : "s",
      transfers.action() == files::TransferAction::Copy       ? "copied"
          : transfers.action() == files::TransferAction::Link ? "linked"
                                                              : "moved"
  );
  if (const auto error = std::ranges::find_if(results, [](const auto& item) { return item.error.has_value(); });
      error != results.end()) {
    message +=
        std::format(" · {}: {}", error->source.displayName().value_or("Item"), files::fileErrorText(*error->error));
  }
  m_page.setActionMessage(std::move(message));
  m_page.browser().refresh();
}

void TransferController::syncConflict() {
  if (!m_page.popupAvailable()) {
    return;
  }
  if (m_activeConflict) {
    const auto* shown = m_page.jobs().find(m_activeConflict->job);
    const auto pending = shown ? shown->conflict() : std::nullopt;
    if (!pending || pending->id != m_activeConflict->conflict) {
      m_activeConflict.reset();
      m_page.dismissMenu();
    }
    return;
  }
  if (m_page.menuSurface() != nullptr) {
    return;
  }
  auto* job = m_page.jobs().oldest([](const OperationJob& candidate) { return candidate.conflict().has_value(); });
  const auto conflict = job ? job->conflict() : std::nullopt;
  if (!conflict) {
    return;
  }
  const bool archive = job->kind() == JobKind::Archive;
  const bool restore = job->kind() == JobKind::Restore;
  const ActiveConflict active{.job = job->id(), .conflict = conflict->id};
  auto& surface = m_page.surface();
  const float width = std::min(520.0F, static_cast<float>(surface.width()) - 32.0F);
  auto facts = [&](const std::string& title, bool folder, std::optional<std::uint64_t> bytes,
                   std::optional<std::uint64_t> modified) {
    std::string description = folder ? "Folder" : "File";
    if (!folder && bytes) {
      const files::GString size(g_format_size_full(*bytes, G_FORMAT_SIZE_IEC_UNITS));
      description += " · " + std::string(size.get());
    }
    std::string date = "Modification time unknown";
    if (modified && *modified <= static_cast<std::uint64_t>(G_MAXINT64)) {
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> value(
          g_date_time_new_from_unix_local(static_cast<gint64>(*modified)), &g_date_time_unref
      );
      if (value) {
        const files::GString text(g_date_time_format(value.get(), "%Y-%m-%d %H:%M"));
        date = text.get();
      }
    }
    return ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceXs, .flexGrow = 1.0F},
        ui::label(
            {.text = title, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        ),
        ui::label({.text = description, .maxWidth = (width - 48.0F) * 0.5F, .maxLines = 1}),
        ui::label(
            {.text = date,
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxWidth = (width - 48.0F) * 0.5F,
             .maxLines = 1}
        )
    );
  };
  const auto applyToAll = std::make_shared<bool>(false);
  const auto choose = [this, applyToAll, active](files::TransferConflictChoice choice) {
    auto* owner = m_page.jobs().find(active.job);
    if (owner
        && owner->resolve(active.conflict, choice, *applyToAll && choice != files::TransferConflictChoice::Cancel)) {
      m_activeConflict.reset();
      m_page.dismissMenu();
    }
  };
  Button* keepBoth = nullptr;
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      files::dialogHeader(
          "Item already exists", "alert-triangle", [choose] { choose(files::TransferConflictChoice::Cancel); }
      ),
      ui::label(
          {.text = conflict->name,
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .configure = [](Label& name) { name.setUseMarkup(false); }}
      ),
      ui::label(
          {.text = restore ? files::locationEditText(conflict->destination) : "",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .visible = restore,
           .participatesInLayout = restore,
           .configure = [](Label& path) { path.setUseMarkup(false); }}
      ),
      ui::separator({}),
      ui::row(
          {.align = FlexAlign::Start, .gap = Style::spaceMd},
          facts("Incoming", conflict->sourceDirectory, conflict->sourceSize, conflict->sourceModified),
          facts("Existing", conflict->destinationDirectory, conflict->destinationSize, conflict->destinationModified)
      ),
      ui::label(
          {.text = restore                      ? "Folder replacement is unavailable. Keep both or skip this item."
               : conflict->canMerge && !archive ? "Merge keeps existing contents and asks about conflicting "
                                                  "children. Replace removes the whole existing folder."
                                                : "Replace removes the existing folder and its contents.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .visible = conflict->destinationDirectory,
           .participatesInLayout = conflict->destinationDirectory}
      ),
      ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .visible = !archive && !restore && conflict->canApplyToAll,
           .participatesInLayout = !archive && !restore && conflict->canApplyToAll},
          ui::toggle({.onChange = [applyToAll](bool checked) { *applyToAll = checked; }}),
          ui::label(
              {.text = conflict->scopeLabel,
               .fontSize = Style::fontSizeCaption,
               .maxWidth = width - 96.0F,
               .maxLines = 2}
          )
      ),
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.text = "Cancel",
               .variant = ButtonVariant::Ghost,
               .onClick = [choose] { choose(files::TransferConflictChoice::Cancel); }}
          ),
          ui::button(
              {.text = "Skip",
               .variant = ButtonVariant::Ghost,
               .onClick = [choose] { choose(files::TransferConflictChoice::Skip); }}
          ),
          ui::button(
              {.out = &keepBoth,
               .text = "Keep both",
               .variant = ButtonVariant::Primary,
               .onClick = [choose] { choose(files::TransferConflictChoice::KeepBoth); }}
          ),
          ui::button(
              {.text = "Merge",
               .variant = ButtonVariant::Ghost,
               .visible = !archive && !restore && conflict->canMerge,
               .participatesInLayout = !archive && !restore && conflict->canMerge,
               .onClick = [choose] { choose(files::TransferConflictChoice::Merge); }}
          ),
          ui::button(
              {.text = "Replace",
               .enabled = !restore || (!conflict->sourceDirectory && !conflict->destinationDirectory),
               .variant = ButtonVariant::Destructive,
               .tooltip = restore && (conflict->sourceDirectory || conflict->destinationDirectory)
                   ? "Folder replacement is unavailable; keep both or skip."
                   : "Replace the existing item",
               .onClick = [choose] { choose(files::TransferConflictChoice::Replace); }}
          )
      )
  );
  files::applyDialogStyle(*root);
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
  if (m_page.menuSurface() != nullptr) {
    m_page.activeDialog().bind(ActiveDialog::Kind::TransferConflict, [this] {
      if (const auto shown = std::exchange(m_activeConflict, std::nullopt)) {
        if (auto* owner = m_page.jobs().find(shown->job)) {
          (void)owner->resolve(shown->conflict, files::TransferConflictChoice::Cancel);
        }
      }
    });
    m_activeConflict = active;
    m_page.menuDispatcher().setFocus(keepBoth->inputArea());
  } else {
    (void)job->resolve(conflict->id, files::TransferConflictChoice::Cancel);
  }
}

OperationJob* TransferController::latestTransfer() const {
  return m_page.jobs().newest([](const OperationJob& job) { return job.kind() == JobKind::Transfer && job.ran(); });
}

OperationJob* TransferController::cancelTarget() const {
  return m_page.jobs().newest([](const OperationJob& job) { return job.cancellable(); });
}

std::string_view TransferController::report() const {
  const auto* job = latestTransfer();
  if (!job || job->running()) {
    return {};
  }
  return job->service<files::TransferService>().report();
}

void TransferController::retry() {
  auto* job = latestTransfer();
  if (m_page.operationsBusy() || !job || job->service<files::TransferService>().retryable() == 0) {
    return;
  }
  auto& transfers = job->service<files::TransferService>();
  if (transfers.action() != files::TransferAction::Move || !m_page.clipboard().hasFiles()) {
    (void)transfers.retryFailed();
    return;
  }
  const auto id = job->id();
  const auto run = transfers.jobId();
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_page.clipboard().requestFiles([this, lifetime, id, run](std::optional<files::ClipboardFiles> selection) {
    if (lifetime.expired() || m_page.operationsBusy()) {
      return;
    }
    auto* job = m_page.jobs().find(id);
    if (!job || job->service<files::TransferService>().jobId() != run) {
      return;
    }
    auto& transfers = job->service<files::TransferService>();
    const bool claim = selection
        && selection->payload.cut
        && std::ranges::any_of(selection->payload.locations, [&transfers](const auto& location) {
                         return std::ranges::any_of(transfers.results(), [&](const auto& result) {
                           return result.source == location;
                         });
                       });
    if (claim && !m_page.clipboard().claimMove(selection->revision)) {
      return;
    }
    if (!transfers.retryFailed()) {
      if (claim) {
        m_page.clipboard().finishMove(*selection, {});
      }
    } else if (claim) {
      finishMove(*job, std::move(*selection));
    }
  });
}

std::shared_ptr<OperationJob> TransferController::startPayloadTransfer(
    files::FilePayload payload, const files::Location& destination, files::TransferAction action
) {
  if (payload.locations.empty()
      || !files::transferLocationSupported(destination)
      || std::ranges::any_of(payload.locations, [](const auto& location) {
           return !files::transferLocationSupported(location);
         })) {
    m_page.actionError("File transfers support local and mounted SFTP folders outside Trash.");
    return nullptr;
  }
  m_page.setActionMessage({});
  auto job = start({std::move(payload.locations), destination, action});
  if (!job) {
    m_page.actionError("Another file transfer is still finishing.");
  }
  return job;
}

void TransferController::updateOperationProgress() {
  if (!m_page.jobs().newest([](const OperationJob& job) {
        return job.cancellable() || (job.kind() == JobKind::Permission && job.running());
      })) {
    m_operationProgress.stop();
  } else if (!m_operationProgress.active()) {
    // The tick checks again: a job that moved to another page sends this page no further change.
    m_operationProgress.startRepeating(std::chrono::milliseconds(100), [this] {
      m_progressTick();
      updateOperationProgress();
    });
  }
}

std::unique_ptr<Button> TransferController::buildRetryButton() {
  return ui::button(
      {.out = &m_retryTransfer,
       .text = "Retry failed",
       .variant = ButtonVariant::Ghost,
       .tooltip = "Retry failed items without repeating completed transfers",
       .visible = false,
       .participatesInLayout = false,
       .onClick = [this] { retry(); }}
  );
}

std::unique_ptr<Button> TransferController::buildCancelButton() {
  return ui::button(
      {.out = &m_cancelOperation,
       .glyph = "x",
       .controlHeight = Style::controlHeightSm,
       .variant = ButtonVariant::Ghost,
       .tooltip = "Cancel transfer",
       .visible = false,
       .participatesInLayout = false,
       .onClick = [this] {
         if (auto* job = cancelTarget()) {
           job->cancel();
         }
       }}
  );
}

bool TransferController::syncOperationButtons() {
  const auto* cancel = cancelTarget();
  const auto* latest = latestTransfer();
  const bool retryable =
      latest && latest->service<files::TransferService>().retryable() > 0 && !m_page.operationsBusy();
  m_retryTransfer->setVisible(retryable);
  m_retryTransfer->setParticipatesInLayout(retryable);
  m_cancelOperation->setVisible(cancel != nullptr);
  m_cancelOperation->setParticipatesInLayout(cancel != nullptr);
  if (!cancel) {
    return retryable;
  }
  const auto kind = cancel->kind();
  m_cancelOperation->setTooltip(
      kind == JobKind::BatchRename   ? "Cancel batch rename"
          : kind == JobKind::Restore ? "Cancel restoration"
          : kind == JobKind::EmptyTrash
          ? (cancel->service<files::EmptyTrashService>().snapshot().selection ? "Cancel deletion"
                                                                              : "Cancel emptying Trash")
          : kind == JobKind::Trash ? "Cancel move to Trash"
                                   : "Cancel transfer"
  );
  return true;
}

void TransferController::stopProgress() { m_operationProgress.stop(); }
