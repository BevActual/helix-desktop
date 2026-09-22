#include "shell/control_center/tabs/cast_tab.h"

#include "config/config_service.h"
#include "dbus/cast/cast_service.h"
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
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/clipboard_service.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

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

  [[nodiscard]] std::string protocolLabel(std::string_view protocol) {
    if (protocol == "chromecast") {
      return i18n::tr("control-center.cast.protocol-chromecast");
    }
    if (protocol == "miracast") {
      return i18n::tr("control-center.cast.protocol-miracast");
    }
    return i18n::tr("control-center.cast.protocol-display");
  }

  [[nodiscard]] std::string sinkDetail(const CastSink& sink) {
    std::string out = protocolLabel(sink.protocol);
    out += "  ·  ";
    if (sink.state == "streaming") {
      out += i18n::tr("control-center.cast.streaming");
    } else if (sink.state == "connecting") {
      out += i18n::tr("control-center.cast.connecting");
    } else if (sink.state == "error") {
      out += i18n::tr("control-center.cast.sink-error");
    } else {
      out += i18n::tr("control-center.cast.idle");
    }
    return out;
  }

} // namespace

CastTab::Catalog CastTab::catalogFromIndex(std::size_t index) noexcept {
  return index == 1 ? Catalog::Displays : Catalog::Wireless;
}

std::size_t CastTab::indexFromCatalog(Catalog catalog) noexcept {
  return catalog == Catalog::Displays ? 1 : 0;
}

CastTab::CastTab(const ControlCenterServices& services)
    : m_service(services.cast), m_clipboard(services.clipboard), m_config(services.config) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("cast_tab", "catalog")) {
      if (*catalog == "displays") {
        m_catalog = Catalog::Displays;
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

std::unique_ptr<Flex> CastTab::create() {
  const float scale = contentScale();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto well = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applyCompactCardStyle(section, scale, opacity, false);
      },
  });
  well->addChild(
      ui::label({
          .out = &m_shareStatus,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  well->addChild(
      ui::label({
          .out = &m_shareDetail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  auto shareToggleRow = ui::row({
      .out = &m_shareToggleRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  shareToggleRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.cast.share"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  shareToggleRow->addChild(
      ui::toggle({
          .out = &m_shareToggle,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool value) {
                if (!m_syncing && m_service != nullptr) {
                  m_service->setShareEnabled(value);
                }
              },
      })
  );
  well->addChild(std::move(shareToggleRow));

  auto viewOnlyRow = ui::row({
      .out = &m_viewOnlyRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  viewOnlyRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.cast.view-only"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  viewOnlyRow->addChild(
      ui::toggle({
          .out = &m_viewOnlyToggle,
          .checkedImmediate = true,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool value) {
                if (!m_syncing && m_service != nullptr) {
                  m_service->setViewOnly(value);
                }
              },
      })
  );
  well->addChild(std::move(viewOnlyRow));

  auto credentialsRow = ui::row({
      .out = &m_credentialsRow,
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceSm * scale,
  });
  credentialsRow->addChild(
      ui::label({
          .out = &m_credentials,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .flexGrow = 1.0F,
      })
  );
  credentialsRow->addChild(
      ui::button({
          .out = &m_copyButton,
          .glyph = "copy",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.cast.copy"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick = [this]() { copyShareTarget(); },
      })
  );
  well->addChild(std::move(credentialsRow));
  well->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "screen-share",
                      .tooltip = i18n::tr("control-center.cast.wireless-title"),
                  },
                  {
                      .label = "",
                      .glyph = "device-tv",
                      .tooltip = i18n::tr("control-center.cast.displays"),
                  },
              },
          .selectedIndex = indexFromCatalog(m_catalog),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyCatalog(index); },
      })
  );
  live->addChild(std::move(well));

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

  auto wireless = ui::column({
      .out = &m_wirelessCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*wireless, i18n::tr("control-center.cast.wireless-title"), scale);
  wireless->addChild(
      ui::label({
          .out = &m_wirelessStatus,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  wireless->addChild(
      ui::label({
          .out = &m_wirelessDetail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  wireless->addChild(
      ui::button({
          .out = &m_wirelessStart,
          .text = i18n::tr("control-center.cast.wireless-start"),
          .glyph = "screen-share",
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->tryStartWireless();
                }
              },
      })
  );
  wireless->addChild(
      ui::button({
          .out = &m_wirelessOpen,
          .text = i18n::tr("control-center.cast.wireless-open"),
          .glyph = "external-link",
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->openWirelessApp();
                }
              },
      })
  );

  auto sinksCard = ui::column({
      .out = &m_sinksCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*sinksCard, i18n::tr("control-center.cast.displays"), scale);
  sinksCard->addChild(
      ui::column({
          .out = &m_sinkList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );
  leftoverContent->addChild(std::move(wireless));
  leftoverContent->addChild(std::move(sinksCard));
  live->addChild(std::move(leftover));
  syncUi();
  return live;
}

std::unique_ptr<Flex> CastTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.cast.refresh"),
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

void CastTab::setActive(bool active) {
  if (m_service != nullptr) {
    m_service->setWantFastPoll(active);
    if (active) {
      m_service->refresh();
    }
  }
}

void CastTab::onClose() {
  if (m_service != nullptr) {
    m_service->setWantFastPoll(false);
  }
  m_rootLayout = nullptr;
  m_shareStatus = nullptr;
  m_shareDetail = nullptr;
  m_shareToggleRow = nullptr;
  m_shareToggle = nullptr;
  m_viewOnlyRow = nullptr;
  m_viewOnlyToggle = nullptr;
  m_credentialsRow = nullptr;
  m_credentials = nullptr;
  m_copyButton = nullptr;
  m_catalogSwitch = nullptr;
  m_wirelessCard = nullptr;
  m_wirelessStatus = nullptr;
  m_wirelessDetail = nullptr;
  m_wirelessStart = nullptr;
  m_wirelessOpen = nullptr;
  m_sinksCard = nullptr;
  m_sinkList = nullptr;
  m_liveScroll = nullptr;
  m_sinkSignature.clear();
}

void CastTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void CastTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

void CastTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString(
      "cast_tab", "catalog", m_catalog == Catalog::Displays ? "displays" : "wireless"
  );
}

void CastTab::syncCatalog() {
  const bool wireless = m_catalog == Catalog::Wireless;
  (void)setShown(m_wirelessCard, wireless);
  (void)setShown(m_sinksCard, !wireless);
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_catalog)) {
    m_catalogSyncing = true;
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_catalog));
    m_catalogSyncing = false;
  }
}

void CastTab::applyCatalog(std::size_t index) {
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

void CastTab::copyShareTarget() {
  if (m_service == nullptr || m_clipboard == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  const std::string text = snap.shareAddress.empty() ? snap.username : snap.shareAddress;
  if (!text.empty()) {
    m_clipboard->setClipboardText(text);
  }
}

void CastTab::syncUi() {
  if (m_shareStatus == nullptr) {
    return;
  }

  const CastSnapshot empty;
  const CastSnapshot& snap = m_service != nullptr ? m_service->snapshot() : empty;
  const bool sharing = snap.shareEnabled;
  const bool wireless = snap.wirelessRunning;
  const bool layoutDirty = setShown(m_shareToggleRow, snap.shareInstalled) ||
                            setShown(m_viewOnlyRow, sharing) || setShown(m_credentialsRow, sharing) ||
                            setShown(m_copyButton, sharing && !snap.shareAddress.empty()) ||
                            setShown(m_wirelessStart, snap.wirelessInstalled && !wireless) ||
                            setShown(m_wirelessOpen, snap.wirelessInstalled);
  syncCatalog();
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }

  m_syncing = true;
  if (m_shareToggle != nullptr) {
    m_shareToggle->setCheckedImmediate(sharing);
    m_shareToggle->setEnabled(m_service != nullptr && snap.shareInstalled && !snap.shareBusy);
  }
  if (m_viewOnlyToggle != nullptr) {
    m_viewOnlyToggle->setCheckedImmediate(snap.viewOnly);
    m_viewOnlyToggle->setEnabled(sharing && !snap.shareBusy);
  }
  if (m_wirelessStart != nullptr) {
    m_wirelessStart->setEnabled(m_service != nullptr && snap.wirelessInstalled && !snap.wirelessBusy);
  }

  if (!snap.shareInstalled) {
    m_shareStatus->setText(i18n::tr("control-center.cast.share-off"));
    m_shareDetail->setText(i18n::tr("control-center.cast.share-missing"));
  } else if (snap.shareBusy) {
    m_shareStatus->setText(i18n::tr("control-center.cast.starting"));
    m_shareDetail->setText(snap.error.empty() ? i18n::tr("control-center.cast.ready-hint") : snap.error);
  } else if (sharing) {
    m_shareStatus->setText(
        snap.shareAddress.empty() ? i18n::tr("control-center.cast.share-on")
                                 : i18n::tr("control-center.cast.share-listening", "address", snap.shareAddress)
    );
    if (m_credentials != nullptr) {
      if (!snap.username.empty()) {
        m_credentials->setText(i18n::tr("control-center.cast.credentials", "user", snap.username));
      } else {
        m_credentials->setText(i18n::tr("control-center.cast.copy-hint"));
      }
    }
    m_shareDetail->setText(snap.error.empty() ? i18n::tr("control-center.cast.share-hint") : snap.error);
  } else {
    m_shareStatus->setText(i18n::tr("control-center.cast.share-off"));
    m_shareDetail->setText(snap.error.empty() ? i18n::tr("control-center.cast.share-off-hint") : snap.error);
  }

  if (m_wirelessStatus != nullptr) {
    if (!snap.wirelessInstalled) {
      m_wirelessStatus->setText(i18n::tr("control-center.cast.wireless-off"));
      m_wirelessDetail->setText(i18n::tr("control-center.cast.wireless-missing"));
    } else if (!wireless) {
      m_wirelessStatus->setText(
          snap.wirelessBusy ? i18n::tr("control-center.cast.starting") : i18n::tr("control-center.cast.wireless-off")
      );
      m_wirelessDetail->setText(
          snap.error.empty() ? i18n::tr("control-center.cast.wireless-hint") : snap.error
      );
    } else {
      m_wirelessStatus->setText(i18n::tr("control-center.cast.wireless-on"));
      m_wirelessDetail->setText(i18n::tr("control-center.cast.wireless-on-hint"));
    }
  }

  m_syncing = false;
  rebuildSinks();
}

void CastTab::rebuildSinks() {
  if (m_sinkList == nullptr || m_service == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  std::string signature;
  for (const auto& sink : snap.sinks) {
    signature += sink.uuid;
    signature += sink.name;
    signature += sink.state;
    signature += sink.unit;
    signature += '|';
  }
  if (signature == m_sinkSignature && !m_sinkList->children().empty()) {
    return;
  }
  m_sinkSignature = std::move(signature);
  while (!m_sinkList->children().empty()) {
    m_sinkList->removeChild(m_sinkList->children().front().get());
  }
  const float scale = contentScale();
  if (snap.sinks.empty()) {
    m_sinkList->addChild(
        ui::label({
            .text = i18n::tr("control-center.cast.displays-empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    PanelManager::instance().requestLayout();
    return;
  }

  const float iconSize = Style::controlHeightSm * scale;
  for (const auto& sink : snap.sinks) {
    auto row = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
    auto header = ui::row({
        .align = FlexAlign::Center,
        .wrap = true,
        .gap = Style::spaceSm * scale,
    });
    header->addChild(
        ui::glyph({
            .glyph = "screen-share",
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = sink.name.empty() ? sink.uuid : sink.name,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = sink.state == "streaming" ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    titles->addChild(
        ui::label({
            .text = sinkDetail(sink),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    header->addChild(std::move(titles));
    const bool streaming = sink.state == "streaming" || !sink.unit.empty();
    header->addChild(
        ui::button({
            .glyph = streaming ? "player-stop" : "player-play",
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr(streaming ? "control-center.cast.stop" : "control-center.cast.start-stream"),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = Style::spaceXs * scale,
            .onClick =
                [this, uuid = sink.uuid, unit = sink.unit, streaming]() {
                  if (m_service == nullptr) {
                    return;
                  }
                  if (streaming) {
                    m_service->stopStream(unit);
                  } else {
                    m_service->startStream(uuid);
                  }
                },
        })
    );
    row->addChild(std::move(header));
    m_sinkList->addChild(std::move(row));
  }
  PanelManager::instance().requestLayout();
}
