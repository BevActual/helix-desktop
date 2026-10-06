#include "render/surface/popup_surface.h"

#include "core/log.h"
#include "wayland/popup_dismiss_order.h"
#include "wayland/wayland_connection.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("wayland");

  const xdg_surface_listener kXdgSurfaceListener = {
      .configure = &PopupSurface::handleXdgSurfaceConfigure,
  };

  const xdg_popup_listener kPopupListener = {
      .configure = &PopupSurface::handlePopupConfigure,
      .popup_done = &PopupSurface::handlePopupDone,
      .repositioned = &PopupSurface::handlePopupRepositioned,
  };

  xdg_positioner* createPositioner(xdg_wm_base* wmBase, const PopupSurfaceConfig& config) {
    if (wmBase == nullptr) {
      return nullptr;
    }

    xdg_positioner* positioner = xdg_wm_base_create_positioner(wmBase);
    if (positioner == nullptr) {
      return nullptr;
    }

    xdg_positioner_set_size(
        positioner, static_cast<std::int32_t>(config.width), static_cast<std::int32_t>(config.height)
    );
    xdg_positioner_set_anchor_rect(positioner, config.anchorX, config.anchorY, config.anchorWidth, config.anchorHeight);
    xdg_positioner_set_anchor(positioner, config.anchor);
    xdg_positioner_set_gravity(positioner, config.gravity);
    xdg_positioner_set_constraint_adjustment(positioner, config.constraintAdjustment);
    xdg_positioner_set_offset(positioner, config.offsetX, config.offsetY);
    if (config.reactive && xdg_positioner_get_version(positioner) >= XDG_POSITIONER_SET_REACTIVE_SINCE_VERSION) {
      xdg_positioner_set_reactive(positioner);
    }
    return positioner;
  }

  // Popups holding an explicit xdg_popup_grab, oldest first. xdg-shell only allows a new
  // grab on a popup whose parent is the topmost grabbing popup; violating that is a fatal
  // protocol error (niri: "xdg_popup was not created on the topmost popup").
  std::vector<const PopupSurface*>& grabStack() {
    static std::vector<const PopupSurface*> stack;
    return stack;
  }

  void removeFromGrabStack(const PopupSurface* popup) {
    auto& stack = grabStack();
    stack.erase(std::remove(stack.begin(), stack.end(), popup), stack.end());
  }

  // Every popup with a live xdg_popup, grabbing or not, so a new grab can clear the popups
  // of other components (dock, tray, bar menus, tooltips) before it asks for the grab.
  std::vector<PopupSurface*>& livePopups() {
    static std::vector<PopupSurface*> popups;
    return popups;
  }

} // namespace

PopupSurface::PopupSurface(WaylandConnection& connection) : Surface(connection) {}

PopupSurface::~PopupSurface() {
  *m_alive = false;
  if (m_popup != nullptr) {
    kLog.debug("popup {}: destroyed while mapped", static_cast<const void*>(this));
  }
  m_connection.unregisterSurface(m_surface);
  destroyRoleObjects();
}

void PopupSurface::wireGrab(xdg_surface* parentXdgSurface) {
  if (m_config.grab && m_config.serial != 0 && m_connection.seat() != nullptr && m_popup != nullptr) {
    auto grabIsLegal = [parentXdgSurface]() {
      const auto& stack = grabStack();
      return stack.empty() || (parentXdgSurface != nullptr && stack.back()->m_xdgSurface == parentXdgSurface);
    };
    if (!grabIsLegal()) {
      // Another component's grabbing popup is still open (the dock opening its menu while a
      // bar or tray menu is up, or input queued across a stall/resume). Dismiss everything
      // outside this popup's parent chain, as the compositor would on an outside click.
      dismissPopupsOutsideChainOf(m_parentPopup);
    }
    if (!grabIsLegal()) {
      // Still not on the topmost grab (parent is a non-grabbing popup under a grab). Show
      // this one without a grab rather than killing the connection.
      kLog.warn("popup: skipping xdg_popup_grab, parent is not the topmost grabbing popup");
      return;
    }
    xdg_popup_grab(m_popup, m_connection.seat(), m_config.serial);
    grabStack().push_back(this);
    kLog.debug(
        "popup {}: grab serial={} parent={} grabs={}", static_cast<const void*>(this), m_config.serial,
        static_cast<const void*>(m_parentPopup), grabStack().size()
    );
  }
}

void PopupSurface::dismissPopupsOutsideChainOf(PopupSurface* keepParent) {
  std::vector<PopupSurface*> others;
  for (PopupSurface* popup : livePopups()) {
    if (popup != this) {
      others.push_back(popup);
    }
  }

  std::vector<popup_dismiss_order::Node> nodes(others.size());
  std::optional<std::size_t> keepIndex;
  for (std::size_t i = 0; i < others.size(); ++i) {
    if (others[i] == keepParent) {
      keepIndex = i;
    }
    const auto parent = std::find(others.begin(), others.end(), others[i]->m_parentPopup);
    if (parent != others.end()) {
      nodes[i].parent = static_cast<std::size_t>(parent - others.begin());
    }
  }

  // Tear down every conflicting xdg_popup first (deepest first, which is the only legal
  // order), then tell the owners. Some owners (tray) close synchronously and could otherwise
  // destroy a parent before we got to its children.
  std::vector<std::function<void()>> callbacks;
  for (std::size_t index : popup_dismiss_order::conflicting(nodes, keepIndex)) {
    PopupSurface* popup = others[index];
    popup->setRunning(false);
    popup->destroyRoleObjects();
    if (popup->m_dismissedCallback) {
      callbacks.push_back(std::move(popup->m_dismissedCallback));
      popup->m_dismissedCallback = nullptr;
    }
  }
  if (!callbacks.empty()) {
    kLog.debug("popup: dismissed {} popup(s) to open a new grabbing popup", callbacks.size());
  }
  for (auto& callback : callbacks) {
    callback();
  }
}

bool PopupSurface::initialize(zwlr_layer_surface_v1* parentLayerSurface, wl_output* output, PopupSurfaceConfig config) {
  if (parentLayerSurface == nullptr || !m_connection.hasXdgShell()) {
    kLog.warn("context menu skipped: missing layer parent or xdg-shell");
    return false;
  }

  std::int32_t bufferScale = 1;
  if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
    bufferScale = wlOutput->scale;
    setConfiguredScaleNumerator(
        wlOutput->configuredScaleNumerator > 0 ? static_cast<std::uint32_t>(wlOutput->configuredScaleNumerator) : 1U
    );
  }
  setBufferScale(bufferScale);

  if (!createWlSurface()) {
    return false;
  }

  m_connection.registerSurfaceOutput(m_surface, output);

  m_config = config;
  m_config.anchorWidth = std::max(m_config.anchorWidth, 1);
  m_config.anchorHeight = std::max(m_config.anchorHeight, 1);
  m_config.width = std::max(m_config.width, 1U);
  m_config.height = std::max(m_config.height, 1U);

  m_pendingWidth = m_config.width;
  m_pendingHeight = m_config.height;

  m_xdgSurface = xdg_wm_base_get_xdg_surface(m_connection.xdgWmBase(), m_surface);
  if (m_xdgSurface == nullptr) {
    destroySurface();
    return false;
  }
  xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);

  xdg_positioner* positioner = createPositioner(m_connection.xdgWmBase(), m_config);
  if (positioner == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  m_popup = xdg_surface_get_popup(m_xdgSurface, nullptr, positioner);
  xdg_positioner_destroy(positioner);
  if (m_popup == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  xdg_popup_add_listener(m_popup, &kPopupListener, this);
  zwlr_layer_surface_v1_get_popup(parentLayerSurface, m_popup);
  m_parentPopup = nullptr;
  livePopups().push_back(this);

  wireGrab(nullptr);

  wl_surface_commit(m_surface);
  // Flush before the roundtrip to ensure any pending destroy messages from a previous
  // popup are delivered to the compositor before we ask it to configure the new one.
  wl_display_flush(m_connection.display());
  // The roundtrip pumps the event queue, which can re-enter input dispatch and destroy
  // this popup mid-init (e.g. the owning menu closes on the same press that opened it).
  // Hold a liveness token so we never touch freed `this` once the roundtrip returns.
  auto alive = m_alive;
  const int roundtripResult = wl_display_roundtrip(m_connection.display());
  if (!*alive) {
    return false;
  }
  if (roundtripResult >= 0 && m_popup == nullptr) {
    // Dismissed during setup (a popup opened re-entrantly from queued input took the grab).
    kLog.debug("popup {}: dismissed before setup finished", static_cast<const void*>(this));
    destroySurface();
    return false;
  }
  if (roundtripResult < 0) {
    kLog.warn("popup: initial roundtrip failed (compositor protocol error)");
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void PopupSurface::setDismissedCallback(std::function<void()> callback) { m_dismissedCallback = std::move(callback); }

bool PopupSurface::resize(std::uint32_t width, std::uint32_t height, bool commit) {
  if (m_surface == nullptr || m_popup == nullptr) {
    return false;
  }

  width = std::max(width, 1U);
  height = std::max(height, 1U);
  if (m_config.width == width
      && m_config.height == height
      && Surface::width() == width
      && Surface::height() == height) {
    return true;
  }

  m_config.width = width;
  m_config.height = height;
  m_pendingWidth = width;
  m_pendingHeight = height;

  // Update our render target immediately so async menu hydration does not leave
  // the next frame clipped to the placeholder popup size while waiting for the
  // compositor's reposition configure.
  Surface::onConfigure(width, height);

  if (xdg_popup_get_version(m_popup) >= XDG_POPUP_REPOSITION_SINCE_VERSION) {
    xdg_positioner* positioner = createPositioner(m_connection.xdgWmBase(), m_config);
    if (positioner != nullptr) {
      xdg_popup_reposition(m_popup, positioner, ++m_repositionToken);
      xdg_positioner_destroy(positioner);
    }
  }

  if (commit) {
    wl_surface_commit(m_surface);
    wl_display_flush(m_connection.display());
  }
  return true;
}

bool PopupSurface::repositionAnchor(const PopupSurfaceConfig& anchorConfig, bool commit) {
  if (m_popup == nullptr) {
    return false;
  }

  m_config.anchorX = anchorConfig.anchorX;
  m_config.anchorY = anchorConfig.anchorY;
  m_config.anchorWidth = std::max(anchorConfig.anchorWidth, 1);
  m_config.anchorHeight = std::max(anchorConfig.anchorHeight, 1);
  m_config.anchor = anchorConfig.anchor;
  m_config.gravity = anchorConfig.gravity;
  m_config.constraintAdjustment = anchorConfig.constraintAdjustment;
  m_config.offsetX = anchorConfig.offsetX;
  m_config.offsetY = anchorConfig.offsetY;

  if (xdg_popup_get_version(m_popup) >= XDG_POPUP_REPOSITION_SINCE_VERSION) {
    xdg_positioner* positioner = createPositioner(m_connection.xdgWmBase(), m_config);
    if (positioner != nullptr) {
      xdg_popup_reposition(m_popup, positioner, ++m_repositionToken);
      xdg_positioner_destroy(positioner);
    }
  }

  if (commit) {
    wl_surface_commit(m_surface);
    wl_display_flush(m_connection.display());
  }
  return true;
}

void PopupSurface::handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial) {
  auto* self = static_cast<PopupSurface*>(data);
  xdg_surface_ack_configure(surface, serial);

  const std::uint32_t width = self->m_pendingWidth == 0 ? self->m_config.width : self->m_pendingWidth;
  const std::uint32_t height = self->m_pendingHeight == 0 ? self->m_config.height : self->m_pendingHeight;
  self->Surface::onConfigure(std::max(1U, width), std::max(1U, height));
}

void PopupSurface::handlePopupConfigure(
    void* data, xdg_popup* /*popup*/, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height
) {
  auto* self = static_cast<PopupSurface*>(data);
  self->m_configuredX = x;
  self->m_configuredY = y;
  if (width > 0) {
    self->m_pendingWidth = static_cast<std::uint32_t>(width);
  }
  if (height > 0) {
    self->m_pendingHeight = static_cast<std::uint32_t>(height);
  }
}

void PopupSurface::handlePopupDone(void* data, xdg_popup* popup) {
  auto* self = static_cast<PopupSurface*>(data);
  if (self->m_popup == nullptr || self->m_popup != popup) {
    return;
  }
  kLog.debug("popup {}: popup_done", static_cast<const void*>(self));
  self->setRunning(false);
  removeFromGrabStack(self);
  auto callback = std::move(self->m_dismissedCallback);
  if (callback) {
    callback();
  }
}

void PopupSurface::handlePopupRepositioned(void* /*data*/, xdg_popup* /*popup*/, std::uint32_t /*token*/) {}

bool PopupSurface::initializeAsChild(xdg_surface* parentXdgSurface, wl_output* output, PopupSurfaceConfig config) {
  if (parentXdgSurface == nullptr || !m_connection.hasXdgShell()) {
    kLog.warn("submenu popup skipped: missing parent xdg_surface or xdg-shell");
    return false;
  }

  std::int32_t bufferScale = 1;
  if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
    bufferScale = wlOutput->scale;
    setConfiguredScaleNumerator(
        wlOutput->configuredScaleNumerator > 0 ? static_cast<std::uint32_t>(wlOutput->configuredScaleNumerator) : 1U
    );
  }
  setBufferScale(bufferScale);

  if (!createWlSurface()) {
    return false;
  }

  m_connection.registerSurfaceOutput(m_surface, output);

  m_config = config;
  m_config.anchorWidth = std::max(m_config.anchorWidth, 1);
  m_config.anchorHeight = std::max(m_config.anchorHeight, 1);
  m_config.width = std::max(m_config.width, 1U);
  m_config.height = std::max(m_config.height, 1U);

  m_pendingWidth = m_config.width;
  m_pendingHeight = m_config.height;

  m_xdgSurface = xdg_wm_base_get_xdg_surface(m_connection.xdgWmBase(), m_surface);
  if (m_xdgSurface == nullptr) {
    destroySurface();
    return false;
  }
  xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);

  xdg_positioner* positioner = createPositioner(m_connection.xdgWmBase(), m_config);
  if (positioner == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  // popup-of-popup: pass the parent xdg_surface directly
  m_popup = xdg_surface_get_popup(m_xdgSurface, parentXdgSurface, positioner);
  xdg_positioner_destroy(positioner);
  if (m_popup == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  xdg_popup_add_listener(m_popup, &kPopupListener, this);
  m_parentPopup = nullptr;
  for (PopupSurface* popup : livePopups()) {
    if (popup->m_xdgSurface == parentXdgSurface) {
      m_parentPopup = popup;
      break;
    }
  }
  livePopups().push_back(this);

  wireGrab(parentXdgSurface);

  wl_surface_commit(m_surface);
  wl_display_flush(m_connection.display());
  // The roundtrip pumps the event queue, which can re-enter input dispatch and destroy
  // this popup mid-init (e.g. the owning menu closes on the same press that opened it).
  // Hold a liveness token so we never touch freed `this` once the roundtrip returns.
  auto alive = m_alive;
  const int roundtripResult = wl_display_roundtrip(m_connection.display());
  if (!*alive) {
    return false;
  }
  if (roundtripResult >= 0 && m_popup == nullptr) {
    // Dismissed during setup (a popup opened re-entrantly from queued input took the grab).
    kLog.debug("popup {}: dismissed before setup finished", static_cast<const void*>(this));
    destroySurface();
    return false;
  }
  if (roundtripResult < 0) {
    kLog.warn("submenu popup: initial roundtrip failed (compositor protocol error)");
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void PopupSurface::destroyRoleObjects() {
  removeFromGrabStack(this);
  auto& live = livePopups();
  live.erase(std::remove(live.begin(), live.end(), this), live.end());
  for (PopupSurface* popup : live) {
    if (popup->m_parentPopup == this) {
      popup->m_parentPopup = nullptr;
    }
  }
  if (m_popup != nullptr) {
    xdg_popup* popup = m_popup;
    m_popup = nullptr;
    xdg_popup_destroy(popup);
  }
  if (m_xdgSurface != nullptr) {
    xdg_surface* surface = m_xdgSurface;
    m_xdgSurface = nullptr;
    xdg_surface_destroy(surface);
  }
}
