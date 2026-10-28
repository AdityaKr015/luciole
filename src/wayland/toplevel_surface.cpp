#include "wayland/toplevel_surface.h"

#include "core/log.h"
#include "wayland/wayland_connection.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <span>
#include <utility>

namespace {

  constexpr Logger kLog("wayland");

  const xdg_surface_listener kXdgSurfaceListener = {
      .configure = &ToplevelSurface::handleXdgSurfaceConfigure,
  };

  const xdg_toplevel_listener kToplevelListener = {
      .configure = &ToplevelSurface::handleToplevelConfigure,
      .close = &ToplevelSurface::handleToplevelClose,
      .configure_bounds = &ToplevelSurface::handleToplevelConfigureBounds,
      .wm_capabilities = &ToplevelSurface::handleToplevelWmCapabilities,
  };

  void decorationConfigure(void* data, zxdg_toplevel_decoration_v1*, std::uint32_t mode) {
    static_cast<ToplevelSurface*>(data)->configureDecoration(mode);
  }
  const zxdg_toplevel_decoration_v1_listener kDecorationListener = {.configure = decorationConfigure};

} // namespace

ToplevelSurface::ToplevelSurface(WaylandConnection& connection) : Surface(connection) {}

ToplevelSurface::~ToplevelSurface() {
  m_connection.unregisterSurface(m_surface);
  destroyRoleObjects();
}

bool ToplevelSurface::initialize(wl_output* output, const ToplevelSurfaceConfig& config) {
  if (!m_connection.hasXdgShell()) {
    kLog.warn("toplevel: missing xdg-shell");
    return false;
  }

  if (!createWlSurface()) {
    return false;
  }

  std::int32_t bufferScale = 1;
  if (output != nullptr) {
    if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
      bufferScale = wlOutput->scale;
    }
    m_connection.registerSurfaceOutput(m_surface, output);
  }
  setBufferScale(bufferScale);

  m_pendingWidth = std::max<std::uint32_t>(1, config.width);
  m_pendingHeight = std::max<std::uint32_t>(1, config.height);
  m_lastToplevelWidth = 0;
  m_lastToplevelHeight = 0;

  m_xdgSurface = xdg_wm_base_get_xdg_surface(m_connection.xdgWmBase(), m_surface);
  if (m_xdgSurface == nullptr) {
    destroySurface();
    return false;
  }
  xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);

  m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
  if (m_toplevel == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  xdg_toplevel_add_listener(m_toplevel, &kToplevelListener, this);
  xdg_toplevel_set_title(m_toplevel, config.title.c_str());
  xdg_toplevel_set_app_id(m_toplevel, config.appId);
  if (config.minWidth > 0 || config.minHeight > 0) {
    xdg_toplevel_set_min_size(
        m_toplevel, static_cast<std::int32_t>(config.minWidth), static_cast<std::int32_t>(config.minHeight)
    );
  }

  if (auto* manager = m_connection.decorationManager(); manager != nullptr) {
    m_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(manager, m_toplevel);
    if (m_decoration != nullptr) {
      zxdg_toplevel_decoration_v1_add_listener(m_decoration, &kDecorationListener, this);
      zxdg_toplevel_decoration_v1_set_mode(m_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
  }
  setRunning(true);
  wl_surface_commit(m_surface);
  return true;
}

void ToplevelSurface::setClosedCallback(std::function<void()> callback) { m_closedCallback = std::move(callback); }

void ToplevelSurface::setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_min_size(m_toplevel, static_cast<std::int32_t>(minWidth), static_cast<std::int32_t>(minHeight));
  }
}

void ToplevelSurface::beginMove(std::uint32_t serial) {
  if (m_toplevel != nullptr && m_connection.seat() != nullptr && serial != 0) {
    xdg_toplevel_move(m_toplevel, m_connection.seat(), serial);
  }
}

void ToplevelSurface::beginResize(std::uint32_t serial, std::uint32_t edges) {
  if (m_toplevel != nullptr && m_connection.seat() != nullptr && serial != 0) {
    xdg_toplevel_resize(m_toplevel, m_connection.seat(), serial, edges);
  }
}
void ToplevelSurface::minimize() {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_minimized(m_toplevel);
  }
}
void ToplevelSurface::toggleMaximized() {
  if (m_toplevel == nullptr) {
    return;
  }
  if (m_maximized) {
    xdg_toplevel_unset_maximized(m_toplevel);
  } else {
    xdg_toplevel_set_maximized(m_toplevel);
  }
}
void ToplevelSurface::toggleFullscreen() {
  if (m_toplevel == nullptr) {
    return;
  }
  if (m_fullscreen) {
    xdg_toplevel_unset_fullscreen(m_toplevel);
  } else {
    xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
  }
}
void ToplevelSurface::setTitle(const std::string& title) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_title(m_toplevel, title.c_str());
  }
}
void ToplevelSurface::showWindowMenu(std::uint32_t serial, std::int32_t x, std::int32_t y) {
  if (m_toplevel != nullptr && m_connection.seat() != nullptr && serial != 0) {
    xdg_toplevel_show_window_menu(m_toplevel, m_connection.seat(), serial, x, y);
  }
}
void ToplevelSurface::close() {
  setRunning(false);
  if (m_closedCallback) {
    m_closedCallback();
  }
}
void ToplevelSurface::configureDecoration(std::uint32_t mode) {
  m_serverDecorated = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
  requestLayout();
}

void ToplevelSurface::handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial) {
  auto* self = static_cast<ToplevelSurface*>(data);
  xdg_surface_ack_configure(surface, serial);
  self->m_maximized = self->m_pendingMaximized;
  self->m_fullscreen = self->m_pendingFullscreen;
  self->m_activated = self->m_pendingActivated;

  std::uint32_t width = self->m_pendingWidth;
  std::uint32_t height = self->m_pendingHeight;
  if (self->m_lastToplevelWidth > 0) {
    width = static_cast<std::uint32_t>(self->m_lastToplevelWidth);
  }
  if (self->m_lastToplevelHeight > 0) {
    height = static_cast<std::uint32_t>(self->m_lastToplevelHeight);
  }
  width = std::max<std::uint32_t>(1, width);
  height = std::max<std::uint32_t>(1, height);
  self->Surface::onConfigure(width, height);
}

void ToplevelSurface::handleToplevelConfigure(
    void* data, xdg_toplevel* /*toplevel*/, std::int32_t width, std::int32_t height, wl_array* states
) {
  auto* self = static_cast<ToplevelSurface*>(data);
  const std::span values(static_cast<const std::uint32_t*>(states->data), states->size / sizeof(std::uint32_t));
  self->m_pendingMaximized = std::ranges::contains(values, XDG_TOPLEVEL_STATE_MAXIMIZED);
  self->m_pendingFullscreen = std::ranges::contains(values, XDG_TOPLEVEL_STATE_FULLSCREEN);
  self->m_pendingActivated = std::ranges::contains(values, XDG_TOPLEVEL_STATE_ACTIVATED);
  self->m_lastToplevelWidth = width;
  self->m_lastToplevelHeight = height;
  if (width > 0) {
    self->m_pendingWidth = static_cast<std::uint32_t>(width);
  }
  if (height > 0) {
    self->m_pendingHeight = static_cast<std::uint32_t>(height);
  }
}

void ToplevelSurface::handleToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
  auto* self = static_cast<ToplevelSurface*>(data);
  self->setRunning(false);
  if (self->m_closedCallback) {
    self->m_closedCallback();
  }
}

void ToplevelSurface::handleToplevelConfigureBounds(
    void* /*data*/, xdg_toplevel* /*toplevel*/, std::int32_t /*width*/, std::int32_t /*height*/
) {}

void ToplevelSurface::
    handleToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/, wl_array* /*capabilities*/) {}

void ToplevelSurface::destroyRoleObjects() {
  if (m_decoration != nullptr) {
    zxdg_toplevel_decoration_v1_destroy(m_decoration);
    m_decoration = nullptr;
  }
  if (m_toplevel != nullptr) {
    xdg_toplevel_destroy(m_toplevel);
    m_toplevel = nullptr;
  }
  if (m_xdgSurface != nullptr) {
    xdg_surface_destroy(m_xdgSurface);
    m_xdgSurface = nullptr;
  }
}
