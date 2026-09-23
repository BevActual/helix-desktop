#include "shell/control_center/tabs/tailscale_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "net/tailscale_service.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] bool peerMatches(const TailscalePeer& peer, std::string_view filter) {
    if (filter.empty()) {
      return true;
    }
    const std::string needle = StringUtils::toLower(filter);
    const auto contains = [&](std::string_view value) {
      return StringUtils::toLower(value).find(needle) != std::string::npos;
    };
    return contains(peer.name) || contains(peer.dnsName) || contains(peer.ipv4) || contains(peer.os);
  }

  [[nodiscard]] std::string exitTarget(const TailscaleExitNode& node) {
    if (!node.ip.empty()) {
      return node.ip;
    }
    if (!node.name.empty()) {
      return node.name;
    }
    return node.id;
  }

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  [[nodiscard]] std::string joinHealth(const std::vector<std::string>& health) {
    std::string out;
    for (const auto& line : health) {
      if (line.empty()) {
        continue;
      }
      if (!out.empty()) {
        out += " · ";
      }
      out += line;
    }
    return out;
  }

  [[nodiscard]] std::string backendLabel(std::string_view state) {
    if (state.empty() || state == "Stopped" || state == "NoState") {
      return i18n::tr("control-center.tailscale.stopped");
    }
    if (state == "Running") {
      return i18n::tr("control-center.tailscale.connected");
    }
    if (state == "NeedsLogin") {
      return i18n::tr("control-center.tailscale.needs-login");
    }
    if (state == "Starting") {
      return i18n::tr("control-center.tailscale.starting");
    }
    if (state == "Stopping") {
      return i18n::tr("control-center.tailscale.stopping");
    }
    return std::string{state};
  }

} // namespace

TailscaleTab::Catalog TailscaleTab::catalogFromIndex(std::size_t index) noexcept {
  return index == 1 ? Catalog::Network : Catalog::Devices;
}

std::size_t TailscaleTab::indexFromCatalog(Catalog catalog) noexcept {
  return catalog == Catalog::Network ? 1 : 0;
}

TailscaleTab::TailscaleTab(const ControlCenterServices& services)
    : m_service(services.tailscale), m_clipboard(services.clipboard), m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "tailscale"})) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("tailscale_tab", "catalog")) {
      if (*catalog == "network") {
        m_catalog = Catalog::Network;
      }
    }
  }
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

std::unique_ptr<Flex> TailscaleTab::create() {
  const float scale = contentScale();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto statusCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applyCompactCardStyle(section, scale, opacity, false);
      },
  });

  statusCard->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  statusCard->addChild(
      ui::label({
          .out = &m_detail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  statusCard->addChild(
      ui::label({
          .out = &m_health,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .visible = false,
      })
  );

  auto ipRow = ui::row({
      .out = &m_ipRow,
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceSm * scale,
  });
  auto ips = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .flexGrow = 1.0F,
  });
  ips->addChild(
      ui::label({
          .out = &m_ipv4,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  ips->addChild(
      ui::label({
          .out = &m_ipv6,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  ipRow->addChild(std::move(ips));
  ipRow->addChild(
      ui::button({
          .out = &m_copyIpButton,
          .glyph = "copy",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.tailscale.copy-ip"),
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick =
              [this]() {
                if (m_service == nullptr) {
                  return;
                }
                const auto& snap = m_service->snapshot();
                copyText(!snap.ipv4.empty() ? snap.ipv4 : snap.ipv6);
              },
      })
  );
  statusCard->addChild(std::move(ipRow));

  auto actions = ui::row({
      .out = &m_actionsRow,
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceSm * scale,
  });
  actions->addChild(
      ui::button({
          .out = &m_connectButton,
          .text = i18n::tr("control-center.tailscale.connect"),
          .glyph = "circles",
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->toggle();
                }
              },
      })
  );
  actions->addChild(
      ui::button({
          .out = &m_adminButton,
          .text = i18n::tr("control-center.tailscale.open-admin"),
          .glyph = "external-link",
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->openAdmin();
                }
              },
      })
  );
  statusCard->addChild(std::move(actions));
  statusCard->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "devices",
                      .tooltip = i18n::tr("control-center.tailscale.peers"),
                  },
                  {
                      .label = "",
                      .glyph = "settings",
                      .tooltip = i18n::tr("control-center.tailscale.preferences"),
                  },
              },
          .selectedIndex = indexFromCatalog(m_catalog),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyCatalog(index); },
      })
  );
  live->addChild(std::move(statusCard));

  auto leftover = ui::scrollView({
      .out = &m_liveScroll,
      .state = &m_liveScrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  auto* leftoverContent = leftover->content();
  leftoverContent->setDirection(FlexDirection::Vertical);
  leftoverContent->setAlign(FlexAlign::Stretch);
  leftoverContent->setGap(Style::spaceMd * scale);

  auto prefsCard = ui::column({
      .out = &m_prefsCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*prefsCard, i18n::tr("control-center.tailscale.preferences"), scale);
  addToggleRow(*prefsCard, scale, nullptr, &m_shields, i18n::tr("control-center.tailscale.shields"));
  addToggleRow(*prefsCard, scale, nullptr, &m_routes, i18n::tr("control-center.tailscale.routes"));
  addToggleRow(*prefsCard, scale, nullptr, &m_ssh, i18n::tr("control-center.tailscale.ssh"));
  addToggleRow(*prefsCard, scale, nullptr, &m_dns, i18n::tr("control-center.tailscale.dns"));
  addToggleRow(*prefsCard, scale, nullptr, &m_advertiseExit, i18n::tr("control-center.tailscale.advertise-exit"));
  addToggleRow(*prefsCard, scale, nullptr, &m_allowLan, i18n::tr("control-center.tailscale.allow-lan"));

  prefsCard->addChild(
      ui::label({
          .text = i18n::tr("control-center.tailscale.exit-node"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  prefsCard->addChild(
      ui::select({
          .out = &m_exitSelect,
          .options = std::vector<std::string>{i18n::tr("control-center.tailscale.exit-none")},
          .selectedIndex = std::size_t{0},
          .surfaceOpacity = panelCardOpacity(),
          .onSelectionChanged =
              [this](std::size_t index, std::string_view /*text*/) {
                if (m_syncing || m_service == nullptr) {
                  return;
                }
                if (index == 0 || index > m_exitTargets.size()) {
                  m_service->setExitNode({});
                  return;
                }
                m_service->setExitNode(m_exitTargets[index - 1]);
              },
      })
  );
  auto peersCard = ui::column({
      .out = &m_peersCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*peersCard, i18n::tr("control-center.tailscale.peers"), scale);
  peersCard->addChild(
      ui::input({
          .out = &m_peerFilter,
          .placeholder = i18n::tr("control-center.tailscale.filter"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .clearButtonEnabled = true,
          .onChange =
              [this](const std::string& value) {
                if (m_peerFilterText == value) {
                  return;
                }
                m_peerFilterText = value;
                m_peerSignature.clear();
                rebuildPeers();
                PanelManager::instance().requestLayout();
              },
      })
  );

  peersCard->addChild(
      ui::column({
          .out = &m_peerList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );
  leftoverContent->addChild(std::move(prefsCard));
  leftoverContent->addChild(std::move(peersCard));
  live->addChild(std::move(leftover));

  if (m_shields != nullptr) {
    m_shields->setOnChange([this](bool value) {
      if (!m_syncing && m_service != nullptr) {
        m_service->setShieldsUp(value);
      }
    });
  }
  if (m_routes != nullptr) {
    m_routes->setOnChange([this](bool value) {
      if (!m_syncing && m_service != nullptr) {
        m_service->setAcceptRoutes(value);
      }
    });
  }
  if (m_ssh != nullptr) {
    m_ssh->setOnChange([this](bool value) {
      if (!m_syncing && m_service != nullptr) {
        m_service->setSsh(value);
      }
    });
  }
  if (m_dns != nullptr) {
    m_dns->setOnChange([this](bool value) {
      if (!m_syncing && m_service != nullptr) {
        m_service->setAcceptDns(value);
      }
    });
  }
  if (m_advertiseExit != nullptr) {
    m_advertiseExit->setOnChange([this](bool value) {
      if (!m_syncing && m_service != nullptr) {
        m_service->setAdvertiseExitNode(value);
      }
    });
  }
  if (m_allowLan != nullptr) {
    m_allowLan->setOnChange([this](bool value) {
      if (!m_syncing && m_service != nullptr) {
        m_service->setAllowLan(value);
      }
    });
  }

  auto wrapped = m_settings->wrapLive(std::move(live), contentScale(), panelCardOpacity());
  syncUi();
  return wrapped;
}

std::unique_ptr<Flex> TailscaleTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.tailscale.refresh"),
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->refresh();
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return m_settings->embedHeader(std::move(live), scale);
}

void TailscaleTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool TailscaleTab::configureMode() const { return m_settings->configureMode(); }

void TailscaleTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

void TailscaleTab::setActive(bool active) {
  if (active && m_service != nullptr) {
    m_service->refresh();
  }
}

void TailscaleTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_status = nullptr;
  m_detail = nullptr;
  m_health = nullptr;
  m_ipRow = nullptr;
  m_ipv4 = nullptr;
  m_ipv6 = nullptr;
  m_actionsRow = nullptr;
  m_connectButton = nullptr;
  m_copyIpButton = nullptr;
  m_adminButton = nullptr;
  m_catalogSwitch = nullptr;
  m_shields = nullptr;
  m_routes = nullptr;
  m_ssh = nullptr;
  m_dns = nullptr;
  m_advertiseExit = nullptr;
  m_allowLan = nullptr;
  m_exitSelect = nullptr;
  m_peerFilter = nullptr;
  m_peerList = nullptr;
  m_liveScroll = nullptr;
  m_prefsCard = nullptr;
  m_peersCard = nullptr;
  m_peerSignature.clear();
  m_exitOptionsKey.clear();
}

void TailscaleTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void TailscaleTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

void TailscaleTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString(
      "tailscale_tab", "catalog", m_catalog == Catalog::Network ? "network" : "devices"
  );
}

void TailscaleTab::syncCatalog() {
  const bool devices = m_catalog == Catalog::Devices;
  (void)setShown(m_peersCard, devices);
  (void)setShown(m_prefsCard, !devices);
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_catalog)) {
    m_catalogSyncing = true;
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_catalog));
    m_catalogSyncing = false;
  }
}

void TailscaleTab::applyCatalog(std::size_t index) {
  if (m_catalogSyncing) {
    return;
  }
  const Catalog next = catalogFromIndex(index);
  if (next == m_catalog) {
    syncCatalog();
    return;
  }
  m_catalog = next;
  persistCatalog();
  syncCatalog();
  PanelManager::instance().requestLayout();
}

void TailscaleTab::addToggleRow(
    Flex& parent, float scale, Label** /*labelOut*/, Toggle** toggleOut, const std::string& label
) {
  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  row->addChild(
      ui::label({
          .text = label,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  row->addChild(
      ui::toggle({
          .out = toggleOut,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
      })
  );
  parent.addChild(std::move(row));
}

void TailscaleTab::copyText(std::string_view text) {
  if (m_clipboard == nullptr || text.empty()) {
    return;
  }
  (void)m_clipboard->copyText(std::string(text));
}

void TailscaleTab::syncUi() {
  if (m_status == nullptr) {
    return;
  }

  m_syncing = true;
  const TailscaleSnapshot empty;
  const TailscaleSnapshot& snap = m_service != nullptr ? m_service->snapshot() : empty;
  const bool installed = snap.installed;
  const bool running = snap.running;
  const bool interactive = installed && !snap.busy;

  const bool hasIp = !snap.ipv4.empty() || !snap.ipv6.empty();
  const std::string health = joinHealth(snap.health);
  const bool layoutDirty = setShown(m_catalogSwitch, installed) || setShown(m_ipRow, hasIp) ||
                            setShown(m_ipv4, !snap.ipv4.empty()) || setShown(m_ipv6, !snap.ipv6.empty()) ||
                            setShown(m_copyIpButton, hasIp) || setShown(m_actionsRow, installed) ||
                            setShown(m_health, !health.empty()) || setShown(m_liveScroll, installed);
  syncCatalog();
  if (!installed) {
    (void)setShown(m_prefsCard, false);
    (void)setShown(m_peersCard, false);
  }
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }

  std::string status = backendLabel(snap.backendState);
  if (!snap.hostname.empty() && installed) {
    status = std::format("{} · {}", snap.hostname, status);
  }
  if (snap.busy) {
    status = i18n::tr("control-center.tailscale.busy");
  }
  (void)m_status->setText(status);

  std::string detail;
  if (!installed) {
    detail = snap.error.empty() ? i18n::tr("control-center.tailscale.missing") : snap.error;
  } else if (!snap.error.empty()) {
    detail = snap.error;
  } else if (!snap.tailnet.empty() && !snap.dnsName.empty()) {
    detail = std::format("{} · {}", snap.tailnet, snap.dnsName);
  } else if (!snap.tailnet.empty()) {
    detail = snap.tailnet;
  } else if (!snap.dnsName.empty()) {
    detail = snap.dnsName;
  } else if (!snap.version.empty()) {
    detail = snap.version;
  }
  (void)m_detail->setText(detail);

  if (m_health != nullptr) {
    (void)m_health->setText(health);
  }

  if (m_ipv4 != nullptr && !snap.ipv4.empty()) {
    (void)m_ipv4->setText(std::format("IPv4  {}", snap.ipv4));
  }
  if (m_ipv6 != nullptr && !snap.ipv6.empty()) {
    (void)m_ipv6->setText(std::format("IPv6  {}", snap.ipv6));
  }

  if (m_copyIpButton != nullptr) {
    m_copyIpButton->setEnabled(installed && (!snap.ipv4.empty() || !snap.ipv6.empty()));
  }
  if (m_adminButton != nullptr) {
    m_adminButton->setEnabled(installed);
  }
  if (m_connectButton != nullptr) {
    m_connectButton->setEnabled(interactive);
    if (snap.backendState == "NeedsLogin") {
      m_connectButton->setText(i18n::tr("control-center.tailscale.login"));
    } else if (running) {
      m_connectButton->setText(i18n::tr("control-center.tailscale.disconnect"));
    } else {
      m_connectButton->setText(i18n::tr("control-center.tailscale.connect"));
    }
  }

  const auto setToggle = [interactive, running](Toggle* toggle, bool checked, bool needsRunning) {
    if (toggle == nullptr) {
      return;
    }
    toggle->setEnabled(interactive && (!needsRunning || running));
    toggle->setCheckedImmediate(checked);
  };
  setToggle(m_shields, snap.shieldsUp, true);
  setToggle(m_routes, snap.acceptRoutes, true);
  setToggle(m_ssh, snap.runSsh, true);
  setToggle(m_dns, snap.acceptDns, true);
  setToggle(m_advertiseExit, snap.advertiseExitNode, true);
  setToggle(m_allowLan, snap.exitNodeAllowLan, true);

  if (m_exitSelect != nullptr) {
    m_exitSelect->setEnabled(interactive && running);
    std::vector<std::string> options;
    options.push_back(i18n::tr("control-center.tailscale.exit-none"));
    m_exitTargets.clear();
    std::size_t selected = 0;
    std::string key = "none";
    for (const auto& node : snap.exitNodes) {
      const std::string target = exitTarget(node);
      std::string label = node.name.empty() ? target : node.name;
      if (!node.online) {
        label += " · ";
        label += i18n::tr("control-center.tailscale.offline");
      }
      options.push_back(label);
      m_exitTargets.push_back(target);
      key += '|';
      key += target;
      if (node.selected) {
        selected = m_exitTargets.size();
      }
    }
    if (selected == 0 && !snap.exitNode.empty()) {
      bool found = false;
      for (std::size_t i = 0; i < m_exitTargets.size(); ++i) {
        if (m_exitTargets[i] == snap.exitNode) {
          selected = i + 1;
          found = true;
          break;
        }
      }
      if (!found) {
        options.push_back(snap.exitNode);
        m_exitTargets.push_back(snap.exitNode);
        selected = m_exitTargets.size();
        key += "|current:";
        key += snap.exitNode;
      }
    }
    if (m_exitOptionsKey != key) {
      m_exitOptionsKey = std::move(key);
      m_exitSelect->setOptions(std::move(options));
    }
    m_exitSelect->setSelectedIndexSilently(selected);
  }

  rebuildPeers();
  m_syncing = false;
}

void TailscaleTab::rebuildPeers() {
  if (m_peerList == nullptr) {
    return;
  }

  const TailscaleSnapshot empty;
  const TailscaleSnapshot& snap = m_service != nullptr ? m_service->snapshot() : empty;
  std::string signature = m_peerFilterText;
  signature += '#';
  signature += std::to_string(snap.peerCount);
  for (const auto& peer : snap.peers) {
    if (!peerMatches(peer, m_peerFilterText)) {
      continue;
    }
    signature += '|';
    signature += peer.id;
    signature += peer.online ? '1' : '0';
    signature += peer.isExitNode ? 'E' : 'n';
    signature += peer.name;
    signature += peer.ipv4;
  }
  if (signature == m_peerSignature) {
    return;
  }
  m_peerSignature = std::move(signature);

  const float scale = contentScale();
  while (!m_peerList->children().empty()) {
    m_peerList->removeChild(m_peerList->children().front().get());
  }

  if (!snap.installed) {
    return;
  }

  int shown = 0;
  for (const auto& peer : snap.peers) {
    if (!peerMatches(peer, m_peerFilterText)) {
      continue;
    }
    ++shown;

    auto row = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
    });
    auto header = ui::row({
        .align = FlexAlign::Center,
        .wrap = true,
        .gap = Style::spaceSm * scale,
    });
    header->addChild(
        ui::label({
            .text = peer.name,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    header->addChild(
        ui::label({
            .text = peer.online ? i18n::tr("control-center.tailscale.online")
                                : i18n::tr("control-center.tailscale.offline"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(peer.online ? ColorRole::Primary : ColorRole::OnSurfaceVariant),
        })
    );
    if (!peer.ipv4.empty()) {
      header->addChild(
          ui::button({
              .glyph = "copy",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.tailscale.copy-ip"),
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .onClick = [this, ip = peer.ipv4]() { copyText(ip); },
          })
      );
    }
    if (peer.exitNodeOption && !peer.isExitNode) {
      header->addChild(
          ui::button({
              .text = i18n::tr("control-center.tailscale.use-exit"),
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * scale,
              .paddingH = Style::spaceSm * scale,
              .onClick =
                  [this, target = peer.ipv4.empty() ? peer.name : peer.ipv4]() {
                    if (m_service != nullptr) {
                      m_service->setExitNode(target);
                    }
                  },
          })
      );
    }
    row->addChild(std::move(header));

    std::string meta;
    if (!peer.ipv4.empty()) {
      meta = peer.ipv4;
    }
    if (!peer.os.empty()) {
      if (!meta.empty()) {
        meta += " · ";
      }
      meta += peer.os;
    }
    if (peer.isExitNode) {
      if (!meta.empty()) {
        meta += " · ";
      }
      meta += i18n::tr("control-center.tailscale.exit-node");
    }
    if (!meta.empty()) {
      row->addChild(
          ui::label({
              .text = std::move(meta),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }
    m_peerList->addChild(std::move(row));
  }

  if (shown == 0) {
    m_peerList->addChild(
        ui::label({
            .text = snap.peerCount == 0 ? i18n::tr("control-center.tailscale.peers-empty")
                                        : i18n::tr("control-center.tailscale.peers-none"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }
}
