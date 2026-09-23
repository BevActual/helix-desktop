#include "shell/control_center/tabs/privacy_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "system/location_service.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

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

  struct CaptureLists {
    std::vector<std::string> microphone;
    std::vector<std::string> camera;
    std::vector<std::string> screen;
  };

  [[nodiscard]] CaptureLists captureLists(const PrivacyState& state) {
    CaptureLists lists;
    for (const auto& capture : state.captures) {
      switch (capture.kind) {
      case PrivacyCaptureKind::Camera:
        lists.camera.push_back(capture.appName.empty() ? i18n::tr("control-center.privacy.unknown-app")
                                                         : capture.appName);
        break;
      case PrivacyCaptureKind::Screen:
        lists.screen.push_back(capture.appName.empty() ? i18n::tr("control-center.privacy.unknown-app")
                                                       : capture.appName);
        break;
      case PrivacyCaptureKind::Microphone:
        lists.microphone.push_back(capture.appName.empty() ? i18n::tr("control-center.privacy.unknown-app")
                                                         : capture.appName);
        break;
      }
    }
    return lists;
  }

  [[nodiscard]] std::string captureSignature(const CaptureLists& lists) {
    std::string key;
    auto append = [&key](char kind, const std::vector<std::string>& apps) {
      key.push_back(kind);
      key.push_back(':');
      for (const auto& app : apps) {
        key += app;
        key.push_back('|');
      }
      key.push_back('\n');
    };
    append('m', lists.microphone);
    append('c', lists.camera);
    append('s', lists.screen);
    return key;
  }

  void fillAppList(Flex* list, const std::vector<std::string>& apps, const std::string& emptyText, float scale) {
    if (list == nullptr) {
      return;
    }
    while (!list->children().empty()) {
      list->removeChild(list->children().front().get());
    }
    if (apps.empty()) {
      list->addChild(
          ui::label({
              .text = emptyText,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 2,
          })
      );
      return;
    }
    for (const auto& app : apps) {
      list->addChild(
          ui::label({
              .text = app,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .flexGrow = 1.0F,
          })
      );
    }
  }

} // namespace

PrivacyTab::PrivacyTab(const ControlCenterServices& services)
    : m_audio(services.audio), m_config(services.config), m_location(services.location),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "security", .includedGroups = {"privacy"}}
      )) {}

std::unique_ptr<Flex> PrivacyTab::create() {
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

  auto capturesWell = ui::column({
      .out = &m_capturesWell,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applyCompactCardStyle(section, scale, opacity, false);
      },
  });
  auto header = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  header->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  );
  header->addChild(
      ui::button({
          .out = &m_muteButton,
          .glyph = "microphone-mute",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.privacy.mute-microphone"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick =
              [this]() {
                if (m_audio == nullptr) {
                  return;
                }
                const AudioNode* source = m_audio->defaultSource();
                if (source == nullptr) {
                  return;
                }
                m_audio->setMicMuted(!source->muted);
                PanelManager::instance().refresh();
              },
      })
  );
  capturesWell->addChild(std::move(header));
  capturesWell->addChild(
      ui::label({
          .out = &m_detail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  m_rootLayout->addChild(std::move(capturesWell));

  auto leftover = ui::column({
      .out = &m_capturesLeftover,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  auto addKindSection = [this, scale, &leftover](
                            const std::string& title, Separator** dividerOut, Flex** cardOut, Flex** listOut
                        ) {
    *dividerOut = addWellDivider(*leftover, scale);
    auto card = ui::column({
        .out = cardOut,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .visible = false,
        .participatesInLayout = false,
    });
    addTitle(*card, title, scale);
    card->addChild(
        ui::column({
            .out = listOut,
            .align = FlexAlign::Stretch,
            .configure =
                [scale](Flex& list) {
                  applyGroupedListStyle(list, scale);
                  list.setPadding(Style::spaceSm * scale);
                  list.setGap(Style::spaceXs * scale);
                },
        })
    );
    leftover->addChild(std::move(card));
    if (*dividerOut != nullptr) {
      (*dividerOut)->setVisible(false);
      (*dividerOut)->setParticipatesInLayout(false);
    }
  };
  addKindSection(i18n::tr("control-center.privacy.microphone"), &m_micDivider, &m_micCard, &m_micList);
  addKindSection(i18n::tr("control-center.privacy.camera"), &m_cameraDivider, &m_cameraCard, &m_cameraList);
  addKindSection(i18n::tr("control-center.privacy.screen"), &m_screenDivider, &m_screenCard, &m_screenList);
  m_rootLayout->addChild(std::move(leftover));

  auto sharingCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*sharingCard, i18n::tr("control-center.privacy.this-machine"), scale);
  auto locationRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  locationRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.privacy.auto-locate"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  locationRow->addChild(
      ui::toggle({
          .out = &m_locationToggle,
          .checkedImmediate = m_config != nullptr && m_config->config().location.autoLocate,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool value) {
                if (m_syncing || m_config == nullptr) {
                  return;
                }
                (void)m_config->setOverride({"location", "auto_locate"}, value);
              },
      })
  );
  sharingCard->addChild(std::move(locationRow));
  sharingCard->addChild(
      ui::label({
          .out = &m_locationHint,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  addWellDivider(*sharingCard, scale);
  auto telemetryRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  telemetryRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.privacy.telemetry"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  telemetryRow->addChild(
      ui::toggle({
          .out = &m_telemetryToggle,
          .checkedImmediate = m_config != nullptr && m_config->config().shell.telemetryEnabled,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool value) {
                if (m_syncing || m_config == nullptr) {
                  return;
                }
                (void)m_config->setOverride({"shell", "telemetry_enabled"}, value);
              },
      })
  );
  sharingCard->addChild(std::move(telemetryRow));
  sharingCard->addChild(
      ui::label({
          .out = &m_telemetryHint,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  m_rootLayout->addChild(std::move(sharingCard));

  auto live = ui::column({
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });
  live->addChild(std::move(liveScroll));
  syncUi();
  return m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
}

std::unique_ptr<Flex> PrivacyTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void PrivacyTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_capturesWell = nullptr;
  m_capturesLeftover = nullptr;
  m_status = nullptr;
  m_detail = nullptr;
  m_muteButton = nullptr;
  m_locationToggle = nullptr;
  m_locationHint = nullptr;
  m_telemetryToggle = nullptr;
  m_telemetryHint = nullptr;
  m_micDivider = nullptr;
  m_micCard = nullptr;
  m_micList = nullptr;
  m_cameraDivider = nullptr;
  m_cameraCard = nullptr;
  m_cameraList = nullptr;
  m_screenDivider = nullptr;
  m_screenCard = nullptr;
  m_screenList = nullptr;
  m_liveScroll = nullptr;
  m_captureSignature.clear();
}

void PrivacyTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool PrivacyTab::configureMode() const { return m_settings->configureMode(); }

void PrivacyTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* PrivacyTab::settingsPane() { return m_settings.get(); }

void PrivacyTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_liveScroll == nullptr) {
    return;
  }
  m_liveScroll->setSize(contentWidth, bodyHeight);
  m_liveScroll->layout(renderer);
}

void PrivacyTab::doUpdate(Renderer& /*renderer*/) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  syncUi();
}

void PrivacyTab::syncUi() {
  if (m_status == nullptr) {
    return;
  }

  if (m_audio == nullptr) {
    m_status->setText(i18n::tr("control-center.privacy.unavailable"));
    m_detail->setText(i18n::tr("control-center.privacy.unavailable-hint"));
    syncMute();
    syncSharing();
    rebuildCaptures();
    return;
  }

  const CaptureLists lists = captureLists(m_audio->privacyState());
  const bool active = !lists.microphone.empty() || !lists.camera.empty() || !lists.screen.empty();
  m_status->setText(
      active ? i18n::tr("control-center.privacy.in-use") : i18n::tr("control-center.privacy.idle")
  );
  if (!active) {
    m_detail->setText(i18n::tr("control-center.privacy.idle-hint"));
  } else if (!lists.microphone.empty() && lists.camera.empty() && lists.screen.empty()) {
    m_detail->setText(i18n::tr("control-center.privacy.in-use-microphone"));
  } else if (lists.microphone.empty() && !lists.camera.empty() && lists.screen.empty()) {
    m_detail->setText(i18n::tr("control-center.privacy.in-use-camera"));
  } else if (lists.microphone.empty() && lists.camera.empty() && !lists.screen.empty()) {
    m_detail->setText(i18n::tr("control-center.privacy.in-use-screen"));
  } else {
    m_detail->setText(i18n::tr("control-center.privacy.in-use-hint"));
  }

  syncMute();
  syncSharing();
  rebuildCaptures();
}

void PrivacyTab::rebuildCaptures() {
  const PrivacyState empty;
  const PrivacyState& state = m_audio != nullptr ? m_audio->privacyState() : empty;
  const CaptureLists lists = captureLists(state);
  const std::string signature = captureSignature(lists);
  if (signature == m_captureSignature && m_micList != nullptr && !m_micList->children().empty()) {
    return;
  }
  m_captureSignature = signature;
  const bool anyCaptures =
      !lists.microphone.empty() || !lists.camera.empty() || !lists.screen.empty();
  bool layoutDirty = false;
  layoutDirty |= setShown(m_capturesLeftover, anyCaptures);
  layoutDirty |= setShown(m_micCard, !lists.microphone.empty());
  layoutDirty |= setShown(m_micDivider, false);
  layoutDirty |= setShown(m_cameraCard, !lists.camera.empty());
  layoutDirty |= setShown(m_cameraDivider, !lists.camera.empty() && !lists.microphone.empty());
  layoutDirty |= setShown(m_screenCard, !lists.screen.empty());
  layoutDirty |= setShown(m_screenDivider, !lists.screen.empty() && (!lists.microphone.empty() || !lists.camera.empty()));
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
  const float scale = contentScale();
  if (!lists.microphone.empty()) {
    fillAppList(m_micList, lists.microphone, i18n::tr("control-center.privacy.microphone-idle"), scale);
  }
  if (!lists.camera.empty()) {
    fillAppList(m_cameraList, lists.camera, i18n::tr("control-center.privacy.camera-idle"), scale);
  }
  if (!lists.screen.empty()) {
    fillAppList(m_screenList, lists.screen, i18n::tr("control-center.privacy.screen-idle"), scale);
  }
}

void PrivacyTab::syncMute() {
  if (m_muteButton == nullptr) {
    return;
  }
  const AudioNode* source = m_audio != nullptr ? m_audio->defaultSource() : nullptr;
  const bool muted = source != nullptr && source->muted;
  m_muteButton->setEnabled(source != nullptr);
  m_muteButton->setGlyph(muted ? "microphone" : "microphone-mute");
  m_muteButton->setTooltip(
      i18n::tr(muted ? "control-center.privacy.unmute-microphone" : "control-center.privacy.mute-microphone")
  );
  m_muteButton->setVariant(muted ? ButtonVariant::Destructive : ButtonVariant::Ghost);
}

void PrivacyTab::syncSharing() {
  m_syncing = true;
  const bool autoLocate = m_config != nullptr && m_config->config().location.autoLocate;
  const bool telemetry = m_config != nullptr && m_config->config().shell.telemetryEnabled;
  if (m_locationToggle != nullptr && m_locationToggle->checked() != autoLocate) {
    m_locationToggle->setCheckedImmediate(autoLocate);
  }
  if (m_telemetryToggle != nullptr && m_telemetryToggle->checked() != telemetry) {
    m_telemetryToggle->setCheckedImmediate(telemetry);
  }
  m_syncing = false;

  if (m_locationHint != nullptr) {
    if (!autoLocate) {
      m_locationHint->setText(i18n::tr("control-center.privacy.auto-locate-off"));
    } else if (m_location != nullptr && m_location->resolving()) {
      m_locationHint->setText(i18n::tr("control-center.privacy.auto-locate-looking"));
    } else if (m_location != nullptr && m_location->hasResolvedLocation()) {
      const auto resolved = m_location->resolvedLocation();
      if (resolved.has_value() && !resolved->name.empty()) {
        m_locationHint->setText(i18n::tr("control-center.privacy.auto-locate-using", "name", resolved->name));
      } else {
        m_locationHint->setText(i18n::tr("control-center.privacy.auto-locate-on"));
      }
    } else if (m_location != nullptr && !m_location->error().empty()) {
      m_locationHint->setText(m_location->error());
    } else {
      m_locationHint->setText(i18n::tr("control-center.privacy.auto-locate-on"));
    }
  }
  if (m_telemetryHint != nullptr) {
    m_telemetryHint->setText(
        i18n::tr(telemetry ? "control-center.privacy.telemetry-on" : "control-center.privacy.telemetry-off")
    );
  }
}
