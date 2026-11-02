#pragma once

#include <functional>
#include <optional>
#include <string>

// Text controls share the ordinary application clipboard transport.
class TextClipboard {
public:
  virtual ~TextClipboard() = default;

  // Most recent plain-text clipboard selection currently known, if any.
  virtual std::optional<std::string> clipboardText() = 0;

  // Replace the system clipboard selection with the given text.
  virtual void setClipboardText(std::string text) = 0;
  virtual void requestClipboardText(std::function<void(std::optional<std::string>)> callback) = 0;
};
