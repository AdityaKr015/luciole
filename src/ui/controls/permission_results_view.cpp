#include "ui/controls/permission_results_view.h"

#include "core/input/key_symbols.h"
#include "ui/builders.h"
#include "ui/controls/file_error_text.h"
#include "ui/controls/virtual_grid_view.h"

#include <algorithm>
#include <array>
#include <format>

namespace {
  constexpr std::array outcomes{"Ready", "Changed", "Unchanged", "Failed", "Partially changed", "Cancelled", "Skipped"};
  class ResultRow final : public Flex {
  public:
    ResultRow() {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Stretch);
      setPadding(4, 6);
      setGap(2);
      addChild(ui::label({.out = &m_name, .fontWeight = FontWeight::SemiBold, .maxLines = 1}));
      addChild(ui::label({.out = &m_metadata, .fontSize = Style::fontSizeCaption, .maxLines = 1}));
      addChild(ui::label({.out = &m_detail, .fontSize = Style::fontSizeCaption, .maxLines = 3}));
      for (auto* label : {m_name, m_metadata, m_detail}) {
        label->setUseMarkup(false);
      }
      setHitTestVisible(false);
    }
    void bind(const luciole::PermissionResult& row, bool hovered) {
      m_name->setText(row.name + " · " + outcomes[static_cast<std::size_t>(row.outcome)]);
      m_metadata->setText(
          row.info ? std::format("Mode {:04o} · Owner {} · Group {}", row.info->mode, row.info->owner, row.info->group)
                   : "Permissions and ownership unknown"
      );
      if (row.info && row.info->acl && row.info->acl->supported) {
        m_metadata->setText(
            m_metadata->text()
            + std::format(" · ACL: {} access, {} default", row.info->acl->access.size(), row.info->acl->defaults.size())
        );
      }
      std::string detail = row.error ? luciole::fileErrorText(*row.error) : "";
      if (!detail.empty() && !row.message.empty()) {
        detail += ' ';
      }
      detail += row.message;
      m_detail->setText(row.location.uri() + (detail.empty() ? "" : "\n" + detail));
      const bool error =
          row.outcome == luciole::PermissionOutcome::Failed || row.outcome == luciole::PermissionOutcome::Partial;
      m_name->setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurface));
      m_metadata->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_detail->setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
      setFill(hovered ? colorSpecFromRole(ColorRole::SurfaceContainerHigh) : clearColorSpec());
    }

  private:
    void doLayout(Renderer& renderer) override {
      for (auto* label : {m_name, m_metadata, m_detail}) {
        label->setMaxWidth(std::max(1.0F, width() - 12.0F));
      }
      Flex::doLayout(renderer);
    }
    Label *m_name = nullptr, *m_metadata = nullptr, *m_detail = nullptr;
  };
} // namespace
class PermissionResultsView::Adapter final : public VirtualGridAdapter {
public:
  explicit Adapter(PermissionResultsView& owner) : m_owner(owner) {}
  std::size_t itemCount() const override { return m_owner.m_results ? m_owner.m_results->size() : 0; }
  std::unique_ptr<Node> createTile() override { return std::make_unique<ResultRow>(); }
  void bindTile(Node& tile, std::size_t index, bool, bool hovered) override {
    static_cast<ResultRow&>(tile).bind((*m_owner.m_results)[index], hovered);
  }

private:
  PermissionResultsView& m_owner;
};
PermissionResultsView::PermissionResultsView() : m_adapter(std::make_unique<Adapter>(*this)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setFlexGrow(1.0F);
  setFillWidth(true);
  setFillHeight(true);
  addChild(
      ui::virtualGridView(
          {.out = &m_grid,
           .columns = 1,
           .cellHeight = 100.0F,
           .squareCells = false,
           .rowGap = 0.0F,
           .overscanRows = 1,
           .adapter = m_adapter.get(),
           .flexGrow = 1.0F}
      )
  );
}
bool PermissionResultsView::navigate(std::uint32_t key, std::uint32_t modifiers, bool preedit) {
  if (preedit || modifiers != 0 || !m_results || m_results->empty()) {
    return false;
  }
  const auto last = m_results->size() - 1;
  const auto current = std::min(m_grid->selectedIndex().value_or(0), last);
  auto next = current;
  const auto page = m_grid->pageItemStride();
  if (KeySymbol::isHome(key)) {
    next = 0;
  } else if (KeySymbol::isEnd(key)) {
    next = last;
  } else if (KeySymbol::isUp(key)) {
    next = current > 0 ? current - 1 : 0;
  } else if (KeySymbol::isDown(key)) {
    next = std::min(current + 1, last);
  } else if (KeySymbol::isPageUp(key)) {
    next = current > page ? current - page : 0;
  } else if (KeySymbol::isPageDown(key)) {
    next = std::min(current + page, last);
  } else {
    return false;
  }
  m_grid->setSelectedIndex(next, true);
  return true;
}
PermissionResultsView::~PermissionResultsView() { m_grid->setAdapter(nullptr); }
void PermissionResultsView::sync(std::shared_ptr<const std::vector<luciole::PermissionResult>> results) {
  if (results == m_results) {
    return;
  }
  m_results = std::move(results);
  m_grid->notifyDataChanged();
}
