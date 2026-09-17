#include "shell/control_center/tabs/firewall_tab.h"

#include "dbus/firewall/firewall_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <string>
#include <utility>

using namespace control_center;

namespace {

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  [[nodiscard]] std::string zoneLabel(std::string_view id) {
    if (id == "FedoraWorkstation") {
      return i18n::tr("control-center.firewall.zone-workstation");
    }
    if (id == "FedoraServer") {
      return i18n::tr("control-center.firewall.zone-server");
    }
    if (id == "public") {
      return i18n::tr("control-center.firewall.zone-public");
    }
    if (id == "home") {
      return i18n::tr("control-center.firewall.zone-home");
    }
    if (id == "work") {
      return i18n::tr("control-center.firewall.zone-work");
    }
    if (id == "trusted") {
      return i18n::tr("control-center.firewall.zone-trusted");
    }
    if (id == "drop") {
      return i18n::tr("control-center.firewall.zone-drop");
    }
    if (id == "block") {
      return i18n::tr("control-center.firewall.zone-block");
    }
    if (id == "internal") {
      return i18n::tr("control-center.firewall.zone-internal");
    }
    if (id == "external") {
      return i18n::tr("control-center.firewall.zone-external");
    }
    if (id == "dmz") {
      return i18n::tr("control-center.firewall.zone-dmz");
    }
    return std::string(id);
  }

  [[nodiscard]] std::string serviceLabel(std::string_view id) {
    if (id == "ssh") {
      return i18n::tr("control-center.firewall.service-ssh");
    }
    if (id == "kdeconnect") {
      return i18n::tr("control-center.firewall.service-kdeconnect");
    }
    if (id == "syncthing") {
      return i18n::tr("control-center.firewall.service-syncthing");
    }
    if (id == "syncthing-gui") {
      return i18n::tr("control-center.firewall.service-syncthing-gui");
    }
    if (id == "mdns") {
      return i18n::tr("control-center.firewall.service-mdns");
    }
    if (id == "samba-client") {
      return i18n::tr("control-center.firewall.service-samba-client");
    }
    if (id == "http") {
      return i18n::tr("control-center.firewall.service-http");
    }
    if (id == "https") {
      return i18n::tr("control-center.firewall.service-https");
    }
    if (id == "cockpit") {
      return i18n::tr("control-center.firewall.service-cockpit");
    }
    if (id == "wireguard") {
      return i18n::tr("control-center.firewall.service-wireguard");
    }
    return std::string(id);
  }

} // namespace

FirewallTab::FirewallTab(const ControlCenterServices& services) : m_service(services.firewall) {
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

std::unique_ptr<Flex> FirewallTab::create() {
  const float scale = contentScale();

  auto liveScroll = ui::scrollView({
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
  m_rootLayout = liveScroll->content();
  m_rootLayout->setDirection(FlexDirection::Vertical);
  m_rootLayout->setAlign(FlexAlign::Stretch);
  m_rootLayout->setGap(Style::spaceMd * scale);

  auto well = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*well, i18n::tr("control-center.firewall.title"), scale);
  well->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  well->addChild(
      ui::label({
          .out = &m_detail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  well->addChild(
      ui::button({
          .out = &m_startButton,
          .text = i18n::tr("control-center.firewall.start"),
          .glyph = "shield-lock",
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->tryStart();
                }
              },
      })
  );

  auto panicRow = ui::row({
      .out = &m_panicRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  panicRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.firewall.panic"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  panicRow->addChild(
      ui::toggle({
          .out = &m_panicToggle,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool value) {
                if (!m_syncing && m_service != nullptr) {
                  m_service->setPanic(value);
                }
              },
      })
  );
  well->addChild(std::move(panicRow));

  m_zoneDivider = addWellDivider(*well, scale);
  auto zoneCard = ui::column({
      .out = &m_zoneCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  addTitle(*zoneCard, i18n::tr("control-center.firewall.zone"), scale);
  zoneCard->addChild(
      ui::select({
          .out = &m_zoneSelect,
          .options = std::vector<std::string>{},
          .selectedIndex = std::size_t{0},
          .surfaceOpacity = panelCardOpacity(),
          .onSelectionChanged =
              [this](std::size_t index, std::string_view /*text*/) {
                if (m_syncing || m_service == nullptr || index >= m_zoneIds.size()) {
                  return;
                }
                m_service->setDefaultZone(m_zoneIds[index]);
              },
      })
  );
  well->addChild(std::move(zoneCard));

  m_servicesDivider = addWellDivider(*well, scale);
  auto servicesCard = ui::column({
      .out = &m_servicesCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  addTitle(*servicesCard, i18n::tr("control-center.firewall.services"), scale);
  servicesCard->addChild(
      ui::column({
          .out = &m_serviceList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );
  servicesCard->addChild(
      ui::button({
          .out = &m_saveButton,
          .text = i18n::tr("control-center.firewall.save"),
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.firewall.save-tooltip"),
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->savePermanent();
                }
              },
      })
  );
  well->addChild(std::move(servicesCard));
  m_rootLayout->addChild(std::move(well));

  auto live = ui::column({
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });
  live->addChild(std::move(liveScroll));
  syncUi();
  return live;
}

std::unique_ptr<Flex> FirewallTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.firewall.refresh"),
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->refresh();
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void FirewallTab::setActive(bool active) {
  if (active && m_service != nullptr) {
    m_service->refresh();
  }
}

void FirewallTab::onClose() {
  m_rootLayout = nullptr;
  m_status = nullptr;
  m_detail = nullptr;
  m_startButton = nullptr;
  m_panicRow = nullptr;
  m_panicToggle = nullptr;
  m_zoneCard = nullptr;
  m_zoneDivider = nullptr;
  m_zoneSelect = nullptr;
  m_servicesCard = nullptr;
  m_servicesDivider = nullptr;
  m_serviceList = nullptr;
  m_saveButton = nullptr;
  m_liveScroll = nullptr;
  m_zoneIds.clear();
  m_zoneOptionsKey.clear();
  m_serviceSignature.clear();
}

void FirewallTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_liveScroll == nullptr) {
    return;
  }
  m_liveScroll->setSize(contentWidth, bodyHeight);
  m_liveScroll->layout(renderer);
}

void FirewallTab::syncUi() {
  if (m_status == nullptr) {
    return;
  }

  const FirewallSnapshot empty;
  const FirewallSnapshot& snap = m_service != nullptr ? m_service->snapshot() : empty;
  const bool running = snap.running;
  const bool showLists = running && !snap.panic;
  const bool layoutDirty = setShown(m_startButton, !running) || setShown(m_panicRow, running) ||
                            setShown(m_zoneDivider, showLists) || setShown(m_zoneCard, showLists) ||
                            setShown(m_servicesDivider, showLists) || setShown(m_servicesCard, showLists);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }

  if (m_startButton != nullptr) {
    m_startButton->setEnabled(m_service != nullptr && snap.installed && !snap.busy);
  }

  m_syncing = true;
  if (m_panicToggle != nullptr) {
    m_panicToggle->setCheckedImmediate(snap.panic);
    m_panicToggle->setEnabled(running && !snap.busy);
  }

  if (!snap.installed) {
    m_status->setText(i18n::tr("control-center.firewall.stopped"));
    m_detail->setText(i18n::tr("control-center.firewall.missing"));
    m_syncing = false;
    return;
  }
  if (!running) {
    m_status->setText(
        snap.busy ? i18n::tr("control-center.firewall.starting") : i18n::tr("control-center.firewall.stopped")
    );
    m_detail->setText(snap.error.empty() ? i18n::tr("control-center.firewall.ready-hint") : snap.error);
    m_syncing = false;
    return;
  }

  if (snap.panic) {
    m_status->setText(i18n::tr("control-center.firewall.panic-on"));
  } else {
    m_status->setText(
        snap.defaultZone.empty() ? i18n::tr("control-center.firewall.ready")
                                 : i18n::tr("control-center.firewall.visible-as", "name", zoneLabel(snap.defaultZone))
    );
  }
  std::string detail = snap.error;
  if (detail.empty()) {
    if (snap.interfaces.empty()) {
      detail = i18n::tr("control-center.firewall.no-interfaces");
    } else {
      detail = i18n::tr("control-center.firewall.interfaces", "names", snap.interfaces.front());
      for (std::size_t i = 1; i < snap.interfaces.size(); ++i) {
        detail += ", ";
        detail += snap.interfaces[i];
      }
    }
  }
  m_detail->setText(detail);

  if (m_zoneSelect != nullptr) {
    std::string key = snap.defaultZone;
    for (const auto& id : snap.zones) {
      key += '|';
      key += id;
    }
    if (key != m_zoneOptionsKey) {
      m_zoneOptionsKey = key;
      m_zoneIds = snap.zones;
      std::vector<std::string> labels;
      labels.reserve(m_zoneIds.size());
      for (const auto& id : m_zoneIds) {
        labels.push_back(zoneLabel(id));
      }
      m_zoneSelect->setOptions(std::move(labels));
    }
    std::size_t selected = 0;
    for (std::size_t i = 0; i < m_zoneIds.size(); ++i) {
      if (m_zoneIds[i] == snap.defaultZone) {
        selected = i;
        break;
      }
    }
    m_zoneSelect->setSelectedIndexSilently(selected);
  }

  m_syncing = false;
  rebuildServices();
}

void FirewallTab::rebuildServices() {
  if (m_serviceList == nullptr || m_service == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  std::string signature;
  for (const auto& service : snap.services) {
    signature += service.id;
    signature += service.enabled ? '1' : '0';
    signature += service.featured ? 'f' : 'x';
    signature += '|';
  }
  if (signature == m_serviceSignature && !m_serviceList->children().empty()) {
    return;
  }
  m_serviceSignature = std::move(signature);
  while (!m_serviceList->children().empty()) {
    m_serviceList->removeChild(m_serviceList->children().front().get());
  }

  const float scale = contentScale();
  if (snap.services.empty()) {
    m_serviceList->addChild(
        ui::label({
            .text = i18n::tr("control-center.firewall.services-empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    return;
  }

  for (const auto& service : snap.services) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = Style::controlHeightSm * scale,
    });
    row->addChild(
        ui::label({
            .text = serviceLabel(service.id),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    row->addChild(
        ui::toggle({
            .checkedImmediate = service.enabled,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange =
                [this, id = service.id](bool value) {
                  if (m_service != nullptr) {
                    m_service->setServiceEnabled(id, value);
                  }
                },
        })
    );
    m_serviceList->addChild(std::move(row));
  }
}
