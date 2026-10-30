#pragma once

#include "wayland/wayland_seat.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <wayland-client.h>

struct xdg_wm_base;
struct wp_viewporter;
struct wp_fractional_scale_manager_v1;
struct wp_cursor_shape_manager_v1;
struct zwp_text_input_manager_v3;
struct zxdg_decoration_manager_v1;
class TextInputService;

struct WaylandOutput {
  std::uint32_t name = 0;
  wl_output* output = nullptr;
  std::string connectorName;
  std::string description;
  std::int32_t scale = 1;
  std::int32_t configuredScaleNumerator = 120;
};

// Application-only transport, adapted from Noctalia's registry/output/seat wiring.
class WaylandConnection {
public:
  WaylandConnection() = default;
  ~WaylandConnection();
  WaylandConnection(const WaylandConnection&) = delete;
  WaylandConnection& operator=(const WaylandConnection&) = delete;

  bool initialize(std::string& error);
  [[nodiscard]] wl_display* display() const noexcept { return m_display; }
  [[nodiscard]] wl_compositor* compositor() const noexcept { return m_compositor; }
  [[nodiscard]] wl_data_device_manager* dataDeviceManager() const noexcept { return m_dataDeviceManager; }
  [[nodiscard]] wl_seat* seat() const noexcept { return m_seat; }
  [[nodiscard]] xdg_wm_base* xdgWmBase() const noexcept { return m_xdgWmBase; }
  [[nodiscard]] wp_viewporter* viewporter() const noexcept { return m_viewporter; }
  [[nodiscard]] wp_fractional_scale_manager_v1* fractionalScaleManager() const noexcept { return m_fractionalScale; }
  [[nodiscard]] zxdg_decoration_manager_v1* decorationManager() const noexcept { return m_decorations; }
  [[nodiscard]] bool hasXdgShell() const noexcept { return m_xdgWmBase != nullptr; }
  [[nodiscard]] bool hasFractionalScale() const noexcept {
    return m_viewporter != nullptr && m_fractionalScale != nullptr;
  }
  [[nodiscard]] WaylandSeat& input() noexcept { return m_input; }
  void bindTextInput(TextInputService& service);

  [[nodiscard]] WaylandOutput* findOutputByWl(wl_output* output);
  [[nodiscard]] const WaylandOutput* findOutputByWl(wl_output* output) const;
  [[nodiscard]] wl_output* outputForSurface(wl_surface* surface) const;
  void registerSurfaceOutput(wl_surface* surface, wl_output* output);
  void notifySurfaceOutputEnter(wl_surface* surface, wl_output* output);
  void notifySurfaceOutputLeave(wl_surface* surface, wl_output* output);
  void unregisterSurface(wl_surface* surface);
  void setOutputChangedCallback(std::function<void(wl_output*)> callback);

  static void
  handleGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
  static void handleGlobalRemove(void* data, wl_registry* registry, std::uint32_t name);
  static void handleOutputScale(void* data, wl_output* output, std::int32_t factor);
  static void handleOutputDone(void* data, wl_output* output);
  static void handleOutputName(void* data, wl_output* output, const char* name);
  static void handleOutputDescription(void* data, wl_output* output, const char* description);

private:
  void cleanup();
  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  wl_compositor* m_compositor = nullptr;
  wl_seat* m_seat = nullptr;
  wl_shm* m_shm = nullptr;
  wl_data_device_manager* m_dataDeviceManager = nullptr;
  xdg_wm_base* m_xdgWmBase = nullptr;
  wp_viewporter* m_viewporter = nullptr;
  wp_fractional_scale_manager_v1* m_fractionalScale = nullptr;
  wp_cursor_shape_manager_v1* m_cursorShape = nullptr;
  zwp_text_input_manager_v3* m_textInputManager = nullptr;
  zxdg_decoration_manager_v1* m_decorations = nullptr;
  TextInputService* m_textInput = nullptr;
  WaylandSeat m_input;
  std::vector<WaylandOutput> m_outputs;
  std::unordered_map<wl_surface*, std::vector<wl_output*>> m_surfaceOutputs;
  std::function<void(wl_output*)> m_outputChanged;
};
