#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"

#include <memory>
#include <print>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  void sharedArea() {
    Node root;
    root.setSize(100, 100);
    auto node = std::make_unique<InputArea>();
    node->setSize(50, 50);
    node->setFocusable(true);
    auto* area = node.get();
    root.addChild(std::move(node));
    InputDispatcher first, second;
    first.setSceneRoot(&root);
    second.setSceneRoot(&root);
    first.setFocus(area);
    second.setFocus(area);
    first.pointerEnter(10, 10, 1);
    second.pointerEnter(10, 10, 1);
    first.pointerButton(10, 10, BTN_LEFT, true, 1, 0, false);
    require(first.pointerCaptured(), "fixture did not capture the area");
    root.removeChild(area).reset();
    require(
        !first.focusedArea()
            && !second.focusedArea()
            && !first.hoveredArea()
            && !second.hoveredArea()
            && !first.pointerCaptured(),
        "one dispatcher lost its destruction notification"
    );
  }
  void reparentedArea() {
    Node left, right;
    left.setSize(100, 100);
    right.setSize(100, 100);
    auto node = std::make_unique<InputArea>();
    node->setSize(50, 50);
    node->setFocusable(true);
    auto* area = node.get();
    left.addChild(std::move(node));
    auto previous = std::make_unique<InputDispatcher>();
    previous->setSceneRoot(&left);
    previous->setFocus(area);
    right.addChild(left.removeChild(area));
    InputDispatcher current;
    current.setSceneRoot(&right);
    current.setFocus(area);
    previous.reset();
    right.removeChild(area).reset();
    require(!current.focusedArea(), "reparented area retained a dead focus");
  }
} // namespace
int main() {
  try {
    sharedArea();
    reparentedArea();
    std::println("input lifetime checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "input lifetime test: {}", error.what());
    return 1;
  }
}
