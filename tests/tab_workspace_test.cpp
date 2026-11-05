#include "browser/tab_workspace.h"

#include <algorithm>
#include <cmath>
#include <print>
#include <random>
#include <set>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }
  void invariant(const files::TabWorkspace& workspace) {
    std::set<files::TabId> ids;
    std::size_t tabCount = 0;
    for (const auto& [id, window] : workspace.windows()) {
      tabCount += window.tabs.size();
      if (window.tabs.empty()) {
        require(
            !window.selected && !window.primary && !window.active && !window.secondary, "empty window retained a page"
        );
      } else {
        require(std::ranges::contains(window.tabs, window.selected), "selected logical tab lost ownership");
        require(window.active == window.primary || window.active == window.secondary, "active page is hidden");
        if (window.secondary) {
          require(*window.secondary != window.primary, "split repeated a page");
        }
      }
      require(
          std::isfinite(window.fraction) && window.fraction >= 0.2F && window.fraction <= 0.8F,
          "invalid divider fraction"
      );
      for (auto tab : window.tabs) {
        const auto pages = workspace.pages(tab);
        require(!pages.empty() && pages.size() <= 2, "invalid logical-tab layout");
        for (auto page : pages) {
          require(
              ids.insert(page).second && workspace.owner(page) == id && workspace.tabFor(page) == tab,
              "page has duplicate ownership"
          );
        }
      }
    }
    require(tabCount <= 32 && ids.size() <= 64, "tab or pane limit exceeded");
    for (auto tab : workspace.closed()) {
      for (auto page : workspace.pages(tab)) {
        require(!ids.contains(page), "closed tab retained a live page");
      }
    }
    require(workspace.closed().size() <= 16, "closed-tab retention exceeded");
  }
  void workflows() {
    files::TabWorkspace workspace;
    const auto left = workspace.addWindow(), right = workspace.addWindow();
    const auto a = workspace.addTab(left);
    const auto auxiliary = workspace.addPane(left);
    require(
        auxiliary && workspace.windows().at(left).tabs.size() == 1 && workspace.tabFor(auxiliary) == a,
        "split became a new tab"
    );
    const auto b = workspace.addTab(left);
    require(!workspace.windows().at(left).secondary, "new tab inherited another tab's split");
    workspace.select(left, a);
    require(
        workspace.windows().at(left).secondary == auxiliary && workspace.windows().at(left).active == auxiliary,
        "tab switch lost split or focus"
    );
    require(workspace.focus(left, a) && workspace.other(left) == auxiliary, "pane focus switched logical tabs");
    require(
        workspace.move(auxiliary, right, 0) && workspace.owner(a) == right && workspace.owner(auxiliary) == right,
        "moving a tab separated its panes"
    );
    require(
        workspace.windows().at(left).selected == b && !workspace.select(left, a),
        "moved tab remained selectable in the source"
    );
    require(
        workspace.close(auxiliary) && !workspace.owner(a) && workspace.reopen(left),
        "closing or reopening an auxiliary pane lost its whole tab"
    );
    require(workspace.windows().at(left).secondary == auxiliary, "reopen lost auxiliary pane");
    require(workspace.swap(left) && workspace.windows().at(left).active == a, "swap changed focused page identity");
    require(
        workspace.unsplit(left)
            && workspace.windows().at(left).tabs.size() == 2
            && workspace.windows().at(left).primary == a
            && !workspace.tabFor(auxiliary),
        "unsplit changed the tab or retained its auxiliary page"
    );
    require(workspace.move(a, left, 0) && workspace.windows().at(left).tabs.front() == a, "reorder changed ownership");
    invariant(workspace);
  }
  void closeEitherPane() {
    files::TabWorkspace workspace;
    const auto window = workspace.addWindow();
    const auto original = workspace.addTab(window);
    const auto right = workspace.addPane(window);
    require(
        workspace.closePane(window, original)
            && workspace.windows().at(window).primary == right
            && workspace.windows().at(window).tabs == std::vector{original},
        "closing the left pane closed or renamed its tab"
    );
    const auto added = workspace.addPane(window);
    require(
        workspace.closePane(window, added) && workspace.windows().at(window).primary == right,
        "closing the right pane changed the surviving page"
    );
    require(!workspace.closePane(window, right), "sole pane close bypassed close-tab semantics");
    require(
        workspace.close(right) && workspace.reopen(window) && workspace.windows().at(window).primary == right,
        "reopen lost a promoted pane"
    );
    const auto second = workspace.addWindow();
    require(
        workspace.move(right, second, 0) && workspace.owner(original) == second && workspace.owner(right) == second,
        "promoted pane lost logical ownership during detach"
    );
    invariant(workspace);
  }
  void transitions() {
    files::TabWorkspace workspace;
    std::mt19937 random(42);
    for (int i = 0; i < 4; ++i) {
      workspace.addWindow();
    }
    for (int i = 0; i < 10000; ++i) {
      const auto window = 1 + random() % 4;
      const auto page = 1 + random() % 500;
      switch (random() % 10) {
      case 0:
        workspace.addTab(window);
        break;
      case 1:
        workspace.close(page);
        break;
      case 2:
        workspace.reopen(window);
        break;
      case 3:
        workspace.select(window, page);
        break;
      case 4:
        workspace.move(page, window, random() % 40);
        break;
      case 5:
        workspace.addPane(window);
        break;
      case 6:
        workspace.unsplit(window);
        break;
      case 7:
        workspace.swap(window);
        break;
      case 8:
        workspace.resize(window, static_cast<float>(random() % 400) / 100 - 1);
        break;
      case 9:
        workspace.focus(window, page);
        break;
      }
      invariant(workspace);
    }
    require(!workspace.resize(1, NAN), "nonfinite divider accepted");
  }
} // namespace
int main() {
  try {
    workflows();
    closeEitherPane();
    transitions();
    std::println("tab workspace checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "tab workspace test: {}", error.what());
    return 1;
  }
}
