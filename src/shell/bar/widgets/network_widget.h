#pragma once

#include "dbus/network/inetwork_service.h"
#include "shell/bar/widget.h"
#include "render/scene/tooltip_content.h"

#include <string>
#include <vector>

class Glyph;
class Label;
class Spinner;
class SystemMonitorService;
struct wl_output;

enum class VpnStatusMode : std::uint8_t {
  Both,    // Show VPN icon next to network icon
  Replace, // Replace network icon with VPN icon when active
  Hidden,  // Don't show VPN icon
};

class NetworkWidget : public Widget {
public:
  struct Options {
    VpnStatusMode vpnStatusMode = VpnStatusMode::Replace;
    bool showLabel = false; // icon-only in the status capsule (enhanced GNOME)
    bool showVpnLabel = false;
  };

  NetworkWidget(INetworkService* network, SystemMonitorService* monitor, wl_output* output, Options options);

  void create() override;
  void setBarForceIconOnly(bool force) override {
    if (force) {
      m_showLabel = false;
    }
  }

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  [[nodiscard]] std::vector<TooltipRow> buildTooltipRows() const;

  INetworkService* m_network = nullptr;
  SystemMonitorService* m_monitor = nullptr;
  bool m_showLabel = false;
  bool m_showVpnLabel = false;
  VpnStatusMode m_vpnStatusMode = VpnStatusMode::Replace;
  Glyph* m_glyph = nullptr;
  Glyph* m_vpnGlyph = nullptr;
  Spinner* m_spinner = nullptr;
  Label* m_label = nullptr;
  Label* m_vpnLabel = nullptr;
  NetworkState m_lastState;
  bool m_haveLastState = false;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
