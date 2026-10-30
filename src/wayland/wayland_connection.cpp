#include "wayland/wayland_connection.h"

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wayland/text_input_service.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>

namespace {
  const wl_registry_listener kRegistryListener = {
      .global = &WaylandConnection::handleGlobal,
      .global_remove = &WaylandConnection::handleGlobalRemove,
  };
  void outputGeometry(
      void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, const char*, const char*,
      std::int32_t
  ) {}
  void outputMode(void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
  const wl_output_listener kOutputListener = {
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = &WaylandConnection::handleOutputDone,
      .scale = &WaylandConnection::handleOutputScale,
      .name = &WaylandConnection::handleOutputName,
      .description = &WaylandConnection::handleOutputDescription,
  };
  void xdgWmBasePing(void*, xdg_wm_base* wmBase, std::uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }
  const xdg_wm_base_listener kWmBaseListener = {.ping = xdgWmBasePing};
} // namespace

WaylandConnection::~WaylandConnection() { cleanup(); }

bool WaylandConnection::initialize(std::string& error) {
  m_display = wl_display_connect(nullptr);
  if (m_display == nullptr) {
    error = std::string("cannot connect to Wayland: ") + std::strerror(errno);
    return false;
  }
  m_registry = wl_display_get_registry(m_display);
  if (m_registry == nullptr) {
    error = "cannot obtain the Wayland registry";
    return false;
  }
  wl_registry_add_listener(m_registry, &kRegistryListener, this);
  if (wl_display_roundtrip(m_display) < 0 || wl_display_roundtrip(m_display) < 0) {
    error = "Wayland connection failed during registry discovery";
    return false;
  }
  if (m_compositor == nullptr || m_xdgWmBase == nullptr) {
    error = "the compositor must provide wl_compositor version 4 and xdg-shell";
    return false;
  }
  m_input.setCursorTheme(m_compositor, m_shm);
  return true;
}

void WaylandConnection::handleGlobal(
    void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version
) {
  auto& self = *static_cast<WaylandConnection*>(data);
  const std::string_view id(interface);
  if (id == wl_compositor_interface.name && version >= 4) {
    self.m_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
  } else if (id == wl_seat_interface.name && self.m_seat == nullptr) {
    self.m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 8U)));
    self.m_input.bind(self.m_seat);
  } else if (id == wl_data_device_manager_interface.name) {
    self.m_dataDeviceManager = static_cast<wl_data_device_manager*>(
        wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3U))
    );
  } else if (id == wl_shm_interface.name) {
    self.m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
  } else if (id == xdg_wm_base_interface.name) {
    self.m_xdgWmBase =
        static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 6U)));
    xdg_wm_base_add_listener(self.m_xdgWmBase, &kWmBaseListener, &self);
  } else if (id == wp_viewporter_interface.name) {
    self.m_viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
  } else if (id == wp_fractional_scale_manager_v1_interface.name) {
    self.m_fractionalScale = static_cast<wp_fractional_scale_manager_v1*>(
        wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1)
    );
  } else if (id == wp_cursor_shape_manager_v1_interface.name) {
    self.m_cursorShape = static_cast<wp_cursor_shape_manager_v1*>(
        wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1)
    );
    self.m_input.setCursorShapeManager(self.m_cursorShape);
  } else if (id == zwp_text_input_manager_v3_interface.name) {
    self.m_textInputManager = static_cast<zwp_text_input_manager_v3*>(
        wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1)
    );
  } else if (id == zxdg_decoration_manager_v1_interface.name) {
    self.m_decorations = static_cast<zxdg_decoration_manager_v1*>(
        wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1)
    );
  } else if (id == wl_output_interface.name) {
    auto* output =
        static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U)));
    self.m_outputs.push_back({.name = name, .output = output, .connectorName = {}, .description = {}});
    wl_output_add_listener(output, &kOutputListener, &self);
  }
}

void WaylandConnection::handleGlobalRemove(void* data, wl_registry*, std::uint32_t name) {
  auto& self = *static_cast<WaylandConnection*>(data);
  auto it = std::ranges::find(self.m_outputs, name, &WaylandOutput::name);
  if (it == self.m_outputs.end()) {
    return;
  }
  auto* removed = it->output;
  for (auto& [surface, outputs] : self.m_surfaceOutputs) {
    (void)surface;
    std::erase(outputs, removed);
  }
  self.m_outputs.erase(it);
  if (self.m_outputChanged) {
    self.m_outputChanged(removed);
  }
  if (wl_output_get_version(removed) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
    wl_output_release(removed);
  } else {
    wl_output_destroy(removed);
  }
}

void WaylandConnection::handleOutputScale(void* data, wl_output* output, std::int32_t factor) {
  auto& self = *static_cast<WaylandConnection*>(data);
  if (auto* out = self.findOutputByWl(output)) {
    out->scale = std::max(1, factor);
    out->configuredScaleNumerator = out->scale * 120;
  }
}

void WaylandConnection::handleOutputDone(void* data, wl_output* output) {
  auto& self = *static_cast<WaylandConnection*>(data);
  if (self.m_outputChanged) {
    self.m_outputChanged(output);
  }
}

void WaylandConnection::handleOutputName(void* data, wl_output* output, const char* name) {
  if (auto* out = static_cast<WaylandConnection*>(data)->findOutputByWl(output)) {
    out->connectorName = name;
  }
}

void WaylandConnection::handleOutputDescription(void* data, wl_output* output, const char* description) {
  if (auto* out = static_cast<WaylandConnection*>(data)->findOutputByWl(output)) {
    out->description = description;
  }
}

WaylandOutput* WaylandConnection::findOutputByWl(wl_output* output) {
  auto it = std::ranges::find(m_outputs, output, &WaylandOutput::output);
  return it == m_outputs.end() ? nullptr : &*it;
}

const WaylandOutput* WaylandConnection::findOutputByWl(wl_output* output) const {
  auto it = std::ranges::find(m_outputs, output, &WaylandOutput::output);
  return it == m_outputs.end() ? nullptr : &*it;
}

wl_output* WaylandConnection::outputForSurface(wl_surface* surface) const {
  const auto it = m_surfaceOutputs.find(surface);
  if (it == m_surfaceOutputs.end() || it->second.empty()) {
    return nullptr;
  }
  return *std::ranges::max_element(it->second, {}, [this](wl_output* out) {
    const auto* info = findOutputByWl(out);
    return info != nullptr ? info->scale : 1;
  });
}

void WaylandConnection::registerSurfaceOutput(wl_surface* surface, wl_output* output) {
  if (output != nullptr) {
    notifySurfaceOutputEnter(surface, output);
  }
}

void WaylandConnection::notifySurfaceOutputEnter(wl_surface* surface, wl_output* output) {
  auto& outputs = m_surfaceOutputs[surface];
  if (!std::ranges::contains(outputs, output)) {
    outputs.push_back(output);
  }
}

void WaylandConnection::notifySurfaceOutputLeave(wl_surface* surface, wl_output* output) {
  if (auto it = m_surfaceOutputs.find(surface); it != m_surfaceOutputs.end()) {
    std::erase(it->second, output);
  }
}

void WaylandConnection::unregisterSurface(wl_surface* surface) {
  m_input.forgetSurface(surface);
  m_surfaceOutputs.erase(surface);
}

void WaylandConnection::setOutputChangedCallback(std::function<void(wl_output*)> callback) {
  m_outputChanged = std::move(callback);
}

void WaylandConnection::bindTextInput(TextInputService& service) {
  m_textInput = &service;
  if (m_textInputManager != nullptr && m_seat != nullptr) {
    service.bind(m_textInputManager, m_seat);
  }
}

void WaylandConnection::cleanup() {
  if (m_textInput != nullptr) {
    m_textInput->cleanup();
  }
  m_input.cleanup();
  for (auto& output : m_outputs) {
    wl_output_destroy(output.output);
  }
  if (m_decorations != nullptr) {
    zxdg_decoration_manager_v1_destroy(m_decorations);
  }
  if (m_textInputManager != nullptr) {
    zwp_text_input_manager_v3_destroy(m_textInputManager);
  }
  if (m_cursorShape != nullptr) {
    wp_cursor_shape_manager_v1_destroy(m_cursorShape);
  }
  if (m_fractionalScale != nullptr) {
    wp_fractional_scale_manager_v1_destroy(m_fractionalScale);
  }
  if (m_viewporter != nullptr) {
    wp_viewporter_destroy(m_viewporter);
  }
  if (m_xdgWmBase != nullptr) {
    xdg_wm_base_destroy(m_xdgWmBase);
  }
  if (m_seat != nullptr) {
    wl_seat_destroy(m_seat);
  }
  if (m_dataDeviceManager != nullptr)
    wl_data_device_manager_destroy(m_dataDeviceManager);
  if (m_shm != nullptr) {
    wl_shm_destroy(m_shm);
  }
  if (m_compositor != nullptr) {
    wl_compositor_destroy(m_compositor);
  }
  if (m_registry != nullptr) {
    wl_registry_destroy(m_registry);
  }
  if (m_display != nullptr) {
    wl_display_disconnect(m_display);
  }
}
