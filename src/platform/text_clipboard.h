#pragma once

#include "core/text_clipboard.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <wayland-client.h>

namespace files {
  // Ordinary application clipboard, bound to this client's seat and input serials.
  class WaylandTextClipboard final : public TextClipboard {
  public:
    WaylandTextClipboard(wl_data_device_manager* manager, wl_seat* seat, std::function<std::uint32_t()> serial);
    ~WaylandTextClipboard() override;
    WaylandTextClipboard(const WaylandTextClipboard&) = delete;
    WaylandTextClipboard& operator=(const WaylandTextClipboard&) = delete;
    [[nodiscard]] bool available() const;
    [[nodiscard]] std::optional<std::string> clipboardText() override;
    void setClipboardText(std::string text) override;
    void requestClipboardText(std::function<void(std::optional<std::string>)> callback) override;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
