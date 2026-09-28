#include "shell/control_center/tabs/power_tab.h"
#include "core/input/keybind_matcher.h"

#include "config/config_service.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "idle/idle_inhibitor.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

  ColorRole healthRole(double health) {
    if (health >= 80.0) {
      return ColorRole::Primary;
    }
    if (health >= 50.0) {
      return ColorRole::Tertiary;
    }
    return ColorRole::Error;
  }

  std::string deviceDisplayName(const UPowerDeviceInfo& info) {
    if (!info.model.empty()) {
      return info.model;
    }
    if (!info.vendor.empty()) {
      return info.vendor;
    }
    return i18n::tr("control-center.power.unknown-device");
  }

  std::string thresholdBehavior(const std::optional<std::uint32_t>& start, const std::optional<std::uint32_t>& end) {
    if (start.has_value() && end.has_value()) {
      return i18n::tr(
          "control-center.power.charging.starts-stops", "start", std::to_string(*start), "end", std::to_string(*end)
      );
    }
    if (start.has_value()) {
      return i18n::tr("control-center.power.charging.starts", "start", std::to_string(*start));
    }
    if (end.has_value()) {
      return i18n::tr("control-center.power.charging.stops", "end", std::to_string(*end));
    }
    return {};
  }

  [[nodiscard]] std::string profileTooltip(std::string_view profile) {
    if (profile == "power-saver") {
      return i18n::tr("control-center.power.power-saver-tooltip");
    }
    if (profile == "balanced") {
      return i18n::tr("control-center.power.balanced-tooltip");
    }
    if (profile == "performance") {
      return i18n::tr("control-center.power.performance-tooltip");
    }
    return {};
  }

  bool sameThresholds(const UPowerChargeLimitState& state) {
    return state.configuredStart == state.effectiveStart && state.configuredEnd == state.effectiveEnd;
  }

} // namespace

PowerTab::ChargeLimitMode PowerTab::classifyChargeLimit(const UPowerChargeLimitState& state) noexcept {
  if (!state.requestPending && state.enabledAvailable && !state.enabled && state.hasRestrictiveThreshold()) {
    return ChargeLimitMode::ExternallyManaged;
  }

  const bool firmware = state.supportedSettings.has_value() && ((*state.supportedSettings & 4U) != 0U);
  const bool hasNumericThreshold = state.configuredStart.has_value()
      || state.configuredEnd.has_value()
      || state.effectiveStart.has_value()
      || state.effectiveEnd.has_value();
  const bool controllable = state.supported && state.methodAvailable && state.enabledAvailable;
  if (controllable) {
    if (!state.enabled) {
      return ChargeLimitMode::UPowerDisabled;
    }
    return firmware && !hasNumericThreshold ? ChargeLimitMode::FirmwareManaged : ChargeLimitMode::UPowerActive;
  }

  if (state.supported && firmware && !hasNumericThreshold) {
    return ChargeLimitMode::FirmwareManaged;
  }
  if (state.supported || hasNumericThreshold) {
    return ChargeLimitMode::ReadOnly;
  }
  return ChargeLimitMode::Unsupported;
}

PowerTab::ChargeLimitControlState PowerTab::chargeLimitControlState(const UPowerChargeLimitState& state) noexcept {
  const ChargeLimitMode mode = classifyChargeLimit(state);
  ChargeLimitControlState control;
  const bool controllable = state.supported && state.methodAvailable && state.enabledAvailable;
  control.visible = mode == ChargeLimitMode::ExternallyManaged
      || (controllable
          && (mode == ChargeLimitMode::UPowerActive
              || mode == ChargeLimitMode::UPowerDisabled
              || mode == ChargeLimitMode::FirmwareManaged));
  control.checked = mode == ChargeLimitMode::ExternallyManaged ? true : state.requestedEnabled.value_or(state.enabled);
  control.enabled = control.visible && mode != ChargeLimitMode::ExternallyManaged && !state.requestPending;
  return control;
}

bool PowerTab::shouldShowChargeLimit(const UPowerChargeLimitState& state) noexcept {
  const bool hasNumericThreshold = state.configuredStart.has_value()
      || state.configuredEnd.has_value()
      || state.effectiveStart.has_value()
      || state.effectiveEnd.has_value();
  const bool firmwareManaged = state.supportedSettings.has_value() && ((*state.supportedSettings & 4U) != 0U);
  return hasNumericThreshold || firmwareManaged || chargeLimitControlState(state).visible;
}

PowerTab::PowerTab(UPowerService* upower, PowerProfilesService* powerProfiles, const ControlCenterServices* services)
    : m_upower(upower), m_powerProfiles(powerProfiles) {
  if (services != nullptr) {
    m_config = services->config;
    m_idleInhibitor = services->idleInhibitor;
    m_session = services->session;
    m_settings =
        std::make_unique<SettingsPane>(*services, SettingsPaneOptions{.section = "power", .inlineSettings = true});
    m_sessionPane = std::make_unique<SettingsPane>(
        *services,
        SettingsPaneOptions{.section = "power", .includedGroups = {"session-panel"}, .inlineSettings = true}
    );
    m_idlePane = std::make_unique<SettingsPane>(
        *services, SettingsPaneOptions{.section = "power", .includedGroups = {"idle"}, .inlineSettings = true}
    );
  }
}

std::unique_ptr<Flex> PowerTab::create() {
  const float scale = contentScale();

  auto outer = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });
  auto subHeader = makeSubPaneHeader(scale, &m_subTitle, [this]() { openPage(Page::Root); });
  m_subHeader = subHeader.get();
  outer->addChild(std::move(subHeader));

  auto tab = ui::column({
      .out = &m_rootPage,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  buildSessionCard(*tab, scale);

  if (m_upower != nullptr || m_powerProfiles != nullptr || m_idleInhibitor != nullptr) {
    auto well = ui::column({
        .out = &m_batteryWell,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .visible = false,
        .participatesInLayout = false,
        .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
          applyCompactCardStyle(card, scale, opacity, false);
        },
    });
    buildStatusCard(*well, scale);
    buildProfilesCard(*well, scale);
    if (m_idleInhibitor != nullptr) {
      auto caffeine = ui::column({
          .out = &m_caffeineRow,
          .align = FlexAlign::Stretch,
          .visible = false,
          .participatesInLayout = false,
      });
      m_caffeineToggle = addToggleRow(
          *caffeine, scale, i18n::tr("control-center.shortcuts.caffeine"),
          [this](bool checked) {
            if (m_syncingCaffeine || m_idleInhibitor == nullptr) {
              return;
            }
            m_idleInhibitor->setEnabled(checked);
          }
      );
      well->addChild(std::move(caffeine));
    }
    tab->addChild(std::move(well));
  }

  if (m_config != nullptr || m_upower != nullptr) {
    auto leftover = ui::scrollView({
        .out = &m_leftoverScroll,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .flexGrow = 1.0F,
        .visible = false,
        .participatesInLayout = false,
        .configure =
            [](ScrollView& scrollView) {
              scrollView.clearFill();
              scrollView.clearBorder();
            },
    });
    auto* content = leftover->content();
    content->setDirection(FlexDirection::Vertical);
    content->setAlign(FlexAlign::Stretch);
    content->setGap(Style::spaceMd * scale);
    buildLidCloseCard(*content, scale);
    if (m_upower != nullptr) {
      buildChargingCard(*content, scale);
      buildHealthCard(*content, scale);
      buildPeripheralsCard(*content, scale);
    }
    tab->addChild(std::move(leftover));
  }

  // Navigation rows into the settings sub-panes, after the live cards.
  if (m_sessionPane != nullptr && m_idlePane != nullptr) {
    auto nav = ui::column({
        .align = FlexAlign::Stretch,
        .configure = [scale](Flex& list) { applyGroupedListStyle(list, scale); },
    });
    nav->addChild(makeNavRow(scale, "power", i18n::tr("control-center.power.pages.session"), [this]() {
      openPage(Page::Session);
    }));
    nav->addChild(ui::separator());
    nav->addChild(makeNavRow(scale, "moon", i18n::tr("control-center.power.pages.idle"), [this]() {
      openPage(Page::Idle);
    }));
    tab->addChild(std::move(nav));
  }
  outer->addChild(std::move(tab));

  if (m_sessionPane != nullptr && m_idlePane != nullptr) {
    auto subScroll = ui::scrollView({
        .out = &m_subScroll,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .flexGrow = 1.0F,
        .configure =
            [](ScrollView& view) {
              view.clearFill();
              view.clearBorder();
            },
    });
    Flex* subContent = subScroll->content();
    subContent->setDirection(FlexDirection::Vertical);
    subContent->setAlign(FlexAlign::Stretch);
    auto sessionPage = m_sessionPane->createInline(scale, panelCardOpacity());
    m_sessionPage = sessionPage.get();
    subContent->addChild(std::move(sessionPage));
    auto idlePage = m_idlePane->createInline(scale, panelCardOpacity());
    m_idlePage = idlePage.get();
    subContent->addChild(std::move(idlePage));
    outer->addChild(std::move(subScroll));
  }

  syncSessionCard();
  syncLidCloseToggle();
  syncPowerProfiles();
  syncBatteryWell();
  syncPage();

  if (m_settings != nullptr) {
    return m_settings->wrapLive(std::move(outer), contentScale(), panelCardOpacity());
  }
  return outer;
}

std::unique_ptr<Flex> PowerTab::createHeaderActions() {
  if (m_settings == nullptr) {
    return nullptr;
  }
  return m_settings->embedHeader(nullptr, contentScale());
}

void PowerTab::setConfigureMode(bool show) {
  if (m_settings != nullptr) {
    m_settings->setConfigureMode(show);
  }
}

bool PowerTab::configureMode() const { return m_settings != nullptr && m_settings->configureMode(); }

void PowerTab::setSettingsSearchQuery(std::string_view query) {
  if (m_settings != nullptr) {
    m_settings->setSearchQuery(std::string(query));
  }
}

SettingsPane* PowerTab::settingsPane() { return m_settings.get(); }

void PowerTab::buildSessionCard(Flex& root, float scale) {
  if (m_session == nullptr) {
    return;
  }

  // Session actions are a module well of functional plates. The battery card below is
  // the tab's one glass compact card, so this must not read as a second glass layer.
  auto card = ui::column({
      .out = &m_sessionCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyModuleWellStyle(section, scale); },
  });

  auto actions = ui::row({
      .out = &m_sessionActionsRow,
      .align = FlexAlign::Stretch,
      .wrap = true,
      .gap = Style::spaceSm * scale,
  });
  const float actionHeight = Style::controlHeight * scale;
  const float actionPad = Style::spaceMd * scale;
  actions->addChild(
      ui::button({
          .out = &m_lockButton,
          .text = i18n::tr("session.actions.lock"),
          .glyph = session_action::defaultGlyph("lock"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = actionHeight,
          .paddingH = actionPad,
          .flexGrow = 1.0F,
          .onClick =
              [this]() {
                PanelManager::instance().close();
                if (m_session != nullptr) {
                  (void)m_session->lock();
                }
              },
      })
  );
  actions->addChild(
      ui::button({
          .out = &m_suspendButton,
          .text = i18n::tr("session.actions.suspend"),
          .glyph = session_action::defaultGlyph("suspend"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = actionHeight,
          .paddingH = actionPad,
          .flexGrow = 1.0F,
          .onClick = [this]() { runSessionAction("suspend"); },
      })
  );
  actions->addChild(
      ui::button({
          .out = &m_logoutButton,
          .text = i18n::tr("session.actions.logout"),
          .glyph = session_action::defaultGlyph("logout"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = actionHeight,
          .paddingH = actionPad,
          .flexGrow = 1.0F,
          .onClick = [this]() { runSessionAction("logout"); },
      })
  );
  actions->addChild(
      ui::button({
          .out = &m_rebootButton,
          .text = i18n::tr("session.actions.reboot"),
          .glyph = session_action::defaultGlyph("reboot"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Destructive,
          .minHeight = actionHeight,
          .paddingH = actionPad,
          .flexGrow = 1.0F,
          .onClick = [this]() { runSessionAction("reboot"); },
      })
  );
  actions->addChild(
      ui::button({
          .out = &m_shutdownButton,
          .text = i18n::tr("session.actions.shutdown"),
          .glyph = session_action::defaultGlyph("shutdown"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Destructive,
          .minHeight = actionHeight,
          .paddingH = actionPad,
          .flexGrow = 1.0F,
          .onClick = [this]() { runSessionAction("shutdown"); },
      })
  );
  card->addChild(std::move(actions));
  root.addChild(std::move(card));
  syncSessionCard();
}

void PowerTab::syncSessionCard() {
  const bool lockEnabled = m_config != nullptr && m_config->isLockScreenEnabled();
  bool anySession = false;
  auto showAction = [&](Button* button, bool show) {
    if (button == nullptr) {
      return;
    }
    (void)setShown(button, show);
    anySession = anySession || show;
  };
  showAction(m_lockButton, m_session != nullptr && lockEnabled);
  showAction(m_suspendButton, m_session != nullptr);

  const auto actionEnabled = [this](std::string_view action) {
    if (m_session == nullptr || m_config == nullptr) {
      return false;
    }
    for (const auto& cfg : m_config->config().shell.session.actions) {
      if (cfg.action == action) {
        return cfg.enabled;
      }
    }
    return true;
  };
  showAction(m_logoutButton, actionEnabled("logout"));
  showAction(m_rebootButton, actionEnabled("reboot"));
  showAction(m_shutdownButton, actionEnabled("shutdown"));
  (void)setShown(m_sessionActionsRow, anySession);
  (void)setShown(m_sessionCard, anySession);

  const bool caffeineAvailable = m_idleInhibitor != nullptr && m_idleInhibitor->available();
  if (m_caffeineRow != nullptr) {
    (void)setShown(m_caffeineRow, caffeineAvailable);
  }
  if (m_caffeineToggle != nullptr && caffeineAvailable) {
    m_syncingCaffeine = true;
    m_caffeineToggle->setCheckedImmediate(m_idleInhibitor->enabled());
    m_syncingCaffeine = false;
  }
}

void PowerTab::runSessionAction(std::string_view action) {
  PanelManager::instance().close();
  if (m_session == nullptr) {
    return;
  }
  if (m_config != nullptr) {
    for (const auto& cfg : m_config->config().shell.session.actions) {
      if (cfg.enabled && cfg.action == action) {
        m_session->invoke(cfg);
        return;
      }
    }
  }
  SessionPanelActionConfig cfg;
  cfg.action = std::string(action);
  m_session->invoke(cfg);
}

void PowerTab::buildLidCloseCard(Flex& root, float scale) {
  if (m_config == nullptr) {
    return;
  }

  auto card = ui::column({
      .out = &m_lidCloseCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  card->addChild(makeCardHeaderRow(i18n::tr("control-center.power.lid-close.title"), scale));
  addCaption(*card, i18n::tr("control-center.power.lid-close.description"), scale, 3);
  m_lidCloseToggle = addToggleRow(
      *card, scale, i18n::tr("settings.schema.lockscreen.lid-close-keep-external-displays.label"),
      [this](bool checked) {
        if (m_syncingLidClose || m_config == nullptr) {
          return;
        }
        (void)m_config->setOverride({"lockscreen", "lid_close_keep_external_displays"}, checked);
      }
  );
  root.addChild(std::move(card));
}

void PowerTab::syncLidCloseToggle() {
  if (m_lidCloseToggle == nullptr || m_config == nullptr) {
    return;
  }
  m_syncingLidClose = true;
  m_lidCloseToggle->setCheckedImmediate(m_config->shouldKeepSessionOnLidClose());
  m_syncingLidClose = false;
}

void PowerTab::buildChargingCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .out = &m_chargingCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  card->addChild(makeCardHeaderRow(i18n::tr("control-center.power.charging.title"), scale));
  card->addChild(
      ui::column({
          .out = &m_chargingList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
      })
  );
  root.addChild(std::move(card));
}

void PowerTab::buildStatusCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .out = &m_statusCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
  });

  auto topRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .out = &m_statusGlyph,
          .glyph = batteryGlyphName(0.0, BatteryState::Unknown),
          .glyphSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .out = &m_percentLabel,
          .text = "--",
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .out = &m_stateLabel,
          .text = "",
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  card->addChild(std::move(topRow));

  card->addChild(
      ui::progressBar({
          .out = &m_levelBar,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::sliderTrackHeight * scale * 0.5F,
          .progress = 0.0F,
          .height = Style::sliderTrackHeight * scale,
      })
  );

  auto timeRow = ui::row(
      {.out = &m_timeRow,
       .align = FlexAlign::Center,
       .gap = Style::spaceXs * scale,
       .visible = false,
       .participatesInLayout = false},
      ui::glyph({
          .glyph = "clock",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::label({
          .out = &m_timeLabel,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  card->addChild(std::move(timeRow));

  auto rateRow = ui::row(
      {.out = &m_rateRow,
       .align = FlexAlign::Center,
       .gap = Style::spaceXs * scale,
       .visible = false,
       .participatesInLayout = false},
      ui::glyph({
          .glyph = "bolt",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::label({
          .out = &m_rateLabel,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  card->addChild(std::move(rateRow));

  root.addChild(std::move(card));
}

void PowerTab::buildProfilesCard(Flex& root, float scale) {
  if (m_powerProfiles == nullptr) {
    return;
  }

  auto card = ui::column({
      .out = &m_profilesCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
  });

  auto profileRow = makeControlRow(i18n::tr("control-center.power.power-profile"), scale, nullptr, true);
  profileRow->addChild(
      ui::row({
          .out = &m_profilesHost,
          .align = FlexAlign::Center,
          .wrap = true,
          .flexGrow = 1.0F,
      })
  );
  card->addChild(std::move(profileRow));

  auto inhibitedRow = ui::row(
      {.out = &m_inhibitedRow,
       .align = FlexAlign::Center,
       .gap = Style::spaceXs * scale,
       .visible = false,
       .participatesInLayout = false},
      ui::glyph({
          .glyph = "alert-triangle",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
      }),
      ui::label({
          .out = &m_inhibitedLabel,
          .text = i18n::tr("control-center.power.performance-inhibited"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0F,
      })
  );
  card->addChild(std::move(inhibitedRow));

  root.addChild(std::move(card));
}

void PowerTab::buildHealthCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .out = &m_healthCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });

  auto header = makeCardHeaderRow(i18n::tr("control-center.power.health"), scale);
  header->addChild(
      ui::label({
          .out = &m_healthLabel,
          .text = "--",
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  card->addChild(std::move(header));

  card->addChild(
      ui::label({
          .text = i18n::tr("control-center.power.design-capacity"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  card->addChild(
      ui::progressBar({
          .out = &m_healthBar,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::sliderTrackHeight * scale * 0.5F,
          .progress = 0.0F,
          .height = Style::sliderTrackHeight * scale,
      })
  );

  root.addChild(std::move(card));
}

void PowerTab::buildPeripheralsCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .out = &m_peripheralsCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });

  card->addChild(makeCardHeaderRow(i18n::tr("control-center.power.peripherals"), scale));

  auto list = ui::column({.out = &m_peripheralsList, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
  card->addChild(std::move(list));

  root.addChild(std::move(card));
}

void PowerTab::syncPage() {
  const bool root = m_page == Page::Root;
  bool changed = setShown(m_rootPage, root);
  changed |= setShown(m_subHeader, !root);
  changed |= setShown(m_subScroll, !root);
  changed |= setShown(m_sessionPage, m_page == Page::Session);
  changed |= setShown(m_idlePage, m_page == Page::Idle);
  if (m_subTitle != nullptr && !root) {
    m_subTitle->setText(
        i18n::tr(m_page == Page::Session ? "control-center.power.pages.session" : "control-center.power.pages.idle")
    );
  }
  if (changed) {
    if (auto* panels = PanelManager::current(); panels != nullptr) {
      panels->requestLayout();
    }
  }
}

void PowerTab::openPage(Page page) {
  if (page == m_page) {
    return;
  }
  m_page = page;
  if (m_subScroll != nullptr) {
    m_subScroll->setScrollOffset(0.0F);
  }
  syncPage();
}

void PowerTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (becameActive && m_page != Page::Root) {
    m_page = Page::Root;
    syncPage();
  }
}

bool PowerTab::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  // Cancel (Escape by default) backs out of a sub-pane before it would close the panel.
  if (!pressed || preedit || configureMode() || m_page == Page::Root
      || !KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    return false;
  }
  openPage(Page::Root);
  return true;
}

void PowerTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  if (m_sessionPane != nullptr) {
    m_sessionPane->onClose();
  }
  if (m_idlePane != nullptr) {
    m_idlePane->onClose();
  }
  m_rootPage = nullptr;
  m_subHeader = nullptr;
  m_subTitle = nullptr;
  m_subScroll = nullptr;
  m_sessionPage = nullptr;
  m_idlePage = nullptr;
  m_root = nullptr;
  m_sessionCard = nullptr;
  m_lockButton = nullptr;
  m_suspendButton = nullptr;
  m_logoutButton = nullptr;
  m_rebootButton = nullptr;
  m_shutdownButton = nullptr;
  m_sessionActionsRow = nullptr;
  m_caffeineToggle = nullptr;
  m_caffeineRow = nullptr;
  m_lidCloseCard = nullptr;
  m_lidCloseToggle = nullptr;
  m_batteryWell = nullptr;
  m_leftoverScroll = nullptr;
  m_statusCard = nullptr;
  m_statusGlyph = nullptr;
  m_percentLabel = nullptr;
  m_stateLabel = nullptr;
  m_levelBar = nullptr;
  m_timeRow = nullptr;
  m_timeLabel = nullptr;
  m_rateRow = nullptr;
  m_rateLabel = nullptr;
  m_chargingCard = nullptr;
  m_chargingList = nullptr;
  m_chargeLimitRows.clear();
  m_lastChargeLimitKey.clear();
  m_profilesCard = nullptr;
  m_profilesHost = nullptr;
  m_profiles = nullptr;
  m_profileOrder.clear();
  m_inhibitedRow = nullptr;
  m_inhibitedLabel = nullptr;
  m_healthCard = nullptr;
  m_healthLabel = nullptr;
  m_healthBar = nullptr;
  m_peripheralsCard = nullptr;
  m_peripheralsList = nullptr;
  m_peripheralRows.clear();
  m_lastPeripheralKey.clear();
}

void PowerTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_root == nullptr) {
    return;
  }
  rebuildPeripherals();
  rebuildChargeLimits();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void PowerTab::doUpdate(Renderer& /*renderer*/) {
  syncSessionCard();
  syncLidCloseToggle();
  syncBatteryStatus();
  rebuildChargeLimits();
  syncPowerProfiles();
  syncBatteryHealth();
  rebuildPeripherals();
  syncBatteryWell();
}

void PowerTab::rebuildChargeLimits() {
  if (m_chargingCard == nullptr || m_chargingList == nullptr || m_upower == nullptr) {
    return;
  }

  std::vector<UPowerDeviceInfo> batteries;
  for (auto& device : m_upower->batteryDevices()) {
    if (device.isLaptopBattery() && device.isPresent && shouldShowChargeLimit(device.chargeLimit)) {
      batteries.push_back(std::move(device));
    }
  }

  std::string key = std::to_string(batteries.size()) + ':';
  for (const auto& battery : batteries) {
    key += battery.path;
    key += ';';
  }

  if (key != m_lastChargeLimitKey) {
    m_lastChargeLimitKey = key;
    for (auto& entry : m_chargeLimitRows) {
      if (entry.row != nullptr) {
        m_chargingList->removeChild(entry.row);
      }
    }
    m_chargeLimitRows.clear();

    const float scale = contentScale();
    const bool showNames = batteries.size() > 1;
    for (const auto& battery : batteries) {
      ChargeLimitRow entry;
      auto row = ui::column({
          .out = &entry.row,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      });
      row->addChild(
          ui::label({
              .out = &entry.nameLabel,
              .text = deviceDisplayName(battery),
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .visible = showNames,
          })
      );
      row->addChild(
          ui::label({
              .out = &entry.behaviorLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          })
      );
      row->addChild(
          ui::label({
              .out = &entry.configuredLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      );
      row->addChild(
          ui::row(
              {.out = &entry.controlRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale, .visible = false},
              ui::label({
                  .out = &entry.controlLabel,
                  .text = i18n::tr("control-center.power.charging.use-thresholds"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .flexGrow = 1.0F,
              }),
              ui::toggle({
                  .out = &entry.toggle,
                  .checkedImmediate = battery.chargeLimit.enabled,
                  .toggleSize = ToggleSize::Small,
                  .scale = scale,
                  .onChange = [this, path = battery.path](bool checked) {
                    if (m_upower != nullptr) {
                      (void)m_upower->enableChargeThreshold(path, checked);
                    }
                  },
              })
          )
      );
      row->addChild(
          ui::label({
              .out = &entry.managementLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      );
      row->addChild(
          ui::label({
              .out = &entry.errorLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Error),
              .visible = false,
          })
      );
      m_chargingList->addChild(std::move(row));
      m_chargeLimitRows.push_back(entry);
    }
  }

  (void)setShown(m_chargingCard, !batteries.empty());
  for (std::size_t i = 0; i < batteries.size() && i < m_chargeLimitRows.size(); ++i) {
    const auto& state = batteries[i].chargeLimit;
    auto& row = m_chargeLimitRows[i];
    const ChargeLimitMode mode = classifyChargeLimit(state);
    const bool permitsFullCharge = (state.enabledAvailable && !state.enabled && !state.hasRestrictiveThreshold())
        || (state.effectiveEnd == 100U && (!state.effectiveStart.has_value() || state.effectiveStart == 0U));
    if (row.nameLabel != nullptr) {
      row.nameLabel->setText(deviceDisplayName(batteries[i]));
    }
    const ChargeLimitControlState control = chargeLimitControlState(state);
    const std::string effective = thresholdBehavior(state.effectiveStart, state.effectiveEnd);
    const std::string configured = thresholdBehavior(state.configuredStart, state.configuredEnd);
    const bool presentConfiguredAsPrimary =
        control.visible && mode != ChargeLimitMode::ExternallyManaged && control.checked && !configured.empty();
    std::string behavior = control.visible && mode != ChargeLimitMode::ExternallyManaged && !control.checked
        ? i18n::tr("control-center.power.charging.full-charge")
        : (presentConfiguredAsPrimary
               ? configured
               : (permitsFullCharge ? i18n::tr("control-center.power.charging.full-charge") : effective));
    if (behavior.empty()) {
      if (mode == ChargeLimitMode::FirmwareManaged) {
        behavior = i18n::tr("control-center.power.charging.firmware-managed");
      } else {
        behavior = i18n::tr("control-center.power.charging.unreadable");
      }
    }
    row.behaviorLabel->setText(behavior);
    row.behaviorLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));

    const bool showConfigured = (mode == ChargeLimitMode::ExternallyManaged || !control.visible)
        && !configured.empty()
        && !sameThresholds(state);
    row.configuredLabel->setVisible(showConfigured);
    row.configuredLabel->setParticipatesInLayout(showConfigured);
    if (showConfigured) {
      row.configuredLabel->setText(i18n::tr("control-center.power.charging.configured", "limits", configured));
    }

    std::string management;
    if (mode == ChargeLimitMode::ExternallyManaged) {
      management = i18n::tr("control-center.power.charging.externally-managed");
    } else if (mode == ChargeLimitMode::Unsupported) {
      management = i18n::tr("control-center.power.charging.unsupported");
    } else if (mode == ChargeLimitMode::ReadOnly || (mode == ChargeLimitMode::FirmwareManaged && !control.visible)) {
      management = i18n::tr("control-center.power.charging.display-only");
    }
    row.managementLabel->setVisible(!management.empty());
    row.managementLabel->setParticipatesInLayout(!management.empty());
    if (!management.empty()) {
      row.managementLabel->setText(management);
      row.managementLabel->setColor(
          colorSpecFromRole(ColorRole::OnSurfaceVariant, mode == ChargeLimitMode::ExternallyManaged ? 0.55F : 1.0F)
      );
    }

    row.controlRow->setVisible(control.visible);
    row.controlRow->setParticipatesInLayout(control.visible);
    if (control.visible) {
      row.controlLabel->setColor(
          mode == ChargeLimitMode::ExternallyManaged ? colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55F)
                                                     : colorSpecFromRole(ColorRole::OnSurface)
      );
      row.toggle->setCheckedImmediate(control.checked);
      row.toggle->setEnabled(control.enabled);
    }

    const bool hasError = state.operationError != ChargeLimitOperationError::None;
    row.errorLabel->setVisible(hasError);
    row.errorLabel->setParticipatesInLayout(hasError);
    if (hasError) {
      row.errorLabel->setText(
          i18n::tr(
              state.operationError == ChargeLimitOperationError::PermissionDenied
                  ? "control-center.power.charging.error-denied"
                  : "control-center.power.charging.error-failed"
          )
      );
    }
  }
}

void PowerTab::syncBatteryStatus() {
  if (m_statusCard == nullptr || m_upower == nullptr) {
    return;
  }

  const UPowerState& state = m_upower->state();
  (void)setShown(m_statusCard, state.isPresent);
  if (!state.isPresent) {
    return;
  }

  if (m_statusGlyph != nullptr) {
    m_statusGlyph->setGlyph(batteryGlyphName(state.percentage, state.state));
  }
  if (m_percentLabel != nullptr) {
    m_percentLabel->setText(std::format("{:.0F}%", state.percentage));
  }
  if (m_stateLabel != nullptr) {
    m_stateLabel->setText(batteryStateLabel(state.state));
  }
  if (m_levelBar != nullptr) {
    m_levelBar->setProgress(static_cast<float>(std::clamp(state.percentage / 100.0, 0.0, 1.0)));
    const bool low = state.state == BatteryState::Discharging && state.percentage <= 20.0;
    m_levelBar->setFill(colorSpecFromRole(low ? ColorRole::Error : ColorRole::Primary));
  }

  const bool charging = state.state == BatteryState::Charging || state.state == BatteryState::PendingCharge;
  const std::int64_t seconds = charging ? state.timeToFull : state.timeToEmpty;
  if (m_timeRow != nullptr) {
    const bool show = seconds > 0;
    (void)setShown(m_timeRow, show);
    if (show && m_timeLabel != nullptr) {
      const std::string duration = formatDuration(std::chrono::seconds{seconds});
      m_timeLabel->setText(
          i18n::tr(
              charging ? "control-center.power.time-to-full" : "control-center.power.time-to-empty", "time", duration
          )
      );
    }
  }

  if (m_rateRow != nullptr) {
    const bool show = state.energyRate > 0.0;
    (void)setShown(m_rateRow, show);
    if (show && m_rateLabel != nullptr) {
      m_rateLabel->setText(std::format("{:.1F} W", state.energyRate));
    }
  }
}

void PowerTab::syncPowerProfiles() {
  if (m_profilesCard == nullptr || m_profilesHost == nullptr || m_powerProfiles == nullptr) {
    return;
  }

  std::vector<std::string> order;
  const auto& available = m_powerProfiles->profiles();
  for (const auto& candidate : powerProfileOrder()) {
    if (std::ranges::find(available, candidate) != available.end()) {
      order.emplace_back(candidate);
    }
  }
  if (order.empty()) {
    (void)setShown(m_profilesCard, false);
    return;
  }

  if (order != m_profileOrder || m_profiles == nullptr) {
    m_profileOrder = std::move(order);
    if (m_profiles != nullptr) {
      (void)m_profilesHost->removeChild(m_profiles);
      m_profiles = nullptr;
    }
    const float scale = contentScale();
    std::vector<ui::SegmentedOption> options;
    options.reserve(m_profileOrder.size());
    for (const auto& profile : m_profileOrder) {
      options.push_back(
          {.label = profileLabel(profile),
           .glyph = std::string(profileGlyphName(profile)),
           .tooltip = profileTooltip(profile)}
      );
    }
    m_profilesHost->addChild(
        ui::segmented({
            .out = &m_profiles,
            .options = std::move(options),
            .fontSize = Style::fontSizeCaption,
            .scale = scale,
            .compact = true,
            .surfaceOpacity = panelCardOpacity(),
            .surfaceRole = ColorRole::Surface,
            .equalSegmentWidths = true,
            .flexGrow = 1.0F,
            .onChange =
                [this](std::size_t index) {
                  if (m_syncingProfiles || m_powerProfiles == nullptr || index >= m_profileOrder.size()) {
                    return;
                  }
                  (void)m_powerProfiles->setActiveProfile(m_profileOrder[index]);
                },
        })
    );
  }

  (void)setShown(m_profilesCard, true);

  const auto& active = m_powerProfiles->activeProfile();
  const auto it = std::ranges::find(m_profileOrder, active);
  if (m_profiles != nullptr && it != m_profileOrder.end()) {
    const auto index = static_cast<std::size_t>(std::distance(m_profileOrder.begin(), it));
    if (index != m_profiles->selectedIndex()) {
      m_syncingProfiles = true;
      m_profiles->setSelectedIndex(index);
      m_syncingProfiles = false;
    }
  }

  if (m_inhibitedRow != nullptr) {
    (void)setShown(m_inhibitedRow, !m_powerProfiles->state().performanceInhibited.empty());
  }
}

void PowerTab::syncBatteryHealth() {
  if (m_healthCard == nullptr || m_upower == nullptr) {
    return;
  }

  const UPowerDeviceInfo* battery = m_upower->defaultSystemBattery();
  const std::optional<double> health = battery != nullptr ? battery->healthPercent() : std::nullopt;
  (void)setShown(m_healthCard, health.has_value());
  if (!health) {
    return;
  }

  if (m_healthLabel != nullptr) {
    m_healthLabel->setText(std::format("{:.0F}%", *health));
  }
  if (m_healthBar != nullptr) {
    m_healthBar->setProgress(static_cast<float>(*health / 100.0));
    m_healthBar->setFill(colorSpecFromRole(healthRole(*health)));
  }
}

void PowerTab::rebuildPeripherals() {
  if (m_peripheralsCard == nullptr || m_peripheralsList == nullptr || m_upower == nullptr) {
    return;
  }

  std::vector<UPowerDeviceInfo> peripherals;
  for (auto& device : m_upower->batteryDevices()) {
    if (!device.isLaptopBattery() && device.isPresent) {
      peripherals.push_back(std::move(device));
    }
  }

  std::string key;
  for (const auto& device : peripherals) {
    key += device.path;
    key += ';';
  }

  const bool structuralChange = key != m_lastPeripheralKey;
  if (structuralChange) {
    m_lastPeripheralKey = key;

    for (auto& entry : m_peripheralRows) {
      if (entry.row != nullptr) {
        m_peripheralsList->removeChild(entry.row);
      }
    }
    m_peripheralRows.clear();

    const float scale = contentScale();
    for (const auto& device : peripherals) {
      PeripheralRow entry;
      entry.path = device.path;
      auto row = ui::column({
          .out = &entry.row,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      });
      row->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
              ui::glyph({
                  .glyph = batteryDeviceGlyphName(device.type),
                  .glyphSize = Style::fontSizeBody * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              }),
              ui::label({
                  .out = &entry.nameLabel,
                  .text = deviceDisplayName(device),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 1,
                  .ellipsize = TextEllipsize::End,
                  .flexGrow = 1.0F,
              }),
              ui::label({
                  .out = &entry.pctLabel,
                  .text = "",
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .minWidth = Style::controlHeightSm * scale,
              })
          )
      );
      row->addChild(
          ui::progressBar({
              .out = &entry.bar,
              .fill = colorSpecFromRole(ColorRole::Primary),
              .track = colorSpecFromRole(ColorRole::Surface),
              .radius = Style::sliderTrackHeight * scale * 0.5F,
              .progress = 0.0F,
              .height = Style::sliderTrackHeight * scale,
          })
      );
      m_peripheralsList->addChild(std::move(row));
      m_peripheralRows.push_back(std::move(entry));
    }
  }

  (void)setShown(m_peripheralsCard, !peripherals.empty());

  for (std::size_t i = 0; i < m_peripheralRows.size() && i < peripherals.size(); ++i) {
    if (m_peripheralRows[i].pctLabel != nullptr) {
      m_peripheralRows[i].pctLabel->setText(std::format("{:.0F}%", peripherals[i].state.percentage));
    }
    if (m_peripheralRows[i].bar != nullptr) {
      m_peripheralRows[i].bar->setProgress(
          static_cast<float>(std::clamp(peripherals[i].state.percentage / 100.0, 0.0, 1.0))
      );
    }
  }
}

void PowerTab::syncBatteryWell() {
  auto shown = [](const Node* node) {
    return node != nullptr && node->visible() && node->participatesInLayout();
  };

  const bool live = shown(m_statusCard) || shown(m_profilesCard) || shown(m_caffeineRow);
  const bool leftover =
      shown(m_lidCloseCard) || shown(m_chargingCard) || shown(m_healthCard) || shown(m_peripheralsCard);
  bool changed = setShown(m_batteryWell, live);
  changed |= setShown(m_leftoverScroll, leftover);
  if (changed) {
    if (auto* panels = PanelManager::current(); panels != nullptr) {
      panels->requestLayout();
    }
  }
}
