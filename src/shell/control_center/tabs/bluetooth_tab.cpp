#include "shell/control_center/tabs/bluetooth_tab.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/collapsible.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

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

  const char* glyphFor(BluetoothDeviceKind kind) {
    switch (kind) {
    case BluetoothDeviceKind::Headset:
      return "bluetooth-device-headset";
    case BluetoothDeviceKind::Headphones:
      return "bluetooth-device-headphones";
    case BluetoothDeviceKind::Earbuds:
      return "bluetooth-device-earbuds";
    case BluetoothDeviceKind::Speaker:
      return "bluetooth-device-speaker";
    case BluetoothDeviceKind::Microphone:
      return "bluetooth-device-microphone";
    case BluetoothDeviceKind::Mouse:
      return "bluetooth-device-mouse";
    case BluetoothDeviceKind::Keyboard:
      return "bluetooth-device-keyboard";
    case BluetoothDeviceKind::Phone:
      return "bluetooth-device-phone";
    case BluetoothDeviceKind::Computer:
      return "device-laptop";
    case BluetoothDeviceKind::Gamepad:
      return "bluetooth-device-gamepad";
    case BluetoothDeviceKind::Watch:
      return "bluetooth-device-watch";
    case BluetoothDeviceKind::Tv:
      return "bluetooth-device-tv";
    case BluetoothDeviceKind::Unknown:
    default:
      return "bluetooth-device-generic";
    }
  }

  enum class DeviceBucket : std::uint8_t {
    Connected,
    Paired,
    Available,
  };

  DeviceBucket bucketFor(const BluetoothDeviceInfo& d) {
    if (d.connected) {
      return DeviceBucket::Connected;
    }
    if (d.paired) {
      return DeviceBucket::Paired;
    }
    return DeviceBucket::Available;
  }

  int signalPercentFromRssi(std::int16_t rssi) {
    constexpr int kWeakRssi = -100;
    constexpr int kStrongRssi = -40;
    constexpr int kRange = kStrongRssi - kWeakRssi;
    const int clamped = std::clamp(static_cast<int>(rssi), kWeakRssi, kStrongRssi);
    return ((clamped - kWeakRssi) * 100 + kRange / 2) / kRange;
  }

  // Coarse signal band (0 weakest .. 4 strongest). Available devices order by band,
  // not by raw RSSI: every advertisement moves the RSSI a little, and ordering on it
  // reshuffles rows under the pointer mid-scan.
  int signalBandFromRssi(std::int16_t rssi) { return std::min(signalPercentFromRssi(rssi) / 20, 4); }

  std::string percentText(int percent) { return std::to_string(percent) + "%"; }

  // Bucket first, then strongest band, then a stable alias/path tiebreak so equally
  // named devices keep their order across rebuilds.
  std::vector<BluetoothDeviceInfo> sortedDevices(std::vector<BluetoothDeviceInfo> devices) {
    std::ranges::sort(devices, [](const BluetoothDeviceInfo& a, const BluetoothDeviceInfo& b) {
      const auto ba = bucketFor(a);
      const auto bb = bucketFor(b);
      if (ba != bb) {
        return static_cast<int>(ba) < static_cast<int>(bb);
      }
      if (ba == DeviceBucket::Available) {
        if (a.hasRssi != b.hasRssi) {
          return a.hasRssi;
        }
        if (a.hasRssi) {
          const int bandA = signalBandFromRssi(a.rssi);
          const int bandB = signalBandFromRssi(b.rssi);
          if (bandA != bandB) {
            return bandA > bandB;
          }
        }
      }
      if (a.alias != b.alias) {
        return a.alias < b.alias;
      }
      return a.path < b.path;
    });
    return devices;
  }

  std::unique_ptr<Flex> makeMetricPill(const char* glyphName, std::string text, float scale, Label** valueOut) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * 0.5F * scale},
        ui::glyph({
            .glyph = glyphName,
            .glyphSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }),
        ui::label({
            .out = valueOut,
            .text = std::move(text),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

} // namespace

class BluetoothDeviceRow : public Collapsible {
public:
  BluetoothDeviceRow(BluetoothDeviceInfo device, BluetoothService* service, ClipboardService* clipboard, float scale)
      : m_device(std::move(device)), m_service(service), m_clipboard(clipboard) {
    setScale(scale);
    setRadius(Style::scaledRadiusMd(scale));
    setFill(colorSpecFromRole(ColorRole::Surface));
    clearBorder();

    auto header = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .padding = Style::spaceSm * scale,
         .minHeight = kRowMinHeight * scale},
        ui::glyph({
            .glyph = glyphFor(m_device.kind),
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::label({
            .text = m_device.alias,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = m_device.connected ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    header->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);

    auto metrics = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
    });

    if (m_device.hasBattery) {
      metrics->addChild(
          makeMetricPill("battery", percentText(static_cast<int>(m_device.batteryPercent)), scale, &m_batteryValue)
      );
    }
    if (m_device.hasRssi && bucketFor(m_device) == DeviceBucket::Available) {
      metrics->addChild(
          makeMetricPill("antenna-bars-5", percentText(signalPercentFromRssi(m_device.rssi)), scale, &m_signalValue)
      );
    }
    if (!metrics->children().empty()) {
      header->addChild(std::move(metrics));
    }

    const auto bucket = bucketFor(m_device);

    if (m_device.connecting) {
      header->addChild(
          ui::spinner({
              .out = &m_connectingSpinner,
              .color = colorSpecFromRole(ColorRole::Primary),
              .spinnerSize = Style::baseGlyphSize * scale,
          })
      );
    } else {
      ButtonVariant primaryVariant = ButtonVariant::Default;
      std::string primaryGlyph;
      std::string primaryTooltip;
      switch (bucket) {
      case DeviceBucket::Connected:
        primaryVariant = ButtonVariant::Destructive;
        primaryGlyph = "plug-off";
        primaryTooltip = i18n::tr("control-center.bluetooth.disconnect");
        break;
      case DeviceBucket::Paired:
        primaryGlyph = "plug";
        primaryTooltip = i18n::tr("control-center.bluetooth.connect");
        break;
      case DeviceBucket::Available:
        primaryGlyph = "bluetooth";
        primaryTooltip = i18n::tr("control-center.bluetooth.pair");
        break;
      }
      auto primary = ui::button({
          .glyph = std::move(primaryGlyph),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = primaryVariant,
          .tooltip = std::move(primaryTooltip),
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() {
            if (m_service == nullptr) {
              return;
            }
            switch (bucketFor(m_device)) {
            case DeviceBucket::Connected:
              m_service->disconnectDevice(m_device.path);
              break;
            case DeviceBucket::Paired:
              m_service->connect(m_device.path);
              break;
            case DeviceBucket::Available:
              m_service->pair(m_device.path);
              break;
            }
            PanelManager::instance().refresh();
          },
      });
      header->addChild(std::move(primary));
    }

    if (m_device.paired) {
      header->addChild(
          ui::button({
              .glyph = "trash",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.bluetooth.forget"),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() {
                if (m_service != nullptr) {
                  m_service->forget(m_device.path);
                }
                PanelManager::instance().refresh();
              },
          })
      );
    }

    setHeader(std::move(header));

    auto bodyColumn = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure = [scale](Flex& col) {
          col.setPadding(
              Style::spaceXs * scale, Style::spaceMd * scale, Style::spaceSm * scale, Style::spaceMd * scale
          );
        },
    });

    if (m_device.paired) {
      bodyColumn->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
              ui::label({
                  .text = i18n::tr("control-center.bluetooth.auto-reconnect"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .flexGrow = 1.0F,
              }),
              ui::toggle({
                  .checkedImmediate = m_device.trusted,
                  .toggleSize = ToggleSize::Small,
                  .scale = scale,
                  .onChange = [this](bool checked) {
                    if (m_service != nullptr) {
                      m_service->setTrusted(m_device.path, checked);
                    }
                  },
              })
          )
      );
    }

    bodyColumn->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            ui::label({
                .text = i18n::tr("control-center.bluetooth.address"),
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .flexGrow = 1.0F,
            }),
            ui::label(
                {.text = m_device.address,
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::OnSurface)}
            ),
            ui::button({
                .glyph = "copy",
                .glyphSize = Style::fontSizeCaption * scale,
                .enabled = m_clipboard != nullptr && !m_device.address.empty(),
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("control-center.bluetooth.copy-address"),
                .minWidth = Style::controlHeightSm * scale,
                .minHeight = Style::controlHeightSm * scale,
                .padding = Style::spaceXs * scale,
                .onClick = [this]() {
                  if (m_clipboard != nullptr && !m_device.address.empty()) {
                    (void)m_clipboard->copyText(m_device.address);
                    PanelManager::instance().requestRedraw();
                  }
                },
            })
        )
    );

    setBody(std::move(bodyColumn));
  }

  void startConnectingSpinner() {
    if (m_connectingSpinner != nullptr) {
      m_connectingSpinner->start();
    }
  }

  [[nodiscard]] const std::string& devicePath() const noexcept { return m_device.path; }

  // Refresh the values that move while a scan runs. Which pills exist is fixed at
  // construction (it belongs to the list structure key); only their text is live.
  // Returns true when a value actually changed, i.e. the row needs relayout.
  bool syncLiveMetrics(const BluetoothDeviceInfo& device) {
    bool changed = false;
    if (m_batteryValue != nullptr && m_batteryValue->setText(percentText(static_cast<int>(device.batteryPercent)))) {
      changed = true;
    }
    if (m_signalValue != nullptr && m_signalValue->setText(percentText(signalPercentFromRssi(device.rssi)))) {
      changed = true;
    }
    m_device.batteryPercent = device.batteryPercent;
    m_device.rssi = device.rssi;
    return changed;
  }

private:
  BluetoothDeviceInfo m_device;
  BluetoothService* m_service = nullptr;
  ClipboardService* m_clipboard = nullptr;
  Spinner* m_connectingSpinner = nullptr;
  Label* m_batteryValue = nullptr;
  Label* m_signalValue = nullptr;
};

BluetoothTab::BluetoothTab(BluetoothService* service, BluetoothAgent* agent, const ControlCenterServices& services)
    : m_service(service), m_agent(agent), m_config(services.config), m_clipboard(services.clipboard),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "bluetooth"})) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> source = m_config->stateString("bluetooth_tab", "catalog")) {
      if (*source == "nearby") {
        m_catalog = Catalog::Nearby;
      }
    }
  }
}

BluetoothTab::~BluetoothTab() = default;

std::unique_ptr<Flex> BluetoothTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto pairingCard = ui::column({
      .out = &m_pairingCard,
      .visible = false,
      // Prompt sheet above the compact card: a solid well, not a second glass layer.
      .configure = [scale](Flex& card) { applyContentCardStyle(card, scale); },
  });

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingDetail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingCode,
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );

  auto pairingInputRow = ui::row(
      {.out = &m_pairingInputRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale, .visible = false},
      ui::input({
          .out = &m_pairingInput,
          .placeholder = i18n::tr("control-center.bluetooth.enter-code"),
          .surfaceOpacity = 1.0F,
          .flexGrow = 1.0F,
          .onSubmit = [this](const std::string& value) {
            if (m_agent == nullptr) {
              return;
            }
            const auto req = m_agent->pendingRequest();
            if (req.kind == BluetoothPairingKind::PinCode) {
              m_agent->submitPin(value);
            } else if (req.kind == BluetoothPairingKind::Passkey) {
              try {
                m_agent->submitPasskey(static_cast<std::uint32_t>(std::stoul(value)));
              } catch (...) {
                m_agent->cancelPending();
              }
            }
            PanelManager::instance().refresh();
          },
      })
  );
  pairingCard->addChild(std::move(pairingInputRow));

  auto pairingButtonRow = ui::row(
      {.out = &m_pairingButtonRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_pairingAccept,
          .text = i18n::tr("control-center.bluetooth.accept"),
          .variant = ButtonVariant::Default,
          .onClick =
              [this]() {
                if (m_agent == nullptr) {
                  return;
                }
                const auto req = m_agent->pendingRequest();
                switch (req.kind) {
                case BluetoothPairingKind::Confirm:
                case BluetoothPairingKind::Authorize:
                case BluetoothPairingKind::AuthorizeService:
                case BluetoothPairingKind::DisplayPinCode:
                  m_agent->acceptConfirm();
                  break;
                case BluetoothPairingKind::PinCode:
                  if (m_pairingInput != nullptr) {
                    m_agent->submitPin(m_pairingInput->value());
                  }
                  break;
                case BluetoothPairingKind::Passkey:
                  if (m_pairingInput != nullptr) {
                    try {
                      m_agent->submitPasskey(static_cast<std::uint32_t>(std::stoul(m_pairingInput->value())));
                    } catch (...) {
                      m_agent->cancelPending();
                    }
                  }
                  break;
                case BluetoothPairingKind::DisplayPasskey:
                  // Informational only: the code is typed on the device, and there is no
                  // Agent1 reply to send. Do not fall through to the canceling default.
                  break;
                default:
                  m_agent->cancelPending();
                  break;
                }
                PanelManager::instance().refresh();
              },
      }),
      ui::button({
          .out = &m_pairingReject,
          .text = i18n::tr("control-center.bluetooth.reject"),
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() {
            if (m_agent != nullptr) {
              m_agent->rejectConfirm();
            }
            PanelManager::instance().refresh();
          },
      })
  );
  pairingCard->addChild(std::move(pairingButtonRow));

  tab->addChild(std::move(pairingCard));

  const float controlHeight = Style::controlHeight * scale;
  auto adapterCard = ui::column({
      .out = &m_adapterCard,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applyCompactCardStyle(card, scale, opacity, false); },
  });
  auto powerRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = controlHeight,
  });
  powerRow->addChild(
      ui::label({
          .out = &m_adapterTitle,
          .text = i18n::tr("control-center.bluetooth.unavailable-title"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  );
  powerRow->addChild(
      ui::spinner({
          .out = &m_scanSpinner,
          .color = colorSpecFromRole(ColorRole::Primary),
          .spinnerSize = Style::baseGlyphSize * scale,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  powerRow->addChild(
      ui::button({
          .out = &m_rescanButton,
          .glyph = "refresh",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.bluetooth.rescan"),
          .minHeight = controlHeight,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() {
            if (m_service == nullptr) {
              return;
            }
            if (m_discoveryLease == DiscoveryLease::None) {
              m_discoveryLease = DiscoveryLease::Pending;
              m_service->startDiscovery();
            }
            m_discoveryTimer.start(discoveryTimeout(), [this]() { stopRequestedDiscovery(); });
          },
      })
  );
  powerRow->addChild(
      ui::toggle({
          .out = &m_powerToggle,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange = [this](bool checked) {
            if (m_service != nullptr) {
              m_service->setPowered(checked);
            }
          },
      })
  );
  adapterCard->addChild(std::move(powerRow));
  adapterCard->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "bluetooth",
                      .tooltip = i18n::tr("control-center.bluetooth.sections.paired"),
                  },
                  {
                      .label = "",
                      .glyph = "radar",
                      .tooltip = i18n::tr("control-center.bluetooth.nearby"),
                  },
              },
          .selectedIndex = m_catalog == Catalog::Nearby ? std::size_t{1} : std::size_t{0},
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyCatalog(index); },
      })
  );
  adapterCard->addChild(
      ui::label({
          .out = &m_adapterStatus,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  auto discoverableHost = ui::column({
      .out = &m_discoverableRow,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
  });
  addWellDivider(*discoverableHost, scale);
  m_discoverableToggle = addToggleRow(
      *discoverableHost, scale, i18n::tr("control-center.bluetooth.visible"),
      [this](bool checked) {
        if (m_service != nullptr) {
          m_service->setDiscoverable(checked);
        }
      }
  );
  adapterCard->addChild(std::move(discoverableHost));
  tab->addChild(std::move(adapterCard));

  auto listScroll = ui::scrollView({
      .out = &m_listScroll,
      .contentScale = contentScale(),
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

  tab->addChild(std::move(listScroll));
  return m_settings->wrapLive(std::move(tab), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> BluetoothTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void BluetoothTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool BluetoothTab::configureMode() const { return m_settings->configureMode(); }

void BluetoothTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

void BluetoothTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  syncPairingCard();
  syncHeader();
  m_rootLayout->layout(renderer);
  rebuildDeviceList(renderer);
  syncDeviceRows();
  m_rootLayout->layout(renderer);
}

void BluetoothTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  syncDiscoveryLease();
  syncPairingCard();
  syncHeader();
  rebuildDeviceList(renderer);
  // A metric pill's text changes its width, so the list has to be laid out again.
  if (syncDeviceRows() && m_list != nullptr) {
    m_list->layout(renderer);
  }
}

void BluetoothTab::setActive(bool active) {
  if (!active) {
    stopRequestedDiscovery();
  }
}

void BluetoothTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  stopRequestedDiscovery();
  m_rootLayout = nullptr;
  m_pairingCard = nullptr;
  m_pairingTitle = nullptr;
  m_pairingDetail = nullptr;
  m_pairingCode = nullptr;
  m_pairingInputRow = nullptr;
  m_pairingInput = nullptr;
  m_pairingButtonRow = nullptr;
  m_pairingAccept = nullptr;
  m_pairingReject = nullptr;
  m_adapterCard = nullptr;
  m_catalogSwitch = nullptr;
  m_discoverableRow = nullptr;
  m_adapterTitle = nullptr;
  m_adapterStatus = nullptr;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_powerToggle = nullptr;
  m_discoverableToggle = nullptr;
  m_rescanButton = nullptr;
  m_scanSpinner = nullptr;
  m_deviceRows.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0F;
}

void BluetoothTab::syncDiscoveryLease() {
  if (m_discoveryLease == DiscoveryLease::None || m_service == nullptr) {
    return;
  }

  const BluetoothState& s = m_service->state();
  if (!s.adapterPresent || !s.powered) {
    // A powered-down adapter cannot be discovering, so BlueZ already dropped the session.
    releaseDiscoveryLease();
    return;
  }
  if (m_discoveryLease == DiscoveryLease::Pending) {
    // StartDiscovery is async; the lease is only confirmed once BlueZ reports Discovering.
    if (s.discovering) {
      m_discoveryLease = DiscoveryLease::Active;
    }
    return;
  }
  if (!s.discovering) {
    // Discovery ended outside the tab: drop the lease so the next Rescan starts a new one.
    releaseDiscoveryLease();
  }
}

void BluetoothTab::releaseDiscoveryLease() {
  m_discoveryLease = DiscoveryLease::None;
  m_discoveryTimer.stop();
}

void BluetoothTab::stopRequestedDiscovery() {
  if (m_discoveryLease == DiscoveryLease::None) {
    return;
  }

  releaseDiscoveryLease();
  if (m_service != nullptr) {
    m_service->stopDiscovery();
  }
}

void BluetoothTab::syncHeader() {
  auto setAdapterTitle = [this](const std::string& text) {
    if (m_adapterTitle != nullptr) {
      m_adapterTitle->setText(text);
    }
  };
  if (m_service == nullptr) {
    if (m_powerToggle != nullptr) {
      m_powerToggle->setEnabled(false);
    }
    if (m_rescanButton != nullptr) {
      m_rescanButton->setEnabled(false);
    }
    setAdapterTitle(i18n::tr("control-center.bluetooth.unavailable-title"));
    if (m_adapterStatus != nullptr) {
      m_adapterStatus->setText(i18n::tr("control-center.bluetooth.unavailable"));
    }
    if (setShown(m_adapterStatus, true) || setShown(m_discoverableRow, false) || setShown(m_listScroll, false)) {
      PanelManager::instance().requestLayout();
    }
    return;
  }
  const BluetoothState& s = m_service->state();
  if (m_powerToggle != nullptr) {
    m_powerToggle->setChecked(s.powered);
    m_powerToggle->setEnabled(s.adapterPresent);
  }
  if (m_discoverableToggle != nullptr) {
    m_discoverableToggle->setChecked(s.discoverable);
    m_discoverableToggle->setEnabled(s.adapterPresent && s.powered);
  }
  if (m_scanSpinner != nullptr) {
    const bool spinnerVisible = s.discovering && s.powered && s.adapterPresent;
    m_scanSpinner->setVisible(spinnerVisible);
    m_scanSpinner->setParticipatesInLayout(spinnerVisible);
    if (spinnerVisible && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!spinnerVisible && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
  if (m_rescanButton != nullptr) {
    m_rescanButton->setEnabled(s.adapterPresent && s.powered);
  }
  if (!s.adapterPresent) {
    setAdapterTitle(i18n::tr("control-center.bluetooth.unavailable-title"));
  } else if (!s.adapterName.empty()) {
    setAdapterTitle(s.adapterName);
  } else if (s.powered) {
    setAdapterTitle(i18n::tr("control-center.bluetooth.on"));
  } else {
    setAdapterTitle(i18n::tr("control-center.bluetooth.off-title"));
  }
  const bool showList = s.adapterPresent && s.powered;
  std::string status;
  if (!s.adapterPresent) {
    status = i18n::tr("control-center.bluetooth.unavailable");
  } else if (s.rfkillSoftBlocked && !s.powered) {
    status = i18n::tr("control-center.bluetooth.rfkill-blocked");
  }
  if (m_adapterStatus != nullptr && !status.empty()) {
    m_adapterStatus->setText(status);
  }
  bool layoutDirty = setShown(m_adapterStatus, !status.empty());
  layoutDirty = setShown(m_discoverableRow, showList) || layoutDirty;
  layoutDirty = setShown(m_listScroll, showList) || layoutDirty;
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void BluetoothTab::syncPairingCard() {
  if (m_pairingCard == nullptr) {
    return;
  }
  const bool hasPending = m_agent != nullptr && m_agent->hasPendingRequest();
  if (setShown(m_pairingCard, hasPending)) {
    PanelManager::instance().requestLayout();
  }
  if (!hasPending) {
    return;
  }
  const auto req = m_agent->pendingRequest();
  std::string alias = req.devicePath;
  if (m_service != nullptr) {
    for (const auto& d : m_service->devices()) {
      if (d.path == req.devicePath && !d.alias.empty()) {
        alias = d.alias;
        break;
      }
    }
  }

  if (m_pairingTitle != nullptr) {
    m_pairingTitle->setText(i18n::tr("control-center.bluetooth.pair-title", "device", alias));
  }
  const bool needsInput = req.kind == BluetoothPairingKind::PinCode || req.kind == BluetoothPairingKind::Passkey;
  const bool showsCode = req.kind == BluetoothPairingKind::Confirm
      || req.kind == BluetoothPairingKind::DisplayPasskey
      || req.kind == BluetoothPairingKind::DisplayPinCode;

  if (m_pairingDetail != nullptr) {
    switch (req.kind) {
    case BluetoothPairingKind::Confirm:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.confirm"));
      break;
    case BluetoothPairingKind::Authorize:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.authorize"));
      break;
    case BluetoothPairingKind::AuthorizeService:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.authorize-service", "uuid", req.uuid));
      break;
    case BluetoothPairingKind::DisplayPinCode:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.display-pin"));
      break;
    case BluetoothPairingKind::DisplayPasskey:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.display-passkey"));
      break;
    case BluetoothPairingKind::PinCode:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.pin-code"));
      break;
    case BluetoothPairingKind::Passkey:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.passkey"));
      break;
    case BluetoothPairingKind::None:
      break;
    }
  }
  if (m_pairingCode != nullptr) {
    (void)setShown(m_pairingCode, showsCode);
    if (showsCode) {
      if (req.kind == BluetoothPairingKind::DisplayPinCode) {
        m_pairingCode->setText(req.pin);
      } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%06u", req.passkey);
        m_pairingCode->setText(buf);
      }
    }
  }
  if (m_pairingInputRow != nullptr) {
    (void)setShown(m_pairingInputRow, needsInput);
  }
  if (m_pairingAccept != nullptr) {
    // DisplayPasskey has nothing to accept: the code is entered on the device and there
    // is no Agent1 reply. Show only the Reject action so Accept cannot clear the card.
    (void)setShown(m_pairingAccept, req.kind != BluetoothPairingKind::DisplayPasskey);
  }
}

// Identity of the built list: which rows exist, in which order, and which controls
// each carries. Live metric values (battery percent, RSSI) are absent by design —
// they refresh in place through syncDeviceRows(), so an advertisement no longer tears
// down the list. Devices arrive sorted, so a change in row order changes the key.
std::string BluetoothTab::structureKey(const std::vector<BluetoothDeviceInfo>& devices) const {
  if (m_service == nullptr) {
    return "empty";
  }
  const auto& s = m_service->state();
  std::string key;
  key += s.adapterPresent ? '1' : '0';
  key += s.powered ? '1' : '0';
  key += s.rfkillSoftBlocked ? '1' : '0';
  key += m_catalog == Catalog::Nearby ? 'N' : 'P';
  key.push_back('|');
  for (const auto& d : devices) {
    key += d.path;
    key.push_back(':');
    key += d.alias;
    key.push_back(':');
    key += std::to_string(static_cast<int>(d.kind));
    key.push_back(':');
    key += d.paired ? '1' : '0';
    key += d.trusted ? '1' : '0';
    key += d.connected ? '1' : '0';
    key += d.connecting ? '1' : '0';
    key += d.hasBattery ? '1' : '0';
    key += d.hasRssi ? '1' : '0';
    key.push_back('\n');
  }
  if (m_config != nullptr) {
    key += "hide:";
    key += m_config->config().controlCenter.bluetoothTab.hideUnnamed ? '1' : '0';
  }
  return key;
}

bool BluetoothTab::syncDeviceRows() {
  if (m_service == nullptr || m_deviceRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& d : m_service->devices()) {
    const auto it = m_deviceRows.find(d.path);
    if (it != m_deviceRows.end() && it->second->syncLiveMetrics(d)) {
      changed = true;
    }
  }
  return changed;
}

void BluetoothTab::rebuildDeviceList(Renderer& renderer) {
  uiAssertNotRendering("BluetoothTab::rebuildDeviceList");
  if (m_list == nullptr || m_listScroll == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0F) {
    return;
  }
  std::vector<BluetoothDeviceInfo> devices;
  if (m_service != nullptr) {
    devices = sortedDevices(m_service->devices());
    applyDeviceFilters(devices);
  }
  const std::string nextKey = structureKey(devices);
  const bool structureChanged = nextKey != m_lastStructureKey;
  if (listWidth == m_lastListWidth && !structureChanged) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextKey;

  if (!structureChanged) {
    m_list->layout(renderer);
    return;
  }

  const float scale = contentScale();

  std::unordered_set<std::string> expandedPaths;
  for (const auto& [path, row] : m_deviceRows) {
    if (row->expanded()) {
      expandedPaths.insert(path);
    }
  }

  m_deviceRows.clear();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  if (m_service == nullptr || !m_service->state().adapterPresent || !m_service->state().powered) {
    m_list->layout(renderer);
    return;
  }

  std::erase_if(devices, [this](const BluetoothDeviceInfo& device) {
    const auto bucket = bucketFor(device);
    if (m_catalog == Catalog::Nearby) {
      return bucket != DeviceBucket::Available;
    }
    return bucket == DeviceBucket::Available;
  });

  Flex* well = nullptr;
  auto wellCard = ui::column({
      .out = &well,
      .configure = [scale](Flex& card) { applyContentCardStyle(card, scale); },
  });

  if (devices.empty()) {
    well->addChild(
        ui::label({
            .text = i18n::tr("control-center.bluetooth.no-devices"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    m_list->addChild(std::move(wellCard));
    m_list->layout(renderer);
    return;
  }

  DeviceBucket currentBucket = DeviceBucket::Connected;
  bool first = true;
  for (const auto& device : devices) {
    const auto bucket = bucketFor(device);
    if (m_catalog != Catalog::Nearby && (first || bucket != currentBucket)) {
      std::string sectionText;
      switch (bucket) {
      case DeviceBucket::Connected:
        sectionText = i18n::tr("control-center.bluetooth.sections.connected");
        break;
      case DeviceBucket::Paired:
        sectionText = i18n::tr("control-center.bluetooth.sections.paired");
        break;
      case DeviceBucket::Available:
        sectionText = i18n::tr("control-center.bluetooth.sections.available");
        break;
      }
      if (!first) {
        addWellDivider(*well, scale);
      }
      well->addChild(makeCardHeaderRow(sectionText, scale));
      currentBucket = bucket;
      first = false;
    }
    auto row = std::make_unique<BluetoothDeviceRow>(device, m_service, m_clipboard, scale);
    auto* rowPtr = row.get();
    well->addChild(std::move(row));
    rowPtr->setExpandedImmediate(expandedPaths.contains(device.path));
    rowPtr->startConnectingSpinner();
    m_deviceRows.emplace(rowPtr->devicePath(), rowPtr);
  }
  m_list->addChild(std::move(wellCard));
  m_list->layout(renderer);
}

void BluetoothTab::onPanelCardOpacityChanged(float opacity) {
  // The pairing prompt is a solid well; its code field stays opaque and does not follow panel glass.
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

std::chrono::seconds BluetoothTab::discoveryTimeout() const {
  std::int32_t seconds = 10;
  if (m_config != nullptr) {
    seconds = m_config->config().controlCenter.bluetoothTab.discoverySeconds;
  }
  return std::chrono::seconds{std::clamp(seconds, 5, 60)};
}

void BluetoothTab::applyDeviceFilters(std::vector<BluetoothDeviceInfo>& devices) const {
  if (m_config == nullptr || !m_config->config().controlCenter.bluetoothTab.hideUnnamed) {
    return;
  }
  std::erase_if(devices, [](const BluetoothDeviceInfo& device) {
    return device.alias.empty() || device.alias == device.address;
  });
}

void BluetoothTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString("bluetooth_tab", "catalog", m_catalog == Catalog::Nearby ? "nearby" : "paired");
}

void BluetoothTab::applyCatalog(std::size_t index) {
  if (m_catalogSyncing) {
    return;
  }
  const Catalog next = index == 1 ? Catalog::Nearby : Catalog::Paired;
  if (next == m_catalog) {
    return;
  }
  m_catalog = next;
  persistCatalog();
  m_lastStructureKey.clear();
  if (auto* panels = PanelManager::current(); panels != nullptr) {
    panels->requestLayout();
  }
}
