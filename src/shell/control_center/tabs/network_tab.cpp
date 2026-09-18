#include "shell/control_center/tabs/network_tab.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/ui_phase.h"
#include "dbus/modem/modem_manager_service.h"
#include "dbus/network/external_ip_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/qr_code.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr float kRowMinHeight = Style::controlHeightLg;

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  // Label and wire value in one row, so the visible dropdown order and the value
  // sent to NetworkManager cannot drift apart.
  struct EapOption {
    std::string_view label;
    network_enterprise::EapMethod method;
  };

  struct Phase2Option {
    std::string_view label;
    network_enterprise::Phase2Auth auth;
  };

  constexpr std::array kEapOptions{
      EapOption{"PEAP", network_enterprise::EapMethod::Peap},
      EapOption{"TTLS", network_enterprise::EapMethod::Ttls},
  };

  // MSCHAPv2 first: it is the only inner method PEAP accepts, and PEAP is the
  // default EAP method.
  constexpr std::array kPhase2Options{
      Phase2Option{"MSCHAPv2", network_enterprise::Phase2Auth::MsChapV2},
      Phase2Option{"PAP", network_enterprise::Phase2Auth::Pap},
      Phase2Option{"MSCHAP", network_enterprise::Phase2Auth::MsChap},
      Phase2Option{"CHAP", network_enterprise::Phase2Auth::Chap},
  };

  constexpr std::size_t phase2IndexOf(network_enterprise::Phase2Auth auth) {
    for (std::size_t index = 0U; index < kPhase2Options.size(); ++index) {
      if (kPhase2Options[index].auth == auth) {
        return index;
      }
    }
    return kPhase2Options.size();
  }

  // Both dropdowns open on their first option, and PEAP accepts MSCHAPv2 only, so
  // choosing PEAP pins the inner dropdown to this row.
  constexpr std::size_t kDefaultOptionIndex = 0U;
  constexpr std::size_t kPeapPhase2Index = phase2IndexOf(network_enterprise::Phase2Auth::MsChapV2);
  static_assert(kPeapPhase2Index < kPhase2Options.size());
  static_assert(kEapOptions[kDefaultOptionIndex].method == network_enterprise::EapMethod::Peap);
  static_assert(kPeapPhase2Index == kDefaultOptionIndex);

  template <typename Options> std::vector<std::string> optionLabels(const Options& options) {
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& option : options) {
      labels.emplace_back(option.label);
    }
    return labels;
  }

  // A Select with no selection reports npos; the first option is the documented
  // default for both dropdowns.
  template <typename Options> const auto& optionAt(const Options& options, std::size_t index) {
    return index < options.size() ? options[index] : options.front();
  }

  std::string validationMessage(network_enterprise::Validation problem) {
    switch (problem) {
    case network_enterprise::Validation::MissingIdentity:
      return i18n::tr("control-center.network.error-missing-identity");
    case network_enterprise::Validation::MissingPassword:
      return i18n::tr("control-center.network.error-missing-password");
    case network_enterprise::Validation::UnsupportedPhase2:
      return i18n::tr("control-center.network.error-unsupported-phase2");
    case network_enterprise::Validation::CaCertNotAbsolute:
      return i18n::tr("control-center.network.error-ca-not-absolute");
    case network_enterprise::Validation::Ok:
      break;
    }
    return {};
  }

  std::string currentTitle(const NetworkState& s, const CellularModemInfo* modem) {
    if (s.kind == NetworkConnectivity::Wireless && s.connected && !s.ssid.empty()) {
      return s.ssid;
    }
    if (s.kind == NetworkConnectivity::Wired && s.connected) {
      return s.interfaceName.empty() ? i18n::tr("control-center.network.wired-connection") : s.interfaceName;
    }
    if (s.kind == NetworkConnectivity::Cellular && s.connected) {
      if (modem != nullptr && !modem->operatorName.empty()) {
        return modem->operatorName;
      }
      return i18n::tr("control-center.network.cellular-connection");
    }
    return i18n::tr("control-center.network.not-connected");
  }

  std::string currentDetail(const NetworkState& s, const std::string& externalIp, const CellularModemInfo* modem) {
    if (!s.connected) {
      if (s.kind == NetworkConnectivity::Cellular && modem != nullptr) {
        return cellularStateText(modem->state);
      }
      return s.wirelessEnabled ? i18n::tr("control-center.network.wifi-on")
                               : i18n::tr("control-center.network.wifi-off");
    }
    std::string out;
    const auto append = [&out](std::string_view part) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += part;
    };
    if (!s.ipv4.empty()) {
      append(s.ipv4);
    }
    if (s.kind == NetworkConnectivity::Wireless) {
      if (s.signalStrength > 0) {
        append(std::to_string(static_cast<int>(s.signalStrength)) + "%");
      }
      if (const char* band = network_display::wifiFrequencyBandLabel(s.frequencyMhz); band != nullptr) {
        append(band);
      }
    }
    if (s.kind == NetworkConnectivity::Cellular && modem != nullptr) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += std::to_string(static_cast<int>(modem->signalQuality)) + "%";
      if (const char* tech = cellularAccessTechnologyName(modem->accessTechnologies); tech[0] != '\0') {
        out += "  •  ";
        out += tech;
      }
    }
    if (!externalIp.empty()) {
      append(i18n::tr("control-center.network.external-ip", "ip", externalIp));
    }
    return out;
  }

  const char* cellularGlyphFor(const CellularModemInfo& modem) {
    return modem.enabled() ? network_display::cellularGlyphForSignal(modem.signalQuality)
                           : network_display::cellularOffGlyph();
  }

  std::string cellularTitleFor(const CellularModemInfo& modem) {
    if (!modem.operatorName.empty()) {
      return modem.operatorName;
    }
    if (!modem.name.empty()) {
      return modem.name;
    }
    return i18n::tr("control-center.network.cellular");
  }

  std::string cellularDetailFor(const CellularModemInfo& modem) {
    std::string out = cellularStateText(modem.state);
    if (modem.enabled()) {
      if (const char* tech = cellularAccessTechnologyName(modem.accessTechnologies); tech[0] != '\0') {
        out += "  •  ";
        out += tech;
      }
      out += "  •  " + std::to_string(static_cast<int>(modem.signalQuality)) + "%";
    }
    return out;
  }

  std::string percentText(std::uint8_t percent) { return std::to_string(static_cast<int>(percent)) + "%"; }

  std::string_view trimView(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) {
      return {};
    }
    const auto end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
  }

  bool ssidEqualsIgnoreCase(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
      return std::tolower(a) == std::tolower(b);
    });
  }

  bool ssidIsHidden(std::string_view ssid, const std::vector<std::string>& hidden) {
    return std::ranges::any_of(hidden, [&](const std::string& item) { return ssidEqualsIgnoreCase(ssid, item); });
  }

  bool matchesWifiBand(std::uint32_t frequencyMhz, WifiBandFilter filter) {
    const auto band = network_display::wifiFrequencyBand(frequencyMhz);
    switch (filter) {
    case WifiBandFilter::All:
      return true;
    case WifiBandFilter::Ghz24:
      return band == network_display::WifiRadioBand::Ghz24;
    case WifiBandFilter::Ghz5:
      return band == network_display::WifiRadioBand::Ghz5;
    case WifiBandFilter::Ghz6:
      return band == network_display::WifiRadioBand::Ghz6;
    }
    return true;
  }

  std::string currentDnsText() {
    std::ifstream in("/etc/resolv.conf");
    if (!in) {
      return {};
    }
    std::vector<std::string> nameservers;
    std::string line;
    while (std::getline(in, line)) {
      const std::string_view trimmed = trimView(line);
      if (trimmed.empty() || trimmed.front() == '#') {
        continue;
      }
      constexpr std::string_view kPrefix = "nameserver";
      if (!trimmed.starts_with(kPrefix)) {
        continue;
      }
      const std::string_view rest = trimView(trimmed.substr(kPrefix.size()));
      if (!rest.empty()) {
        nameservers.emplace_back(rest);
      }
    }
    if (nameservers.empty()) {
      return {};
    }
    std::string joined;
    for (std::size_t i = 0; i < nameservers.size(); ++i) {
      if (i > 0) {
        joined += ", ";
      }
      joined += nameservers[i];
    }
    return i18n::tr("control-center.network.dns", "servers", joined);
  }

  std::string escapeWifiField(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
      if (c == '\\' || c == ';' || c == ',' || c == ':') {
        out.push_back('\\');
      }
      out.push_back(c);
    }
    return out;
  }

  std::string wifiJoinPayload(
      const NetworkState& state, bool secured, const std::optional<std::string>& psk
  ) {
    std::string payload = "WIFI:T:";
    payload += secured ? "WPA" : "nopass";
    payload += ";S:";
    payload += escapeWifiField(state.ssid);
    payload += ';';
    if (secured && psk.has_value() && !psk->empty()) {
      payload += "P:";
      payload += escapeWifiField(*psk);
      payload += ';';
    } else if (secured) {
      payload += "P:;";
    }
    payload += ';';
    return payload;
  }

  struct WifiShareKind {
    bool secured = true;
    bool enterprise = false;
  };

  WifiShareKind wifiShareKind(const NetworkState& state, const std::vector<AccessPointInfo>& aps) {
    WifiShareKind kind;
    for (const auto& ap : aps) {
      if (ap.active && ap.ssid == state.ssid) {
        kind.secured = ap.secured;
        kind.enterprise = ap.isEnterprise();
        return kind;
      }
    }
    return kind;
  }

  std::unique_ptr<Flex> makeWifiBucketHeaderRow(const std::string& title, float scale) {
    auto row = ui::row({
        .align = FlexAlign::Center,
    });

    auto pill = ui::row({
        .align = FlexAlign::Center,
        .paddingV = Style::spaceXs * 0.5F * scale,
        .paddingH = Style::spaceSm * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.8F),
        .radius = Style::scaledRadiusSm(scale),
    });
    pill->addChild(
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeMini * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    row->addChild(std::move(pill));
    return row;
  }

  // Active first, then by signal band, then by name. Ordering on the raw strength —
  // as the services do — reshuffles rows on every scan update, moving the row you are
  // aiming at out from under the pointer.
  std::vector<AccessPointInfo> sortedAccessPoints(std::vector<AccessPointInfo> aps) {
    std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
      if (a.active != b.active) {
        return a.active;
      }
      const int bandA = network_display::wifiSignalBand(a.strength);
      const int bandB = network_display::wifiSignalBand(b.strength);
      if (bandA != bandB) {
        return bandA > bandB;
      }
      return a.ssid < b.ssid;
    });
    return aps;
  }

} // namespace

class AccessPointRow : public Flex {
public:
  AccessPointRow(
      float scale, AccessPointInfo ap, bool saved, std::function<void(const AccessPointInfo&)> onActivate,
      std::function<void(const AccessPointInfo&)> onForget
  )
      : m_ap(std::move(ap)), m_onActivate(std::move(onActivate)), m_onForget(std::move(onForget)), m_scale(scale) {
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setGap(Style::spaceSm * scale);
    setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    applyGroupedListRowStyle(*this, scale, GroupedListRowEmphasis::None);

    addChild(
        ui::glyph({
            .out = &m_signalGlyph,
            .glyph = network_display::wifiGlyphForSignal(m_ap.strength),
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    addChild(
        ui::label({
            .out = &m_title,
            .text = m_ap.ssid,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = m_ap.active ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );

    if (m_ap.secured) {
      addChild(
          ui::glyph({
              .glyph = "lock",
              .glyphSize = Style::baseGlyphSize * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }

    addChild(
        ui::label({
            .out = &m_signalValue,
            .text = percentText(m_ap.strength),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    const float actionOpacity = (m_ap.active || saved) ? 1.0F : 0.0F;
    auto action = ui::button({
        .out = &m_actionButton,
        .glyphSize = Style::baseGlyphSize * scale,
        .variant = ButtonVariant::Ghost,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusSm(scale),
        .opacity = actionOpacity,
    });
    if (m_ap.active) {
      action->setGlyph("check");
    } else if (saved) {
      action->setGlyph("trash");
      action->setTooltip(i18n::tr("control-center.network.forget"));
      action->setOnClick([this]() {
        if (m_onForget) {
          m_onForget(m_ap);
        }
      });
    }
    addChild(std::move(action));

    auto area = ui::inputArea({});
    area->setPropagateEvents(true);
    area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnLeave([this]() { applyState(); });
    area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnClick([this](const InputArea::PointerData& /*data*/) {
      if (m_onActivate) {
        m_onActivate(m_ap);
      }
    });
    m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

    applyState();
    m_paletteConn = paletteChanged().connect([this] { applyState(); });
  }

  void doLayout(Renderer& renderer) override {
    if (m_inputArea == nullptr) {
      return;
    }
    m_inputArea->setVisible(false);
    Flex::doLayout(renderer);
    m_inputArea->setVisible(true);
    m_inputArea->setPosition(0.0F, 0.0F);
    m_inputArea->setSize(width(), height());
    if (m_actionButton != nullptr) {
      const float areaWidth = std::max(0.0F, m_actionButton->x() - gap());
      m_inputArea->setSize(areaWidth, height());
    }
    applyState();
  }

  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
    return measureByLayout(renderer, constraints);
  }

  void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  [[nodiscard]] const std::string& ssid() const noexcept { return m_ap.ssid; }

  // Refresh the values that move while a scan runs — the signal glyph and percent.
  // Everything else about the row is structural and belongs to the list key.
  // Returns true when a value actually changed, i.e. the row needs relayout.
  bool syncLiveMetrics(const AccessPointInfo& ap) {
    bool changed = false;
    if (m_signalGlyph != nullptr && m_signalGlyph->setGlyph(network_display::wifiGlyphForSignal(ap.strength))) {
      changed = true;
    }
    if (m_signalValue != nullptr && m_signalValue->setText(percentText(ap.strength))) {
      changed = true;
    }
    m_ap.strength = ap.strength;
    return changed;
  }

private:
  void applyState() {
    const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
    const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
    GroupedListRowEmphasis emphasis = GroupedListRowEmphasis::None;
    if (pressed) {
      emphasis = GroupedListRowEmphasis::Pressed;
    } else if (hov) {
      emphasis = GroupedListRowEmphasis::Hover;
    } else if (m_ap.active) {
      emphasis = GroupedListRowEmphasis::Selected;
    }
    applyGroupedListRowStyle(*this, m_scale, emphasis);
    if (m_title != nullptr) {
      m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
    }
  }

  AccessPointInfo m_ap;
  std::function<void(const AccessPointInfo&)> m_onActivate;
  std::function<void(const AccessPointInfo&)> m_onForget;
  float m_scale = 1.0F;
  Label* m_title = nullptr;
  Button* m_actionButton = nullptr;
  InputArea* m_inputArea = nullptr;
  Glyph* m_signalGlyph = nullptr;
  Label* m_signalValue = nullptr;
  Signal<>::ScopedConnection m_paletteConn;
};

namespace {

  class VpnConnectionRow : public Flex {
  public:
    VpnConnectionRow(
        float scale, VpnConnectionInfo vpn, std::function<void(const VpnConnectionInfo&)> onActivate,
        std::function<void(const VpnConnectionInfo&)> onDeactivate
    )
        : m_vpn(std::move(vpn)), m_onActivate(std::move(onActivate)), m_onDeactivate(std::move(onDeactivate)) {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(Style::spaceSm * scale);
      setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
      setMinHeight(kRowMinHeight * scale);
      setRadius(Style::scaledRadiusMd(scale));
      setFill(colorSpecFromRole(ColorRole::Surface));
      clearBorder();

      addChild(
          ui::label({
              .out = &m_title,
              .text = m_vpn.name,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = m_vpn.active ? FontWeight::Bold : FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .flexGrow = 1.0F,
          })
      );

      addChild(
          ui::button({
              .out = &m_checkButton,
              .glyph = "check",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .opacity = m_vpn.active ? 1.0F : 0.0F,
          })
      );

      addChild(
          ui::button({
              .out = &m_actionButton,
              .glyph = m_vpn.active ? "plug-off" : "plug",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = m_vpn.active ? ButtonVariant::Destructive : ButtonVariant::Default,
              .tooltip = i18n::tr(
                  m_vpn.active ? "control-center.network.disconnect" : "control-center.network.connect"
              ),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() { triggerAction(); },
          })
      );

      auto area = ui::inputArea({});
      area->setPropagateEvents(true);
      area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
      area->setOnLeave([this]() { applyState(); });
      area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
      area->setOnClick([this](const InputArea::PointerData& /*data*/) { triggerAction(); });
      m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

      applyState();
      m_paletteConn = paletteChanged().connect([this] { applyState(); });
    }

    void doLayout(Renderer& renderer) override {
      if (m_inputArea == nullptr) {
        return;
      }
      m_inputArea->setVisible(false);
      Flex::doLayout(renderer);
      m_inputArea->setVisible(true);
      m_inputArea->setPosition(0.0F, 0.0F);
      m_inputArea->setSize(width(), height());
      if (m_actionButton != nullptr) {
        const float areaWidth = std::max(0.0F, m_actionButton->x() - gap());
        m_inputArea->setSize(areaWidth, height());
      }
      applyState();
    }

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      return measureByLayout(renderer, constraints);
    }

    void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  private:
    void triggerAction() {
      if (m_vpn.active) {
        if (m_onDeactivate) {
          m_onDeactivate(m_vpn);
        }
      } else {
        if (m_onActivate) {
          m_onActivate(m_vpn);
        }
      }
    }

    void applyState() {
      const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
      const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
      if (pressed) {
        setFill(colorSpecFromRole(ColorRole::Primary));
        setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
        if (m_title != nullptr) {
          m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
        }
        return;
      }
      setFill(colorSpecFromRole(ColorRole::Surface));
      if (hov) {
        setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
      } else {
        clearBorder();
      }
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
    }

    VpnConnectionInfo m_vpn;
    std::function<void(const VpnConnectionInfo&)> m_onActivate;
    std::function<void(const VpnConnectionInfo&)> m_onDeactivate;
    Label* m_title = nullptr;
    Button* m_checkButton = nullptr;
    Button* m_actionButton = nullptr;
    InputArea* m_inputArea = nullptr;
    Signal<>::ScopedConnection m_paletteConn;
  };

} // namespace

// Informational modem row: signal glyph, operator/modem name, and live status
// detail. It has no actions of its own; the card header carries the enable toggle.
class CellularRow : public Flex {
public:
  CellularRow(float scale, CellularModemInfo modem) : m_modem(std::move(modem)) {
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setGap(Style::spaceSm * scale);
    setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    setMinHeight(kRowMinHeight * scale);
    setRadius(Style::scaledRadiusMd(scale));
    setFill(colorSpecFromRole(ColorRole::Surface));
    clearBorder();

    addChild(
        ui::glyph({
            .out = &m_signalGlyph,
            .glyph = cellularGlyphFor(m_modem),
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    addChild(
        ui::label({
            .out = &m_title,
            .text = cellularTitleFor(m_modem),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );

    addChild(
        ui::label({
            .out = &m_detail,
            .text = cellularDetailFor(m_modem),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  // Push live modem values into the row. Returns true when a value actually changed.
  bool syncLive(const CellularModemInfo& modem) {
    bool changed = false;
    if (m_signalGlyph != nullptr && m_signalGlyph->setGlyph(cellularGlyphFor(modem))) {
      changed = true;
    }
    if (m_title != nullptr && m_title->setText(cellularTitleFor(modem))) {
      changed = true;
    }
    if (m_detail != nullptr && m_detail->setText(cellularDetailFor(modem))) {
      changed = true;
    }
    m_modem = modem;
    return changed;
  }

private:
  CellularModemInfo m_modem;
  Glyph* m_signalGlyph = nullptr;
  Label* m_title = nullptr;
  Label* m_detail = nullptr;
};

NetworkTab::NetworkTab(const ControlCenterServices& services)
    : m_network(services.network), m_secrets(services.networkSecrets), m_externalIpService(services.externalIp),
      m_modem(services.modem), m_config(services.config), m_http(services.httpClient), m_sysmon(services.sysmon),
      m_clipboard(services.clipboard),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "network"})) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> source = m_config->stateString("network_tab", "catalog")) {
      if (*source == "vpn") {
        m_catalog = Catalog::Vpn;
      } else if (*source == "cellular") {
        m_catalog = Catalog::Cellular;
      }
    }
  }
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback([this](const NetworkSecretAgent::SecretRequest& request) {
      showPasswordPrompt(request);
      PanelManager::instance().refresh();
    });
  }
}

NetworkTab::~NetworkTab() {
  cancelSpeedTest();
  ++m_pingGeneration;
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback(nullptr);
    m_secrets->cancelSecret();
  }
}

std::unique_ptr<Flex> NetworkTab::create() {
  const float scale = contentScale();
  const float controlHeight = Style::controlHeight * scale;

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto currentCard = ui::column({
      .out = &m_currentCard,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applySectionCardStyle(card, scale, opacity); },
  });

  auto catalogHost = ui::column({
      .out = &m_catalogHost,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
  });
  catalogHost->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyCatalog(index); },
      })
  );
  currentCard->addChild(std::move(catalogHost));

  auto connRow = ui::row(
      {.out = &m_currentRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::label({
          .out = &m_currentTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      }),
      ui::button({
          .out = &m_disconnectButton,
          .glyph = "plug-off",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Destructive,
          .tooltip = i18n::tr("control-center.network.disconnect"),
          .minHeight = controlHeight,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() {
            if (m_network == nullptr || m_actionPending) {
              return;
            }
            const bool wasConnected = m_network->state().connected;
            if (wasConnected) {
              m_network->disconnect();
            } else if (!m_network->activateWiredConnection()) {
              return;
            }
            beginPendingAction(wasConnected);
            PanelManager::instance().refresh();
          },
      })
  );
  currentCard->addChild(std::move(connRow));
  currentCard->addChild(
      ui::label({
          .out = &m_currentDetail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  currentCard->addChild(
      ui::label({
          .out = &m_throughputLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  addWellDivider(*currentCard, scale);
  m_dnsProfilePicker = addSelectRow(
      *currentCard, scale, i18n::tr("control-center.network.dns-label"),
      {
          i18n::tr("control-center.network.dns-automatic"),
          i18n::tr("control-center.network.dns-google"),
          i18n::tr("control-center.network.dns-cloudflare"),
          i18n::tr("control-center.network.dns-nextdns"),
          i18n::tr("control-center.network.dns-custom"),
      },
      panelCardOpacity(),
      [this](std::size_t index) {
        if (m_dnsSyncing || index > static_cast<std::size_t>(DnsProfile::Custom)) {
          return;
        }
        m_dnsProfile = static_cast<DnsProfile>(index);
        syncDnsPicker();
        if (m_dnsProfile == DnsProfile::Custom) {
          if (m_dnsCustomInput != nullptr && m_dnsCustomInput->value().empty()) {
            if (m_config != nullptr && !m_config->config().controlCenter.networkTab.customDns.empty()) {
              m_dnsCustomInput->setValue(m_config->config().controlCenter.networkTab.customDns);
            }
          }
          PanelManager::instance().requestLayout();
          return;
        }
        applySelectedDnsProfile();
      }
  );
  currentCard->addChild(
      ui::label({
          .out = &m_dnsLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  currentCard->addChild(
      ui::row(
          {.out = &m_dnsNextDnsRow,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm * scale,
           .visible = false,
           .participatesInLayout = false},
          ui::input({
              .out = &m_dnsNextDnsInput,
              .placeholder = i18n::tr("control-center.network.dns-nextdns-placeholder"),
              .surfaceOpacity = panelCardOpacity(),
              .flexGrow = 1.0F,
              .onSubmit = [this](const std::string&) { applySelectedDnsProfile(); },
          })
      )
  );
  currentCard->addChild(
      ui::row(
          {.out = &m_dnsCustomRow,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm * scale,
           .visible = false,
           .participatesInLayout = false},
          ui::input({
              .out = &m_dnsCustomInput,
              .placeholder = i18n::tr("control-center.network.dns-custom-placeholder"),
              .surfaceOpacity = panelCardOpacity(),
              .flexGrow = 1.0F,
              .onSubmit = [this](const std::string&) { applySelectedDnsProfile(); },
          }),
          ui::button({
              .out = &m_dnsApplyButton,
              .text = i18n::tr("control-center.network.dns-apply"),
              .variant = ButtonVariant::Ghost,
              .minHeight = controlHeight,
              .onClick = [this]() { applySelectedDnsProfile(); },
          })
      )
  );
  addWellDivider(*currentCard, scale);
  currentCard->addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceXs * scale},
          ui::button({
              .out = &m_pingButton,
              .glyph = "activity-heartbeat",
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.network.ping"),
              .minHeight = controlHeight,
              .paddingH = Style::spaceMd * scale,
              .onClick = [this]() { startPing(); },
          }),
          ui::button({
              .out = &m_speedButton,
              .glyph = "arrow-down",
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.network.speed-test"),
              .minHeight = controlHeight,
              .paddingH = Style::spaceMd * scale,
              .onClick = [this]() { startSpeedTest(); },
          }),
          ui::button({
              .out = &m_copyIpButton,
              .glyph = "copy",
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.network.copy-ip"),
              .minHeight = controlHeight,
              .paddingH = Style::spaceMd * scale,
              .onClick = [this]() { copyIpv4(); },
          }),
          ui::button({
              .out = &m_shareButton,
              .glyph = "qrcode",
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.network.share-wifi"),
              .minHeight = controlHeight,
              .paddingH = Style::spaceMd * scale,
              .onClick = [this]() { shareWifi(); },
          })
      )
  );
  currentCard->addChild(
      ui::label({
          .out = &m_toolsStatus,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  auto qrBlock = ui::column({
      .out = &m_qrBlock,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
  });
  qrBlock->addChild(
      ui::row(
          {.align = FlexAlign::Start, .gap = Style::spaceSm * scale},
          ui::image({
              .out = &m_qrImage,
              .fit = ImageFit::Contain,
              .width = 180.0F * scale,
              .height = 180.0F * scale,
          }),
          ui::button({
              .out = &m_qrCloseButton,
              .glyph = "x",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.network.hide-wifi-qr"),
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .onClick = [this]() { hideWifiQr(); },
          })
      )
  );
  qrBlock->addChild(
      ui::label({
          .out = &m_qrHint,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  currentCard->addChild(std::move(qrBlock));

  m_rootLayout->addChild(std::move(currentCard));

  auto passwordCard = ui::column({
      .out = &m_passwordCard,
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        applySectionCardStyle(card, scale, opacity);
        card.setGap(Style::spaceMd * scale);
      },
  });

  passwordCard->addChild(
      ui::label({
          .out = &m_passwordTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  passwordCard->addChild(
      ui::label({
          .out = &m_credentialError,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .maxLines = 4,
          .visible = false,
      })
  );

  const auto submitFromForm = [this](const std::string& /*value*/) {
    submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{});
  };

  auto enterpriseFields = ui::column({
      .out = &m_enterpriseFields,
      // Flex defaults to Center on the cross axis, which collapses each field to
      // its natural width. The card itself stretches via applySectionCardStyle();
      // this nested column has to say so for itself.
      .align = FlexAlign::Stretch,
      .visible = false,
      .configure = [scale](Flex& column) { column.setGap(Style::spaceSm * scale); },
  });

  enterpriseFields->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::select({
              .out = &m_eapSelect,
              .options = optionLabels(kEapOptions),
              .selectedIndex = std::size_t{0},
              .placeholder = i18n::tr("control-center.network.eap-method"),
              .surfaceOpacity = panelCardOpacity(),
              .flexGrow = 1.0F,
              .onSelectionChanged =
                  [this](std::size_t index, std::string_view /*text*/) {
                    // PEAP tunnels MSCHAPv2 only. Pinning and locking the inner method
                    // beats letting the user assemble a pair that cannot authenticate
                    // and only fails later, at association time.
                    if (m_phase2Select == nullptr) {
                      return;
                    }
                    const bool pinsInnerMethod =
                        optionAt(kEapOptions, index).method == network_enterprise::EapMethod::Peap;
                    if (pinsInnerMethod) {
                      m_phase2Select->setSelectedIndexSilently(kPeapPhase2Index);
                    }
                    m_phase2Select->setEnabled(!pinsInnerMethod);
                  },
          }),
          ui::select({
              .out = &m_phase2Select,
              .options = optionLabels(kPhase2Options),
              .selectedIndex = kPeapPhase2Index,
              .placeholder = i18n::tr("control-center.network.phase2-auth"),
              .enabled = false,
              .surfaceOpacity = panelCardOpacity(),
              .flexGrow = 1.0F,
          })
      )
  );

  enterpriseFields->addChild(
      ui::input({
          .out = &m_identityInput,
          .placeholder = i18n::tr("control-center.network.identity"),
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = submitFromForm,
      })
  );

  enterpriseFields->addChild(
      ui::input({
          .out = &m_anonymousIdentityInput,
          .placeholder = i18n::tr("control-center.network.anonymous-identity"),
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = submitFromForm,
      })
  );

  enterpriseFields->addChild(
      ui::input({
          .out = &m_domainMatchInput,
          .placeholder = i18n::tr("control-center.network.domain-suffix-match"),
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = submitFromForm,
      })
  );

  enterpriseFields->addChild(
      ui::input({
          .out = &m_caCertInput,
          .placeholder = i18n::tr("control-center.network.ca-certificate"),
          .surfaceOpacity = panelCardOpacity(),
          .onSubmit = submitFromForm,
      })
  );

  passwordCard->addChild(std::move(enterpriseFields));

  auto inputRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::input({
          .out = &m_passwordInput,
          .placeholder = i18n::tr("control-center.network.password"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onSubmit = [this](const std::string& value) { submitPasswordPrompt(value); },
      }),
      ui::button({
          .out = &m_passwordRevealButton,
          .glyph = "eye",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.network.show-password"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this]() {
                if (m_passwordInput == nullptr) {
                  return;
                }
                m_passwordRevealed = !m_passwordRevealed;
                m_passwordInput->setPasswordMode(!m_passwordRevealed);
                if (m_passwordRevealButton != nullptr) {
                  m_passwordRevealButton->setGlyph(m_passwordRevealed ? "eye-off" : "eye");
                  m_passwordRevealButton->setTooltip(i18n::tr(
                      m_passwordRevealed ? "control-center.network.hide-password"
                                         : "control-center.network.show-password"
                  ));
                }
              },
      }),
      ui::button({
          .text = i18n::tr("control-center.network.connect"),
          .variant = ButtonVariant::Default,
          .onClick =
              [this]() { submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{}); },
      }),
      ui::button({
          .text = i18n::tr("common.actions.cancel"),
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { cancelPasswordPrompt(); },
      })
  );

  passwordCard->addChild(std::move(inputRow));
  m_rootLayout->addChild(std::move(passwordCard));

  auto listScroll = ui::scrollView({
      .out = &m_liveScroll,
      .state = &m_liveScrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_list = listScroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(Style::spaceMd * scale);
  m_rootLayout->addChild(std::move(listScroll));

  if (m_config != nullptr) {
    const auto& networkTab = m_config->config().controlCenter.networkTab;
    if (m_dnsNextDnsInput != nullptr && !networkTab.nextDnsId.empty()) {
      m_dnsNextDnsInput->setValue(networkTab.nextDnsId);
    }
    if (m_dnsCustomInput != nullptr && !networkTab.customDns.empty()) {
      m_dnsCustomInput->setValue(networkTab.customDns);
    }
  }
  syncDnsPicker();
  return m_settings->wrapLive(std::move(live), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> NetworkTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void NetworkTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool NetworkTab::configureMode() const { return m_settings->configureMode(); }

void NetworkTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

void NetworkTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_active && m_network != nullptr) {
    m_network->requestScan();
    refreshDnsOverride();
  } else {
    ++m_pingGeneration;
    m_pingRunning = false;
    ++m_shareGeneration;
    m_shareRunning = false;
    ++m_dnsGeneration;
    m_dnsRunning = false;
    cancelSpeedTest();
  }
}

void NetworkTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncPasswordCard();
  rebuildApList(renderer);
  syncApRows();
  syncCellularCard();
  syncCurrentCard();
  uploadQr(renderer);
  m_rootLayout->layout(renderer);
}

void NetworkTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  syncPasswordCard();
  const bool hasVpns = m_network != nullptr && !m_network->vpnConnections().empty();
  const bool hasCellular = m_modem != nullptr && !m_modem->modems().empty();
  syncCatalogBar(hasVpns, hasCellular);
  rebuildApList(renderer);
  bool listChanged = syncApRows();
  listChanged = syncCellularCard() || listChanged;
  if (listChanged && m_liveScroll != nullptr) {
    m_liveScroll->layout(renderer);
  }
  syncCurrentCard();
  uploadQr(renderer);
}

void NetworkTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  ++m_pingGeneration;
  m_pingRunning = false;
  ++m_shareGeneration;
  m_shareRunning = false;
  cancelSpeedTest();
  m_rootLayout = nullptr;
  m_currentCard = nullptr;
  m_catalogHost = nullptr;
  m_catalogSwitch = nullptr;
  m_currentTitle = nullptr;
  m_currentDetail = nullptr;
  m_throughputLabel = nullptr;
  m_dnsLabel = nullptr;
  m_dnsProfilePicker = nullptr;
  m_dnsNextDnsRow = nullptr;
  m_dnsCustomRow = nullptr;
  m_dnsNextDnsInput = nullptr;
  m_dnsCustomInput = nullptr;
  m_dnsApplyButton = nullptr;
  m_toolsStatus = nullptr;
  m_pingButton = nullptr;
  m_speedButton = nullptr;
  m_copyIpButton = nullptr;
  m_shareButton = nullptr;
  m_qrBlock = nullptr;
  m_qrImage = nullptr;
  m_qrCloseButton = nullptr;
  m_qrHint = nullptr;
  m_passwordCard = nullptr;
  m_passwordTitle = nullptr;
  m_passwordInput = nullptr;
  m_passwordRevealButton = nullptr;
  m_passwordRevealed = false;
  m_enterpriseFields = nullptr;
  m_eapSelect = nullptr;
  m_phase2Select = nullptr;
  m_identityInput = nullptr;
  m_anonymousIdentityInput = nullptr;
  m_caCertInput = nullptr;
  m_domainMatchInput = nullptr;
  m_credentialError = nullptr;
  m_liveScroll = nullptr;
  m_list = nullptr;
  m_rescanButton = nullptr;
  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_currentRow = nullptr;
  m_disconnectButton = nullptr;
  m_cellularToggle = nullptr;
  m_cellularRows.clear();
  m_cellularTogglePending = false;
  m_cellularTogglePendingTimer.stop();
  m_apRows.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0F;
  m_pendingAccessPoint.reset();
  m_active = false;
  m_actionPending = false;
  m_actionPendingTimer.stop();
  m_wifiTogglePending = false;
  m_wifiToggleWriteComplete = false;
  m_wifiToggleTargetObserved = false;
  ++m_wifiToggleRequestGeneration;
  m_qrVisible = false;
  m_qrDirty = false;
  ++m_dnsGeneration;
  m_dnsRunning = false;
  m_dnsConnectionKey.clear();
}

void NetworkTab::syncPasswordCard() {
  if (m_passwordCard == nullptr) {
    return;
  }
  const bool layoutDirty = setShown(m_passwordCard, m_hasPendingSecret) ||
                            setShown(m_enterpriseFields, m_hasPendingSecret && m_pendingEnterprise);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
  if (m_hasPendingSecret && m_passwordTitle != nullptr) {
    if (m_pendingSsid.empty()) {
      m_passwordTitle->setText(i18n::tr("control-center.network.password-prompt"));
    } else if (m_pendingEnterprise) {
      m_passwordTitle->setText(i18n::tr("control-center.network.enterprise-prompt-for", "ssid", m_pendingSsid));
    } else {
      m_passwordTitle->setText(i18n::tr("control-center.network.password-prompt-for", "ssid", m_pendingSsid));
    }
  }
}

void NetworkTab::setCredentialError(const std::string& message) {
  if (m_credentialError == nullptr) {
    return;
  }
  m_credentialError->setText(message);
  if (setShown(m_credentialError, !message.empty())) {
    PanelManager::instance().requestLayout();
  }
}

network_enterprise::EnterpriseCredentials NetworkTab::collectEnterpriseCredentials(const std::string& password) const {
  const auto selectedIndexOf = [](const Select* select) -> std::size_t {
    return select != nullptr ? select->selectedIndex() : kDefaultOptionIndex;
  };
  const auto valueOf = [](const Input* input) -> std::string {
    return input != nullptr ? input->value() : std::string{};
  };

  network_enterprise::EnterpriseCredentials credentials;
  credentials.eap = optionAt(kEapOptions, selectedIndexOf(m_eapSelect)).method;
  credentials.phase2 = optionAt(kPhase2Options, selectedIndexOf(m_phase2Select)).auth;
  credentials.identity = valueOf(m_identityInput);
  credentials.anonymousIdentity = valueOf(m_anonymousIdentityInput);
  credentials.password = password;
  credentials.caCertPath = valueOf(m_caCertInput);
  credentials.domainSuffixMatch = valueOf(m_domainMatchInput);
  return credentials;
}

std::string NetworkTab::enterpriseBlockReason(const AccessPointInfo& ap) const {
  if (m_network == nullptr || !m_network->supportsEnterprise()) {
    return i18n::tr("control-center.network.error-enterprise-unsupported");
  }
  if (!network_enterprise::passwordAuthUsable(ap.keyManagement)) {
    return i18n::tr("control-center.network.error-certificate-required");
  }
  return {};
}

void NetworkTab::showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request) {
  // A form the user is filling outranks NM's own secret request: letting it
  // through would replace the card mid-typing and discard what has been entered.
  // Decline instead, so NM stops waiting rather than blocking on a prompt that
  // will never be answered.
  if (m_hasPendingSecret && m_pendingAccessPoint.has_value()) {
    if (m_secrets != nullptr) {
      m_secrets->cancelSecret();
    }
    return;
  }

  clearPasswordPrompt();
  m_hasPendingSecret = true;
  m_pendingSsid = request.ssid;
  // NM is asking for one secret against a profile it already holds, so only the
  // password is missing; the rest of the 802.1X form would have nothing to fill.
  m_pendingEnterprise = false;
  PanelManager::instance().requestLayout();
}

void NetworkTab::showPasswordPrompt(const AccessPointInfo& ap) {
  clearPasswordPrompt();
  m_hasPendingSecret = true;
  m_pendingSsid = ap.ssid;
  m_pendingAccessPoint = ap;
  m_pendingEnterprise = ap.isEnterprise();
  if (m_pendingEnterprise) {
    // Say up front when this network cannot be joined with a password, rather
    // than after the user has filled in the whole form.
    setCredentialError(enterpriseBlockReason(ap));
  }
  PanelManager::instance().requestLayout();
}

void NetworkTab::submitPasswordPrompt(const std::string& value) {
  if (m_pendingAccessPoint.has_value() && m_pendingEnterprise) {
    if (m_network == nullptr) {
      return;
    }
    // Every rejection below leaves the card open with a reason showing, so the
    // user can fix the field rather than watch the prompt close and nothing happen.
    if (const auto blocked = enterpriseBlockReason(*m_pendingAccessPoint); !blocked.empty()) {
      setCredentialError(blocked);
      PanelManager::instance().refresh();
      return;
    }
    const auto credentials = collectEnterpriseCredentials(value);
    const auto problem = network_enterprise::validate(credentials);
    if (problem != network_enterprise::Validation::Ok) {
      setCredentialError(validationMessage(problem));
      PanelManager::instance().refresh();
      return;
    }
    m_network->activateEnterpriseAccessPoint(*m_pendingAccessPoint, credentials);
  } else if (m_pendingAccessPoint.has_value()) {
    if (value.empty()) {
      return;
    }
    if (m_network != nullptr) {
      m_network->activateAccessPoint(*m_pendingAccessPoint, value);
    }
  } else if (m_secrets != nullptr) {
    m_secrets->submitSecret(value);
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::cancelPasswordPrompt() {
  if (!m_pendingAccessPoint.has_value() && m_secrets != nullptr) {
    m_secrets->cancelSecret();
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::clearPasswordPrompt() {
  m_hasPendingSecret = false;
  m_pendingEnterprise = false;
  m_pendingSsid.clear();
  m_pendingAccessPoint.reset();
  m_passwordRevealed = false;
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordMode(true);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
  for (Input* input : {m_identityInput, m_anonymousIdentityInput, m_caCertInput, m_domainMatchInput}) {
    if (input != nullptr) {
      input->setValue("");
    }
  }
  if (m_eapSelect != nullptr) {
    m_eapSelect->setSelectedIndexSilently(kDefaultOptionIndex);
  }
  if (m_phase2Select != nullptr) {
    m_phase2Select->setSelectedIndexSilently(kPeapPhase2Index);
    m_phase2Select->setEnabled(false); // PEAP is the default, and it pins MSCHAPv2.
  }
  setCredentialError({});
}

void NetworkTab::syncCurrentCard() {
  if (m_currentTitle == nullptr || m_currentDetail == nullptr) {
    return;
  }
  if (m_network == nullptr) {
    m_currentTitle->setText(i18n::tr("control-center.network.unavailable-title"));
    m_currentDetail->setText(i18n::tr("control-center.network.unavailable-detail"));
    if (m_currentRow != nullptr) {
    (void)setShown(m_currentRow, false);
    }
    return;
  }
  const NetworkState& s = m_network->state();
  if (m_actionPending) {
    const bool flipped = s.connected != m_actionPendingConnected;
    const bool timedOut = std::chrono::steady_clock::now() - m_actionPendingSince > kActionPendingTimeout;
    if (flipped || timedOut) {
      m_actionPending = false;
      m_actionPendingTimer.stop();
    }
  }
  if (m_wifiTogglePending) {
    if (s.wirelessEnabled == m_wifiToggleTarget) {
      m_wifiToggleTargetObserved = true;
    }
    if (m_wifiToggleWriteComplete && m_wifiToggleTargetObserved) {
      m_wifiTogglePending = false;
      m_wifiToggleWriteComplete = false;
      m_wifiToggleTargetObserved = false;
    }
  }
  static const std::string kNoExternalIp;
  const std::string& externalIp = m_externalIpService != nullptr ? m_externalIpService->externalIp() : kNoExternalIp;
  const CellularModemInfo* modem = m_modem != nullptr ? m_modem->primaryModem() : nullptr;
  m_currentTitle->setText(currentTitle(s, modem));
  m_currentDetail->setText(currentDetail(s, externalIp, modem));
  if (m_disconnectButton != nullptr) {
    const bool canReconnectWired = !s.connected && m_network->canActivateWiredConnection();
    (void)setShown(m_disconnectButton, s.connected || canReconnectWired || m_actionPending);
    m_disconnectButton->setGlyph(s.connected ? "plug-off" : "plug");
    m_disconnectButton->setVariant(s.connected ? ButtonVariant::Destructive : ButtonVariant::Default);
    m_disconnectButton->setTooltip(
        i18n::tr(s.connected ? "control-center.network.disconnect" : "control-center.network.connect")
    );
    m_disconnectButton->setEnabled(!m_actionPending);
  }
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setChecked(m_wifiTogglePending ? m_wifiToggleTarget : s.wirelessEnabled);
    m_wifiToggle->setEnabled(!m_wifiTogglePending);
  }
  if (m_scanSpinner != nullptr) {
    m_scanSpinner->setVisible(s.scanning);
    m_scanSpinner->setParticipatesInLayout(s.scanning);
    if (s.scanning && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!s.scanning && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
  if (m_throughputLabel != nullptr) {
    if (m_sysmon != nullptr && m_sysmon->isRunning() && s.connected) {
      const double rx = m_sysmon->netRxBytesPerSec(s.interfaceName);
      const double tx = m_sysmon->netTxBytesPerSec(s.interfaceName);
      (void)setShown(m_throughputLabel, true);
      m_throughputLabel->setText(i18n::tr(
          "control-center.network.throughput", "down", FormatUnits::formatDecimalBytesPerSecond(rx), "up",
          FormatUnits::formatDecimalBytesPerSecond(tx)
      ));
    } else {
      (void)setShown(m_throughputLabel, false);
    }
  }
  if (m_dnsLabel != nullptr) {
    const std::string dns = currentDnsText();
    (void)setShown(m_dnsLabel, !dns.empty());
    if (!dns.empty()) {
      m_dnsLabel->setText(dns);
    }
  }
  std::string connectionKey;
  connectionKey += s.interfaceName;
  connectionKey += '|';
  connectionKey += s.ssid;
  connectionKey += '|';
  connectionKey += s.connected ? '1' : '0';
  if (connectionKey != m_dnsConnectionKey) {
    m_dnsConnectionKey = std::move(connectionKey);
    refreshDnsOverride();
  }
  syncDnsPicker();
  const bool wifiConnected = s.connected && s.kind == NetworkConnectivity::Wireless && !s.ssid.empty();
  if (m_copyIpButton != nullptr) {
    m_copyIpButton->setEnabled(s.connected && !s.ipv4.empty());
  }
  if (m_shareButton != nullptr) {
    m_shareButton->setEnabled(wifiConnected && !m_shareRunning);
  }
  if (m_speedButton != nullptr) {
    m_speedButton->setEnabled(s.connected && !m_speedRunning);
  }
  if (m_pingButton != nullptr) {
    m_pingButton->setEnabled(s.connected && !m_pingRunning);
  }
  if (!wifiConnected) {
    hideWifiQr();
  }
}

void NetworkTab::beginPendingAction(bool wasConnected) {
  m_actionPending = true;
  m_actionPendingConnected = wasConnected;
  m_actionPendingSince = std::chrono::steady_clock::now();
  m_actionPendingTimer.start(kActionPendingTimeout + std::chrono::milliseconds(50), []() {
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
  if (m_disconnectButton != nullptr) {
    m_disconnectButton->setEnabled(false);
  }
}

void NetworkTab::requestWirelessEnabled(bool enabled) {
  m_wifiTogglePending = true;
  m_wifiToggleTarget = enabled;
  m_wifiToggleWriteComplete = false;
  m_wifiToggleTargetObserved = false;
  const std::uint64_t generation = ++m_wifiToggleRequestGeneration;
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setEnabled(false);
  }
  if (m_network == nullptr) {
    handleWirelessEnabledCompletion(generation, false);
    return;
  }
  m_network->setWirelessEnabled(enabled, [this, generation](bool success) {
    handleWirelessEnabledCompletion(generation, success);
  });
}

void NetworkTab::handleWirelessEnabledCompletion(std::uint64_t generation, bool success) {
  if (!m_wifiTogglePending || generation != m_wifiToggleRequestGeneration) {
    return;
  }
  m_wifiToggleWriteComplete = success;
  if (!success) {
    m_wifiTogglePending = false;
    m_wifiToggleTargetObserved = false;
  }
  PanelManager::instance().requestUpdateOnly();
  PanelManager::instance().requestRedraw();
}

// Identity of the built list: which rows exist, in which order, which controls
// each carries, and how each activates. The signal strength is absent by design —
// it refreshes in place through syncApRows(), so a scan update no longer tears the
// list down. Access points arrive sorted, so a change in row order changes the key.
std::string
NetworkTab::structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const {
  std::string key;
  for (const auto& ap : aps) {
    key += ap.ssid;
    key.push_back(':');
    key += ap.secured ? '1' : '0';
    key.push_back(':');
    key += static_cast<char>('0' + static_cast<int>(ap.keyManagement));
    key.push_back(':');
    key += ap.active ? '1' : '0';
    key.push_back(':');
    key += (m_network != nullptr && m_network->hasSavedConnection(ap.ssid)) ? '1' : '0';
    key.push_back('\n');
  }
  key += "---\n";
  for (const auto& vpn : vpns) {
    key += vpn.path;
    key.push_back(':');
    key += vpn.name;
    key.push_back(':');
    key += vpn.active ? '1' : '0';
    key.push_back('\n');
  }
  const bool wirelessEnabled = m_network != nullptr && m_network->state().wirelessEnabled;
  const bool scanning = m_network != nullptr && m_network->state().scanning;
  key += "vis:";
  key += m_vpnVisible ? '1' : '0';
  key += "\nwifi:";
  key += wirelessEnabled ? '1' : '0';
  key += "\nscan:";
  key += scanning ? '1' : '0';
  key += "\ncatalog:";
  key += std::to_string(static_cast<int>(m_catalog));
  if (m_config != nullptr) {
    const auto& networkTab = m_config->config().controlCenter.networkTab;
    key += "\nband:";
    key += std::to_string(static_cast<int>(networkTab.wifiBand));
    key += "\nhidden:\n";
    for (const auto& ssid : networkTab.hiddenSsids) {
      key += ssid;
      key.push_back('\n');
    }
  }
  // Cellular rows live-sync their state/signal, so only the structural
  // identity (which modems exist) belongs to the key.
  key += "\ncellular:\n";
  if (m_modem != nullptr) {
    for (const auto& modem : m_modem->modems()) {
      key += modem.path;
      key.push_back(':');
      key += modem.name;
      key.push_back('\n');
    }
  }
  return key;
}

void NetworkTab::rebuildApList(Renderer& renderer) {
  uiAssertNotRendering("NetworkTab::rebuildApList");
  if (m_list == nullptr || m_liveScroll == nullptr) {
    return;
  }
  const float listWidth = m_liveScroll->contentViewportWidth();
  if (listWidth <= 0.0F) {
    return;
  }

  std::vector<AccessPointInfo> aps;
  if (m_network != nullptr) {
    aps = sortedAccessPoints(m_network->accessPoints());
    applyAccessPointFilters(aps);
  }
  const auto& vpns = m_network != nullptr ? m_network->vpnConnections() : std::vector<VpnConnectionInfo>{};
  const std::string nextStructure = structureKey(aps, vpns);
  if (listWidth == m_lastListWidth && nextStructure == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextStructure;
  const float scale = contentScale();

  auto buildApRows = [&]() {
    auto container = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
    });
    if (aps.empty()) {
      container->addChild(
          ui::label({
              .text = i18n::tr("control-center.network.no-networks"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    } else {
      std::vector<AccessPointInfo> activeAps;
      std::vector<AccessPointInfo> savedAps;
      std::vector<AccessPointInfo> availableAps;
      activeAps.reserve(aps.size());
      savedAps.reserve(aps.size());
      availableAps.reserve(aps.size());

      for (const auto& ap : aps) {
        const bool saved = m_network != nullptr && m_network->hasSavedConnection(ap.ssid);
        if (ap.active) {
          activeAps.push_back(ap);
        } else if (saved) {
          savedAps.push_back(ap);
        } else {
          availableAps.push_back(ap);
        }
      }

      auto addRows = [&](Flex& parent, const std::vector<AccessPointInfo>& bucket) {
        if (bucket.empty()) {
          return;
        }
        for (const auto& ap : bucket) {
          const bool saved = m_network != nullptr && m_network->hasSavedConnection(ap.ssid);
          auto row = std::make_unique<AccessPointRow>(
              scale, ap, saved,
              [this](const AccessPointInfo& clicked) {
                if (clicked.active || m_network == nullptr) {
                  return;
                }
                if (clicked.secured && !m_network->hasSavedConnection(clicked.ssid)) {
                  showPasswordPrompt(clicked);
                  PanelManager::instance().refresh();
                  return;
                }
                m_network->activateAccessPoint(clicked);
              },
              [this](const AccessPointInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->forgetSsid(clicked.ssid);
                }
                PanelManager::instance().refresh();
              }
          );
          auto* rowPtr = row.get();
          parent.addChild(std::move(row));
          m_apRows.emplace(rowPtr->ssid(), rowPtr);
        }
      };

      addRows(*container, activeAps);

      auto addBucket = [&](const std::string& title, const std::vector<AccessPointInfo>& bucket) {
        if (bucket.empty()) {
          return;
        }
        auto group = ui::column({
            .align = FlexAlign::Stretch,
            .gap = Style::spaceXs * scale,
        });
        group->addChild(makeWifiBucketHeaderRow(title, scale));
        addRows(*group, bucket);
        container->addChild(std::move(group));
      };

      addBucket(i18n::tr("control-center.network.saved"), savedAps);
      addBucket(i18n::tr("control-center.network.available"), availableAps);
    }
    return container;
  };

  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_rescanButton = nullptr;
  m_cellularToggle = nullptr;
  m_cellularRows.clear();
  m_apRows.clear();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  if (m_network == nullptr) {
    syncCatalogBar(false, false);
    auto empty = ui::column({
        .configure =
            [scale, opacity = panelCardOpacity()](Flex& card) { applySectionCardStyle(card, scale, opacity); },
    });
    empty->addChild(
        ui::label({
            .text = i18n::tr("control-center.network.unavailable-title"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    m_list->addChild(std::move(empty));
  } else {
    const bool hasVpns = !vpns.empty();
    const bool hasCellular = m_modem != nullptr && !m_modem->modems().empty();
    syncCatalogBar(hasVpns, hasCellular);
    const float opacity = panelCardOpacity();
    Flex* well = nullptr;
    auto wellCard = ui::column({
        .out = &well,
        .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
    });

    if (m_catalog == Catalog::Vpn && hasVpns) {
      auto vpnHeader = makeCardHeaderRow(i18n::tr("control-center.network.vpns"), scale);
      vpnHeader->addChild(
          ui::toggle({
              .checkedImmediate = m_vpnVisible,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this, vpns](bool checked) {
                if (!checked && m_network != nullptr) {
                  for (const auto& vpn : vpns) {
                    if (vpn.active) {
                      m_network->deactivateVpnConnection(vpn);
                    }
                  }
                }
                m_vpnVisible = checked;
                PanelManager::instance().refresh();
              },
          })
      );
      well->addChild(std::move(vpnHeader));

      if (m_vpnVisible) {
        for (const auto& vpn : vpns) {
          auto row = std::make_unique<VpnConnectionRow>(
              scale, vpn,
              [this](const VpnConnectionInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->activateVpnConnection(clicked);
                }
                PanelManager::instance().refresh();
              },
              [this](const VpnConnectionInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->deactivateVpnConnection(clicked);
                }
                PanelManager::instance().refresh();
              }
          );
          well->addChild(std::move(row));
        }
      }
    } else if (m_catalog == Catalog::Cellular && hasCellular) {
      auto cellularHeader = makeCardHeaderRow(i18n::tr("control-center.network.cellular"), scale);
      cellularHeader->addChild(
          ui::toggle({
              .out = &m_cellularToggle,
              .checkedImmediate = cellularToggleDisplayChecked(),
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) { requestCellularEnabled(checked); },
          })
      );
      well->addChild(std::move(cellularHeader));

      for (const auto& modem : m_modem->modems()) {
        auto row = std::make_unique<CellularRow>(scale, modem);
        m_cellularRows.push_back(row.get());
        well->addChild(std::move(row));
      }
    } else {
      auto wifiHeader = makeCardHeaderRow(i18n::tr("control-center.network.wifi"), scale);
      wifiHeader->addChild(
          ui::spinner({
              .out = &m_scanSpinner,
              .color = colorSpecFromRole(ColorRole::Primary),
              .spinnerSize = Style::baseGlyphSize * scale,
              .visible = false,
          })
      );

      wifiHeader->addChild(
          ui::button({
              .out = &m_rescanButton,
              .glyph = "refresh",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.network.rescan"),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() {
                if (m_network != nullptr) {
                  m_network->requestScan();
                }
              },
          })
      );

      wifiHeader->addChild(
          ui::toggle({
              .out = &m_wifiToggle,
              .checkedImmediate = m_network->state().wirelessEnabled,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) { requestWirelessEnabled(checked); },
          })
      );
      well->addChild(std::move(wifiHeader));
      well->addChild(buildApRows());
    }

    m_list->addChild(std::move(wellCard));

    // Live state (spinner visibility/animation, toggle checked) is owned by
    // syncCurrentCard(), which runs every frame after the card is attached.
    // rebuildApList() builds structure only and must not drive animations.
  }
  m_liveScroll->layout(renderer);
}

bool NetworkTab::syncApRows() {
  if (m_network == nullptr || m_apRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& ap : m_network->accessPoints()) {
    const auto it = m_apRows.find(ap.ssid);
    if (it != m_apRows.end() && it->second->syncLiveMetrics(ap)) {
      changed = true;
    }
  }
  return changed;
}

bool NetworkTab::syncCellularCard() {
  if (m_modem == nullptr) {
    return false;
  }
  if (m_cellularTogglePending) {
    // Bringing mobile data up walks Enabling -> Searching -> Registered ->
    // Connecting, so the requested position is held instead of snapping back
    // while the modem works. Give up once it lands or the request times out.
    const bool reached = cellularToggleChecked() == m_cellularToggleTarget;
    const bool timedOut = std::chrono::steady_clock::now() - m_cellularTogglePendingSince > kCellularPendingTimeout;
    if (reached || timedOut) {
      m_cellularTogglePending = false;
      m_cellularTogglePendingTimer.stop();
    }
  }
  const auto& modems = m_modem->modems();
  bool changed = false;
  const std::size_t count = std::min(m_cellularRows.size(), modems.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (m_cellularRows[i] != nullptr && m_cellularRows[i]->syncLive(modems[i])) {
      changed = true;
    }
  }
  if (m_cellularToggle != nullptr) {
    m_cellularToggle->setChecked(cellularToggleDisplayChecked());
  }
  return changed;
}

bool NetworkTab::cellularToggleChecked() const {
  if (m_network != nullptr && m_network->canActivateCellularConnection()) {
    return m_network->state().cellularActive;
  }
  if (m_modem != nullptr) {
    return std::ranges::any_of(m_modem->modems(), [](const CellularModemInfo& modem) { return modem.enabled(); });
  }
  return false;
}

// The requested position while a toggle request is in flight, the observed one
// otherwise.
bool NetworkTab::cellularToggleDisplayChecked() const {
  return m_cellularTogglePending ? m_cellularToggleTarget : cellularToggleChecked();
}

void NetworkTab::requestCellularEnabled(bool enabled) {
  const bool nmCellular = m_network != nullptr && m_network->canActivateCellularConnection();
  if (enabled) {
    // GNOME parity: powering the modem is a prerequisite; the gsm connection is
    // what actually brings up mobile data.
    if (m_modem != nullptr) {
      m_modem->setAllModemsEnabled(true);
    }
    if (nmCellular) {
      m_network->activateCellularConnection();
    }
  } else {
    // Data off while the modem stays registered, like GNOME's mobile-data
    // switch. Without an NM gsm profile there is only modem power to cut.
    if (nmCellular) {
      m_network->deactivateCellularConnection();
    } else if (m_modem != nullptr) {
      m_modem->setAllModemsEnabled(false);
    }
  }
  m_cellularTogglePending = true;
  m_cellularToggleTarget = enabled;
  m_cellularTogglePendingSince = std::chrono::steady_clock::now();
  // Nothing else wakes the panel if the request never changes any state.
  m_cellularTogglePendingTimer.start(kCellularPendingTimeout + std::chrono::milliseconds(50), []() {
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

void NetworkTab::onPanelCardOpacityChanged(float opacity) {
  if (m_passwordInput != nullptr) {
    m_passwordInput->setSurfaceOpacity(opacity);
  }
  if (m_dnsCustomInput != nullptr) {
    m_dnsCustomInput->setSurfaceOpacity(opacity);
  }
  if (m_dnsNextDnsInput != nullptr) {
    m_dnsNextDnsInput->setSurfaceOpacity(opacity);
  }
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

void NetworkTab::applyAccessPointFilters(std::vector<AccessPointInfo>& aps) const {
  if (m_config == nullptr) {
    return;
  }
  const auto& networkTab = m_config->config().controlCenter.networkTab;
  std::erase_if(aps, [&](const AccessPointInfo& ap) {
    if (ssidIsHidden(ap.ssid, networkTab.hiddenSsids)) {
      return true;
    }
    return !matchesWifiBand(ap.frequencyMhz, networkTab.wifiBand);
  });
}

void NetworkTab::startPing() {
  if (m_http == nullptr || m_network == nullptr || !m_network->state().connected) {
    if (m_toolsStatus != nullptr) {
      m_toolsStatus->setText({});
    }
    m_pingRunning = false;
    return;
  }
  const std::uint64_t generation = ++m_pingGeneration;
  m_pingRunning = true;
  if (m_pingButton != nullptr) {
    m_pingButton->setEnabled(false);
  }
  if (m_toolsStatus != nullptr) {
    m_toolsStatus->setText(i18n::tr("control-center.network.ping-running"));
  }
  PanelManager::instance().requestRedraw();
  const auto started = std::chrono::steady_clock::now();
  HttpRequest req;
  req.url = "https://1.1.1.1/cdn-cgi/trace";
  req.freshConnection = true;
  m_http->request(std::move(req), [this, generation, started](HttpResponse response) {
    if (generation != m_pingGeneration) {
      return;
    }
    m_pingRunning = false;
    if (m_pingButton != nullptr) {
      m_pingButton->setEnabled(m_network != nullptr && m_network->state().connected);
    }
    if (m_toolsStatus == nullptr) {
      return;
    }
    if (!response.transportOk || response.status < 200 || response.status >= 300) {
      m_toolsStatus->setText(i18n::tr("control-center.network.ping-failed"));
      PanelManager::instance().requestRedraw();
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    m_toolsStatus->setText(i18n::tr("control-center.network.ping-result", "ms", std::to_string(ms)));
    PanelManager::instance().requestRedraw();
  });
}

void NetworkTab::cancelSpeedTest() {
  ++m_speedGeneration;
  if (m_http != nullptr && m_speedStream != 0) {
    m_http->cancelStream(m_speedStream);
    m_speedStream = 0;
  }
  m_speedRunning = false;
  m_speedBytes = 0;
  if (m_speedButton != nullptr) {
    m_speedButton->setEnabled(m_network != nullptr && m_network->state().connected);
  }
}

void NetworkTab::startSpeedTest() {
  if (m_http == nullptr || m_speedRunning || m_network == nullptr || !m_network->state().connected) {
    return;
  }
  cancelSpeedTest();
  const std::uint64_t generation = ++m_speedGeneration;
  m_speedRunning = true;
  m_speedBytes = 0;
  m_speedStarted = std::chrono::steady_clock::now();
  if (m_speedButton != nullptr) {
    m_speedButton->setEnabled(false);
  }
  if (m_toolsStatus != nullptr) {
    m_toolsStatus->setText(i18n::tr("control-center.network.speed-test-running"));
  }
  HttpRequest req;
  req.url = "https://speed.cloudflare.com/__down?bytes=2500000";
  req.freshConnection = true;
  m_speedStream = m_http->startStream(
      std::move(req),
      [this, generation](std::string_view chunk) {
        if (generation != m_speedGeneration) {
          return;
        }
        m_speedBytes += chunk.size();
      },
      [this, generation](HttpStreamResult result) {
        if (generation != m_speedGeneration) {
          return;
        }
        m_speedStream = 0;
        m_speedRunning = false;
        if (m_speedButton != nullptr) {
          m_speedButton->setEnabled(m_network != nullptr && m_network->state().connected);
        }
        if (m_toolsStatus == nullptr) {
          return;
        }
        const auto elapsed = std::chrono::steady_clock::now() - m_speedStarted;
        const double seconds = std::chrono::duration<double>(elapsed).count();
        if (!result.transportOk || result.status < 200 || result.status >= 300 || m_speedBytes == 0 || seconds <= 0.0) {
          m_toolsStatus->setText(i18n::tr("control-center.network.speed-test-failed"));
        } else {
          const double mbps = (static_cast<double>(m_speedBytes) * 8.0 / 1'000'000.0) / seconds;
          char buffer[32];
          std::snprintf(buffer, sizeof(buffer), "%.1f", mbps);
          m_toolsStatus->setText(i18n::tr("control-center.network.speed-test-result", "mbps", buffer));
        }
        PanelManager::instance().requestRedraw();
      }
  );
}

void NetworkTab::copyIpv4() {
  if (m_network == nullptr || m_clipboard == nullptr) {
    return;
  }
  const std::string& ipv4 = m_network->state().ipv4;
  if (ipv4.empty()) {
    return;
  }
  (void)m_clipboard->copyText(ipv4);
  if (m_toolsStatus != nullptr) {
    m_toolsStatus->setText(i18n::tr("control-center.network.ip-copied"));
  }
  PanelManager::instance().requestRedraw();
}

void NetworkTab::shareWifi() {
  if (m_qrVisible) {
    hideWifiQr();
    return;
  }
  if (m_network == nullptr || m_shareRunning) {
    return;
  }
  const NetworkState state = m_network->state();
  if (!state.connected || state.kind != NetworkConnectivity::Wireless || state.ssid.empty()) {
    return;
  }
  const WifiShareKind kind = wifiShareKind(state, m_network->accessPoints());
  if (kind.enterprise) {
    applyWifiShare(
        wifiJoinPayload(state, true, std::nullopt), i18n::tr("control-center.network.share-wifi-enterprise")
    );
    return;
  }
  if (!kind.secured) {
    applyWifiShare(wifiJoinPayload(state, false, std::nullopt), i18n::tr("control-center.network.share-wifi-copied"));
    return;
  }

  const std::uint64_t generation = ++m_shareGeneration;
  m_shareRunning = true;
  if (m_shareButton != nullptr) {
    m_shareButton->setEnabled(false);
  }
  m_network->requestActiveWifiPsk([this, generation, state](std::optional<std::string> psk) {
    if (generation != m_shareGeneration) {
      return;
    }
    m_shareRunning = false;
    if (m_shareButton != nullptr) {
      m_shareButton->setEnabled(
          m_network != nullptr && m_network->state().connected && m_network->state().kind == NetworkConnectivity::Wireless
          && !m_network->state().ssid.empty()
      );
    }
    const char* hintKey = psk.has_value() ? "control-center.network.share-wifi-copied"
                                        : "control-center.network.share-wifi-password-missing";
    applyWifiShare(wifiJoinPayload(state, true, psk), i18n::tr(hintKey));
  });
}

void NetworkTab::hideWifiQr() {
  if (!m_qrVisible) {
    return;
  }
  m_qrVisible = false;
  if (m_qrBlock != nullptr) {
    (void)setShown(m_qrBlock, false);
  }
  if (m_shareButton != nullptr) {
    m_shareButton->setSelected(false);
    m_shareButton->setText(i18n::tr("control-center.network.share-wifi"));
  }
  PanelManager::instance().requestLayout();
}

void NetworkTab::applyWifiShare(const std::string& payload, const std::string& hint) {
  if (m_clipboard != nullptr) {
    (void)m_clipboard->copyText(payload);
  }
  m_qrVisible = true;
  if (m_qrBlock != nullptr) {
    (void)setShown(m_qrBlock, true);
  }
  if (auto bitmap = qr_code::encodeRgba(payload, 6)) {
    m_qrBitmap = std::move(*bitmap);
    m_qrDirty = true;
  }
  if (m_qrHint != nullptr) {
    m_qrHint->setText(hint);
  }
  if (m_shareButton != nullptr) {
    m_shareButton->setSelected(true);
    m_shareButton->setText(i18n::tr("control-center.network.hide-wifi-qr"));
  }
  if (m_toolsStatus != nullptr) {
    m_toolsStatus->setText(hint);
  }
  PanelManager::instance().requestLayout();
}

void NetworkTab::uploadQr(Renderer& renderer) {
  if (!m_qrDirty || m_qrImage == nullptr || m_qrBitmap.rgba.empty() || m_qrBitmap.pixels <= 0) {
    return;
  }
  const int pixels = m_qrBitmap.pixels;
  const std::size_t size = m_qrBitmap.rgba.size();
  (void)m_qrImage->setSourceRaw(
      renderer, m_qrBitmap.rgba.data(), size, pixels, pixels, pixels * 4, PixmapFormat::RGBA, false
  );
  m_qrDirty = false;
}

void NetworkTab::syncDnsPicker() {
  const bool supported = m_network != nullptr && m_network->supportsDnsOverride();
  const bool connected = m_network != nullptr && m_network->state().connected;
  const bool enabled = supported && connected && !m_dnsRunning;
  if (m_dnsProfilePicker != nullptr) {
    m_dnsProfilePicker->setEnabled(enabled);
    m_dnsSyncing = true;
    m_dnsProfilePicker->setSelectedIndexSilently(static_cast<std::size_t>(m_dnsProfile));
    m_dnsSyncing = false;
  }
  const bool showNextDns = m_dnsProfile == DnsProfile::NextDns;
  const bool showCustom = m_dnsProfile == DnsProfile::Custom;
  bool layoutDirty = false;
  if (m_dnsNextDnsRow != nullptr) {
    layoutDirty = setShown(m_dnsNextDnsRow, showNextDns) || layoutDirty;
  }
  if (m_dnsCustomRow != nullptr) {
    layoutDirty = setShown(m_dnsCustomRow, showCustom) || layoutDirty;
  }
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
  if (m_dnsNextDnsInput != nullptr) {
    m_dnsNextDnsInput->setEnabled(enabled);
  }
  if (m_dnsCustomInput != nullptr) {
    m_dnsCustomInput->setEnabled(enabled);
  }
  if (m_dnsApplyButton != nullptr) {
    m_dnsApplyButton->setEnabled(enabled);
  }
}

void NetworkTab::refreshDnsOverride() {
  if (m_network == nullptr || !m_network->supportsDnsOverride()) {
    m_dnsProfile = DnsProfile::Automatic;
    syncDnsPicker();
    return;
  }
  const std::uint64_t generation = ++m_dnsGeneration;
  m_network->requestActiveDnsOverride([this, generation](DnsOverride override) {
    if (generation != m_dnsGeneration) {
      return;
    }
    m_dnsProfile = override.profile;
    if (m_dnsNextDnsInput != nullptr && !override.nextDnsId.empty()) {
      m_dnsNextDnsInput->setValue(override.nextDnsId);
    }
    if (m_dnsCustomInput != nullptr && override.profile == DnsProfile::Custom && !override.servers.empty()) {
      std::string joined;
      for (std::size_t i = 0; i < override.servers.size(); ++i) {
        if (i > 0) {
          joined += ", ";
        }
        joined += override.servers[i];
      }
      m_dnsCustomInput->setValue(joined);
    }
    syncDnsPicker();
    PanelManager::instance().requestLayout();
  });
}

void NetworkTab::applySelectedDnsProfile() {
  if (m_network == nullptr || !m_network->supportsDnsOverride()) {
    if (m_toolsStatus != nullptr) {
      m_toolsStatus->setText(i18n::tr("control-center.network.dns-requires-networkmanager"));
    }
    return;
  }
  if (!m_network->state().connected) {
    if (m_toolsStatus != nullptr) {
      m_toolsStatus->setText(i18n::tr("control-center.network.dns-requires-connection"));
    }
    return;
  }

  DnsOverride override;
  override.profile = m_dnsProfile;
  if (m_dnsProfile == DnsProfile::NextDns) {
    override.nextDnsId = m_dnsNextDnsInput != nullptr ? StringUtils::trim(m_dnsNextDnsInput->value()) : std::string{};
    if (!dns_override::validNextDnsId(override.nextDnsId)) {
      if (m_toolsStatus != nullptr) {
        m_toolsStatus->setText(i18n::tr("control-center.network.dns-invalid"));
      }
      return;
    }
  } else if (m_dnsProfile == DnsProfile::Custom) {
    const std::string text = m_dnsCustomInput != nullptr ? m_dnsCustomInput->value() : std::string{};
    std::vector<ParsedDnsServer> parsed;
    if (!dns_override::parseServerList(text, parsed)) {
      if (m_toolsStatus != nullptr) {
        m_toolsStatus->setText(i18n::tr("control-center.network.dns-invalid"));
      }
      return;
    }
    override.servers.reserve(parsed.size());
    for (const auto& server : parsed) {
      override.servers.push_back(dns_override::formatServer(server));
    }
  }
  applyDnsOverride(override);
}

void NetworkTab::applyDnsOverride(const DnsOverride& override) {
  if (m_network == nullptr) {
    return;
  }
  const std::uint64_t generation = ++m_dnsGeneration;
  m_dnsRunning = true;
  syncDnsPicker();
  m_network->setDnsOverride(override, [this, generation](bool success, std::string message) {
    if (generation != m_dnsGeneration) {
      return;
    }
    m_dnsRunning = false;
    syncDnsPicker();
    if (m_toolsStatus != nullptr) {
      if (success) {
        m_toolsStatus->setText(i18n::tr("control-center.network.dns-applied"));
      } else if (!message.empty()) {
        m_toolsStatus->setText(message);
      } else {
        m_toolsStatus->setText(i18n::tr("control-center.network.dns-invalid"));
      }
    }
    if (success) {
      persistDnsFields();
      refreshDnsOverride();
    }
    PanelManager::instance().requestLayout();
  });
}

void NetworkTab::persistDnsFields() {
  if (m_config == nullptr) {
    return;
  }
  const auto& networkTab = m_config->config().controlCenter.networkTab;
  if (m_dnsProfile == DnsProfile::NextDns && m_dnsNextDnsInput != nullptr) {
    const std::string id = StringUtils::trim(m_dnsNextDnsInput->value());
    if (id != networkTab.nextDnsId) {
      (void)m_config->setOverride({"control_center", "network", "nextdns_id"}, id);
    }
  }
  if (m_dnsProfile == DnsProfile::Custom && m_dnsCustomInput != nullptr) {
    const std::string custom = StringUtils::trim(m_dnsCustomInput->value());
    if (custom != networkTab.customDns) {
      (void)m_config->setOverride({"control_center", "network", "custom_dns"}, custom);
    }
  }
}

std::vector<NetworkTab::Catalog> NetworkTab::catalogOrder(bool hasVpns, bool hasCellular) const {
  std::vector<Catalog> order;
  order.push_back(Catalog::Wifi);
  if (hasVpns) {
    order.push_back(Catalog::Vpn);
  }
  if (hasCellular) {
    order.push_back(Catalog::Cellular);
  }
  return order;
}

void NetworkTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  const char* value = "wifi";
  if (m_catalog == Catalog::Vpn) {
    value = "vpn";
  } else if (m_catalog == Catalog::Cellular) {
    value = "cellular";
  }
  (void)m_config->setStateString("network_tab", "catalog", value);
}

void NetworkTab::applyCatalog(std::size_t index) {
  if (m_catalogSyncing || index >= m_catalogOrder.size()) {
    return;
  }
  if (m_catalog == m_catalogOrder[index]) {
    return;
  }
  m_catalog = m_catalogOrder[index];
  persistCatalog();
  m_lastStructureKey.clear();
  if (auto* panels = PanelManager::current(); panels != nullptr) {
    panels->requestLayout();
  }
}

void NetworkTab::syncCatalogBar(bool hasVpns, bool hasCellular) {
  auto order = catalogOrder(hasVpns, hasCellular);
  if (std::ranges::find(order, m_catalog) == order.end()) {
    m_catalog = Catalog::Wifi;
    persistCatalog();
  }
  const bool show = order.size() > 1;
  (void)setShown(m_catalogHost, show);
  (void)setShown(m_catalogSwitch, show);
  if (!show || m_catalogSwitch == nullptr) {
    m_catalogOrder = std::move(order);
    return;
  }
  if (order == m_catalogOrder && m_catalogSwitch->selectedIndex() < order.size()
      && order[m_catalogSwitch->selectedIndex()] == m_catalog) {
    return;
  }
  m_catalogOrder = std::move(order);
  m_catalogSyncing = true;
  m_catalogSwitch->clearOptions();
  std::size_t selected = 0;
  for (std::size_t i = 0; i < m_catalogOrder.size(); ++i) {
    const char* glyph = "wifi";
    const char* tooltipKey = "control-center.network.wifi";
    if (m_catalogOrder[i] == Catalog::Vpn) {
      glyph = "plug";
      tooltipKey = "control-center.network.vpns";
    } else if (m_catalogOrder[i] == Catalog::Cellular) {
      glyph = "device-mobile";
      tooltipKey = "control-center.network.cellular";
    }
    m_catalogSwitch->addOption("", glyph);
    m_catalogSwitch->setOptionTooltip(i, i18n::tr(tooltipKey));
    if (m_catalogOrder[i] == m_catalog) {
      selected = i;
    }
  }
  m_catalogSwitch->setSelectedIndex(selected);
  m_catalogSyncing = false;
}
