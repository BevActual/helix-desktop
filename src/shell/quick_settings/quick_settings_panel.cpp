#include "shell/quick_settings/quick_settings_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_service.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "system/brightness_service.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <utility>

namespace {

  using control_center::applyModuleWellStyle;

  // Shown when [control_center].shortcuts is empty.
  const std::vector<std::string> kDefaultPins = {
      "power_profile",
      "caffeine",
      "mic_mute",
  };

  struct EverydaySpec {
    std::string_view type;
    const char* settingsContext;
  };

  constexpr EverydaySpec kEverydaySpecs[] = {
      {"wifi", "network"},
      {"bluetooth", "bluetooth"},
      {"nightlight", "displays"},
      {"dark_mode", "appearance"},
      {"notification", "notifications"},
  };

  Button::ButtonStateColors shortcutState(ColorSpec bg, ColorSpec label) {
    return Button::ButtonStateColors{.bg = bg, .border = clearColorSpec(), .label = label};
  }

  Button::ButtonPalette tileIconPalette(bool on) {
    const auto label = on ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface);
    const auto rest = clearColorSpec();
    const auto hover = colorSpecFromRole(ColorRole::OnSurface, on ? 0.0F : 0.10F);
    return Button::ButtonPalette{
        .borderWidth = 0.0F,
        .normal = shortcutState(rest, label),
        .hover = shortcutState(hover, label),
        .pressed = shortcutState(colorSpecFromRole(ColorRole::OnSurface, on ? 0.0F : 0.16F), label),
        .disabled = shortcutState(clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface, 0.55F)),
        .selected = shortcutState(rest, label),
    };
  }

  void openSettings(std::string context) {
    PanelManager::instance().closePanel(false);
    PanelManager::instance().openSettings(std::move(context));
  }

} // namespace

QuickSettingsPanel::QuickSettingsPanel(
    ConfigService* config, ShortcutServices shortcuts, PipeWireService* audio, BrightnessService* brightness,
    SessionActionRunner* session
)
    : m_config(config), m_shortcuts(shortcuts), m_audio(audio), m_brightness(brightness), m_session(session) {}

QuickSettingsPanel::~QuickSettingsPanel() = default;

PanelPlacement QuickSettingsPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.controlCenterPlacement : PanelPlacement::Floating;
}

float QuickSettingsPanel::cornerRadius() const noexcept {
  // Match the floating top bar's pill language so the dropdown reads as one family.
  return Style::scaledRadius2xl(contentScale());
}

bool QuickSettingsPanel::hasBrightness() const {
  return m_brightness != nullptr && m_brightness->available();
}

bool QuickSettingsPanel::hasMicrophone() const {
  return m_audio != nullptr && m_audio->defaultSource() != nullptr;
}

bool QuickSettingsPanel::hasMedia() const {
  return m_shortcuts.mpris != nullptr && m_shortcuts.mpris->activePlayer().has_value();
}

std::string QuickSettingsPanel::statusCaption(const Shortcut& shortcut) {
  if (!shortcut.enabled()) {
    return i18n::tr("control-center.home.quick-unavailable");
  }
  if (std::string status = shortcut.displayStatus(); !status.empty()) {
    return status;
  }
  return i18n::tr(shortcut.active() ? "control-center.home.quick-on" : "control-center.home.quick-off");
}

std::string QuickSettingsPanel::tileTitle(const Shortcut& shortcut) {
  if (shortcut.id() == "notification") {
    // Full "Do Not Disturb" overflows the two-column tile at elevated ui_scale.
    return i18n::tr("control-center.shortcuts.notification");
  }
  if (shortcut.id() == "dark_mode") {
    // Title is stable; mode (Dark/Light/Auto) lives in the status line.
    return i18n::tr("control-center.shortcuts.appearance");
  }
  return shortcut.defaultLabel();
}

const char* QuickSettingsPanel::settingsContextForType(std::string_view type) {
  for (const EverydaySpec& spec : kEverydaySpecs) {
    if (spec.type == type) {
      return spec.settingsContext;
    }
  }
  return nullptr;
}

std::vector<ShortcutConfig> QuickSettingsPanel::resolvedShortcutOrder() const {
  std::vector<ShortcutConfig> order;
  if (m_config != nullptr && !m_config->config().controlCenter.shortcuts.empty()) {
    order = m_config->config().controlCenter.shortcuts;
  } else {
    order.reserve(std::size(kEverydaySpecs) + kDefaultPins.size());
    for (const EverydaySpec& spec : kEverydaySpecs) {
      order.push_back(ShortcutConfig{.type = std::string(spec.type)});
    }
    for (const auto& type : kDefaultPins) {
      order.push_back(ShortcutConfig{.type = type});
    }
  }
  for (const EverydaySpec& spec : kEverydaySpecs) {
    const bool present = std::ranges::any_of(order, [&](const ShortcutConfig& item) { return item.type == spec.type; });
    if (!present) {
      order.push_back(ShortcutConfig{.type = std::string(spec.type)});
    }
  }
  return order;
}

void QuickSettingsPanel::applyTileStyle(ToggleTile& tile) {
  if (tile.cell == nullptr || tile.shortcut == nullptr) {
    return;
  }
  const bool on = tile.shortcut->enabled() && tile.shortcut->active();
  tile.active = on;
  tile.cell->setFill(on ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::SurfaceVariant));
  tile.cell->setSoftness(0.0F);
  if (Style::highContrastEnabled()) {
    tile.cell->setBorder(colorSpecFromRole(on ? ColorRole::OnPrimary : ColorRole::Outline), Style::emphasizedBorderWidth);
  } else {
    tile.cell->clearBorder();
  }
  const auto statusColor = on ? colorSpecFromRole(ColorRole::OnPrimary, 0.85F) : colorSpecFromRole(ColorRole::OnSurfaceVariant);
  if (tile.status != nullptr) {
    tile.status->setColor(statusColor);
  }
  if (tile.iconButton != nullptr) {
    tile.iconButton->setVariant(ButtonVariant::Ghost);
    tile.iconButton->setCustomPalette(tileIconPalette(on));
    tile.iconButton->setSoftness(1.0F);
    tile.iconButton->setSelected(on);
    tile.iconButton->setEnabled(tile.shortcut->enabled());
  }
  if (tile.titleButton != nullptr) {
    tile.titleButton->setVariant(ButtonVariant::Ghost);
    tile.titleButton->setCustomPalette(tileIconPalette(on));
    tile.titleButton->setSoftness(1.0F);
    tile.titleButton->setEnabled(tile.shortcut->enabled());
  }
  if (tile.menuButton != nullptr) {
    tile.menuButton->setVariant(ButtonVariant::Ghost);
    tile.menuButton->setCustomPalette(tileIconPalette(on));
    tile.menuButton->setSoftness(1.0F);
  }
}

void QuickSettingsPanel::buildTiles() {
  for (auto& tile : m_tiles) {
    if (tile.shortcut != nullptr) {
      tile.shortcut->onPanelClose();
    }
  }
  m_tiles.clear();

  for (const ShortcutConfig& configured : resolvedShortcutOrder()) {
    if (m_config != nullptr && !ShortcutRegistry::isAvailable(configured.type, m_config->config())) {
      continue;
    }
    auto shortcut = ShortcutRegistry::create(configured.type, m_shortcuts);
    if (shortcut == nullptr) {
      continue;
    }
    const char* settingsContext = settingsContextForType(configured.type);
    if (settingsContext == nullptr && !shortcut->enabled()) {
      continue;
    }
    shortcut->onPanelOpen();
    ToggleTile tile;
    tile.shortcut = std::move(shortcut);
    tile.size = configured.size;
    if (settingsContext != nullptr) {
      tile.settingsContext = settingsContext;
    }
    m_tiles.push_back(std::move(tile));
  }
}

QuickSettingsPanel::ShadeMode QuickSettingsPanel::shadeModeForContext(std::string_view context) {
  // Desktop clock deep-links used "clock"; bar clock/notifications use "notifications".
  if (context == "notifications" || context == "clock") {
    return ShadeMode::Notifications;
  }
  return ShadeMode::QuickSettings;
}

QuickSettingsPanel::ShadeMode QuickSettingsPanel::activeShadeMode() const {
  const auto pending = pendingOpenContext();
  if (!pending.empty()) {
    return shadeModeForContext(pending);
  }
  return m_mode;
}

bool QuickSettingsPanel::isContextActive(std::string_view context) const {
  if (context.empty()) {
    return false;
  }
  return shadeModeForContext(context) == m_mode;
}

bool QuickSettingsPanel::requiresRecreateForContext(std::string_view context) const {
  return shadeModeForContext(context.empty() ? "home" : context) != m_mode;
}

float QuickSettingsPanel::preferredWidth() const {
  const float column =
      activeShadeMode() == ShadeMode::Notifications ? kNotifColumnWidth : kQsColumnWidth;
  // PanelManager lays the root out inside the chrome inset, so the visual size
  // has to include panelPadding or the cards paint past the rounded border.
  return scaled(column + Style::spaceMd * 2.0F + Style::panelPadding * 2.0F);
}

float QuickSettingsPanel::preferredHeight() const {
  const float scale = contentScale();
  if (activeShadeMode() == ShadeMode::Notifications) {
    // Tall enough for a few two-line history rows, plus the chrome inset.
    return std::ceil(
        Style::controlHeightLg * 10.0F * scale + Style::spaceMd * 2.0F * scale + Style::panelPadding * 2.0F * scale
    );
  }

  const std::size_t tiles =
      m_tiles.empty() ? resolvedShortcutOrder().size() : m_tiles.size();
  const std::size_t tileRows = (tiles + 1) / 2;

  float qs = Style::spaceMd * 2.0F * scale;
  if (tileRows > 0) {
    qs += static_cast<float>(tileRows) * (Style::controlHeightLg * 1.35F) * scale
        + static_cast<float>(tileRows - 1) * Style::spaceSm * scale + Style::spaceSm * scale;
  }
  if (hasMedia()) {
    qs += Style::controlHeightLg * scale + Style::spaceSm * scale;
  }
  qs += Style::controlHeightSm * scale; // volume
  if (hasMicrophone()) {
    qs += Style::controlHeightSm * scale + Style::spaceSm * scale;
  }
  if (hasBrightness()) {
    qs += Style::controlHeightSm * scale + Style::spaceSm * scale;
  }
  qs += Style::spaceSm * scale;
  qs += Style::controlHeight * scale;
  qs += Style::spaceSm * 2.0F * scale;
  qs += Style::panelPadding * 2.0F * scale;
  return std::ceil(qs);
}

void QuickSettingsPanel::create() {
  const float scale = contentScale();
  m_mode = activeShadeMode();
  const bool showNotifications = m_mode == ShadeMode::Notifications;
  const bool showQuickSettings = m_mode == ShadeMode::QuickSettings;
  m_footerButtons.clear();

  if (showQuickSettings) {
    buildTiles();
    m_volumeShortcut = ShortcutRegistry::create("audio", m_shortcuts);
    if (m_volumeShortcut != nullptr) {
      m_volumeShortcut->onPanelOpen();
    }
    m_micShortcut = ShortcutRegistry::create("mic_mute", m_shortcuts);
    if (m_micShortcut != nullptr) {
      m_micShortcut->onPanelOpen();
    }
  } else {
    m_tiles.clear();
  }

  auto shade = ui::row({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .padding = Style::spaceMd * scale,
  });

  if (showNotifications) {
    auto notifColumn = ui::column({
        .out = &m_notifColumn,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .minWidth = kNotifColumnWidth * scale,
        .flexGrow = 1.0F,
        .configure =
            [scale](Flex& host) {
              applyModuleWellStyle(host, scale);
              host.setDirection(FlexDirection::Vertical);
              host.setGap(Style::spaceSm * scale);
              host.setPadding(Style::spaceSm * scale);
            },
    });
    auto notifHeader = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    notifHeader->addChild(ui::label({
        .text = i18n::tr("control-center.notifications.history"),
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    }));
    notifHeader->addChild(ui::button({
        .out = &m_clearNotifications,
        .text = i18n::tr("control-center.notifications.clear-all"),
        .fontSize = Style::fontSizeCaption * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("control-center.notifications.clear-all"),
        .onClick =
            [this]() {
              if (m_shortcuts.notifications != nullptr) {
                m_shortcuts.notifications->clearHistory();
              }
              PanelManager::instance().requestUpdateOnly();
            },
        .configure = [](Button& button) { button.setSoftness(1.0F); },
    }));
    notifColumn->addChild(std::move(notifHeader));
    notifColumn->addChild(ui::label({
        .out = &m_notifEmpty,
        .text = i18n::tr("control-center.notifications.empty-title"),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = 2,
        .visible = false,
        .participatesInLayout = false,
    }));
    auto notifScroll = ui::scrollView({
        .out = &m_notifScroll,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .flexGrow = 1.0F,
        .configure =
            [](ScrollView& scrollView) {
              scrollView.clearFill();
              scrollView.clearBorder();
            },
    });
    auto* notifContent = notifScroll->content();
    notifContent->setDirection(FlexDirection::Vertical);
    notifContent->setAlign(FlexAlign::Stretch);
    notifContent->setGap(Style::spaceSm * scale);
    m_notifList = notifContent;
    notifColumn->addChild(std::move(notifScroll));
    shade->addChild(std::move(notifColumn));
  }

  if (!showQuickSettings) {
    setRoot(std::move(shade));
    if (m_animations != nullptr) {
      this->root()->setAnimationManager(m_animations);
    }
    rebuildNotificationList();
    return;
  }

  auto root = ui::column({
      .out = &m_qsColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .minWidth = kQsColumnWidth * scale,
      .flexGrow = 1.0F,
  });

  {
    auto grid = ui::column({.out = &m_tileGrid, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
    root->addChild(std::move(grid));
    populateTileGrid();
  }

  auto media = ui::row({
      .out = &m_mediaWell,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .configure =
          [scale](Flex& host) {
            applyModuleWellStyle(host, scale);
            host.setGap(Style::spaceSm * scale);
            host.setPadding(Style::spaceSm * scale);
          },
  });
  auto mediaText = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * 0.5F * scale,
      .flexGrow = 1.0F,
  });
  mediaText->addChild(ui::label({
      .out = &m_mediaTitle,
      .text = {},
      .fontSize = Style::fontSizeBody * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));
  mediaText->addChild(ui::label({
      .out = &m_mediaArtist,
      .text = {},
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));
  media->addChild(std::move(mediaText));
  const float mediaBtn = Style::controlHeightSm * scale;
  const auto mediaButton = [scale, mediaBtn, this](Button** out, std::string glyph, std::function<void()> onClick) {
    auto button = ui::button({
        .out = out,
        .glyph = std::move(glyph),
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Ghost,
        .minWidth = mediaBtn,
        .minHeight = mediaBtn,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = std::move(onClick),
        .configure = [](Button& control) { control.setSoftness(1.0F); },
    });
    m_footerButtons.push_back(button.get());
    return button;
  };
  media->addChild(mediaButton(&m_mediaPrev, "media-prev", [this]() {
    if (m_shortcuts.mpris != nullptr) {
      (void)m_shortcuts.mpris->previousActive();
    }
    PanelManager::instance().requestUpdateOnly();
  }));
  media->addChild(mediaButton(&m_mediaPlayPause, "media-play", [this]() {
    if (m_shortcuts.mpris != nullptr) {
      (void)m_shortcuts.mpris->playPauseActive();
    }
    PanelManager::instance().requestUpdateOnly();
  }));
  media->addChild(mediaButton(&m_mediaNext, "media-next", [this]() {
    if (m_shortcuts.mpris != nullptr) {
      (void)m_shortcuts.mpris->nextActive();
    }
    PanelManager::instance().requestUpdateOnly();
  }));
  root->addChild(std::move(media));

  auto well = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .configure =
          [scale](Flex& host) {
            applyModuleWellStyle(host, scale);
            host.setDirection(FlexDirection::Vertical);
            host.setGap(Style::spaceSm * scale);
            host.setPadding(Style::spaceSm * scale);
          },
  });

  auto volumeRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  volumeRow->addChild(ui::button({
      .out = &m_volumeButton,
      .glyph = m_volumeShortcut != nullptr ? m_volumeShortcut->displayIcon() : std::string("volume-high"),
      .tooltip = i18n::tr("control-center.shortcuts.audio"),
      .onClick =
          [this]() {
            if (m_volumeShortcut != nullptr) {
              m_volumeShortcut->onClick();
            }
            PanelManager::instance().requestUpdateOnly();
          },
      .onRightClick = []() { openSettings("sound"); },
      .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
  }));
  volumeRow->addChild(ui::slider({
      .out = &m_volume,
      .minValue = 0.0,
      .maxValue = 1.5,
      .step = 0.01,
      .trackHeight = Style::sliderTrackHeight * scale,
      .thumbSize = Style::sliderThumbSize * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .wheelAdjustEnabled = true,
      .flexGrow = 1.0F,
      .onValueChanged =
          [this](double value) {
            if (m_syncingSliders || m_audio == nullptr) {
              return;
            }
            m_audio->setVolume(static_cast<float>(value));
          },
  }));
  volumeRow->addChild(ui::button({
      .glyph = Style::rtl() ? "chevron-left" : "chevron-right",
      .variant = ButtonVariant::Ghost,
      .tooltip = i18n::tr("control-center.shortcuts.audio"),
      .minWidth = Style::controlHeightSm * scale,
      .minHeight = Style::controlHeightSm * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = []() { openSettings("sound"); },
      .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
  }));
  well->addChild(std::move(volumeRow));

  auto micHost = ui::column({
      .out = &m_micRow,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  auto micRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  micRow->addChild(ui::button({
      .out = &m_micButton,
      .glyph = m_micShortcut != nullptr ? m_micShortcut->displayIcon() : std::string("microphone"),
      .tooltip = i18n::tr("control-center.shortcuts.mic-mute"),
      .onClick =
          [this]() {
            if (m_micShortcut != nullptr) {
              m_micShortcut->onClick();
            }
            PanelManager::instance().requestUpdateOnly();
          },
      .onRightClick = []() { openSettings("sound"); },
      .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
  }));
  micRow->addChild(ui::slider({
      .out = &m_mic,
      .minValue = 0.0,
      .maxValue = 1.0,
      .step = 0.01,
      .trackHeight = Style::sliderTrackHeight * scale,
      .thumbSize = Style::sliderThumbSize * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .wheelAdjustEnabled = true,
      .flexGrow = 1.0F,
      .onValueChanged =
          [this](double value) {
            if (m_syncingSliders || m_audio == nullptr) {
              return;
            }
            m_audio->setMicVolume(static_cast<float>(value));
          },
  }));
  micHost->addChild(std::move(micRow));
  well->addChild(std::move(micHost));

  auto brightnessHost = ui::column({
      .out = &m_brightnessRow,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  auto brightnessRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  brightnessRow->addChild(ui::button({
      .glyph = "brightness-high",
      .tooltip = i18n::tr("control-center.display.brightness"),
      .onClick = []() { openSettings("displays"); },
      .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
  }));
  brightnessRow->addChild(ui::slider({
      .out = &m_brightnessSlider,
      .minValue = 0.0,
      .maxValue = 1.0,
      .step = 0.01,
      .trackHeight = Style::sliderTrackHeight * scale,
      .thumbSize = Style::sliderThumbSize * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .wheelAdjustEnabled = true,
      .flexGrow = 1.0F,
      .onValueChanged =
          [this](double value) {
            if (m_syncingSliders || m_brightness == nullptr) {
              return;
            }
            m_brightness->setAllBrightness(static_cast<float>(value));
          },
  }));
  brightnessHost->addChild(std::move(brightnessRow));
  well->addChild(std::move(brightnessHost));
  root->addChild(std::move(well));

  const float actionSize = Style::controlHeight * scale;
  const auto sessionButton = [scale, actionSize, this](
                                 Button** out, std::string glyph, std::string label, std::function<void()> onClick
                             ) {
    auto button = ui::button({
        .out = out,
        .glyph = std::move(glyph),
        .glyphSize = Style::fontSizeHeader * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = label,
        .minWidth = actionSize,
        .minHeight = actionSize,
        .flexGrow = 1.0F,
        .onClick = std::move(onClick),
        .configure =
            [](Button& control) {
              control.setSoftness(1.0F);
              control.setJustify(FlexJustify::Center);
            },
    });
    m_footerButtons.push_back(button.get());
    return button;
  };

  // GNOME SystemItem: Settings, Lock, Power — Suspend stays as a Helix convenience.
  auto session = ui::row({
      .out = &m_sessionWell,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .configure =
          [scale](Flex& host) {
            applyModuleWellStyle(host, scale);
            host.setGap(Style::spaceSm * scale);
            host.setPadding(Style::spaceSm * scale);
          },
  });
  session->addChild(sessionButton(
      &m_settingsButton, "settings", i18n::tr("control-center.open-settings"),
      []() { openSettings("control-center"); }
  ));
  session->addChild(sessionButton(
      &m_lockButton, std::string(session_action::defaultGlyph("lock")), i18n::tr("session.actions.lock"),
      [this]() {
        PanelManager::instance().closePanel(false);
        if (m_session != nullptr) {
          (void)m_session->lock();
        }
      }
  ));
  session->addChild(sessionButton(
      &m_suspendButton, std::string(session_action::defaultGlyph("suspend")), i18n::tr("session.actions.suspend"),
      [this]() {
        PanelManager::instance().closePanel(false);
        if (m_session != nullptr) {
          m_session->invoke(SessionPanelActionConfig{.action = "suspend"});
        }
      }
  ));
  session->addChild(sessionButton(
      nullptr, "power", i18n::tr("control-center.tabs.power"),
      []() {
        PanelManager::instance().closePanel(false);
        PanelManager::instance().togglePanel("session");
      }
  ));
  root->addChild(std::move(session));
  shade->addChild(std::move(root));

  setRoot(std::move(shade));
  if (m_animations != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }
  syncSliders();
  syncTiles();
  syncMedia();
  syncSessionVisibility();
}

void QuickSettingsPanel::populateTileGrid() {
  if (m_tileGrid == nullptr) {
    return;
  }
  while (!m_tileGrid->children().empty()) {
    m_tileGrid->removeChild(m_tileGrid->children().back().get());
  }
  const float scale = contentScale();
  const float tileHeight = Style::controlHeightLg * 1.35F * scale;
  for (std::size_t i = 0; i < m_tiles.size(); i += 2) {
    auto row = ui::row({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    for (std::size_t j = i; j < std::min(i + 2, m_tiles.size()); ++j) {
      ToggleTile& tile = m_tiles[j];
      Shortcut* shortcut = tile.shortcut.get();
      const std::string settingsContext = tile.settingsContext;
      const auto toggle = [this, shortcut]() {
        if (m_tileDrag.active || m_tileDrag.armed || m_tileDrag.suppressClick) {
          return;
        }
        shortcut->onClick();
        PanelManager::instance().requestUpdateOnly();
      };
      const auto openDetails = [this, settingsContext, shortcut]() {
        if (m_tileDrag.active || m_tileDrag.armed || m_tileDrag.suppressClick) {
          return;
        }
        if (!settingsContext.empty()) {
          openSettings(settingsContext);
          return;
        }
        shortcut->onRightClick();
      };

      auto cell = ui::row({
          .out = &tile.cell,
          .align = FlexAlign::Center,
          .gap = Style::spaceXs * scale,
          .padding = Style::spaceSm * scale,
          .minHeight = tileHeight,
          .flexGrow = 1.0F,
          .configure =
              [scale](Flex& host) {
                applyModuleWellStyle(host, scale);
                host.setPadding(Style::spaceSm * scale);
                host.setGap(Style::spaceXs * scale);
              },
      });
      cell->addChild(ui::button({
          .out = &tile.iconButton,
          .glyph = shortcut->displayIcon(),
          .glyphSize = Style::fontSizeHeader * scale,
          .tooltip = shortcut->defaultLabel(),
          .minWidth = Style::controlHeight * scale,
          .minHeight = Style::controlHeight * scale,
          .radius = Style::scaledRadiusXl(scale),
          .onClick = toggle,
          .onRightClick = openDetails,
      }));
      auto names = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * 0.5F * scale,
          .flexGrow = 1.0F,
      });
      const std::string title = tileTitle(*shortcut);
      names->addChild(ui::button({
          .out = &tile.titleButton,
          .text = title,
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = title,
          .onClick = toggle,
          .onRightClick = openDetails,
          .configure =
              [](Button& button) {
                button.setSoftness(1.0F);
                button.setJustify(FlexJustify::Start);
                button.setContentAlign(ButtonContentAlign::Start);
              },
      }));
      names->addChild(ui::label({
          .out = &tile.status,
          .text = statusCaption(*shortcut),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }));
      cell->addChild(std::move(names));
      if (!settingsContext.empty()) {
        cell->addChild(ui::button({
            .out = &tile.menuButton,
            .glyph = Style::rtl() ? "chevron-left" : "chevron-right",
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr(
                shortcut->id() == "wifi"         ? "control-center.tabs.network"
                : shortcut->id() == "bluetooth"  ? "control-center.tabs.bluetooth"
                : shortcut->id() == "nightlight" ? "control-center.tabs.monitor"
                : shortcut->id() == "dark_mode"  ? "control-center.shortcuts.appearance"
                                                 : "control-center.tabs.notifications"
            ),
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = openDetails,
        }));
      }
      tile.label = title;
      tile.icon = shortcut->displayIcon();
      tile.statusText = statusCaption(*shortcut);
      applyTileStyle(tile);
      wireTileReorder(tile, j);
      row->addChild(std::move(cell));
    }
    if (m_tiles.size() - i == 1) {
      row->addChild(ui::box({.flexGrow = 1.0F}));
    }
    m_tileGrid->addChild(std::move(row));
  }
}

void QuickSettingsPanel::wireTileReorder(ToggleTile& tile, std::size_t index) {
  const auto press = [this, index](float localX, float localY, bool pressed) {
    if (pressed) {
      beginTileDrag(index, localX, localY);
      return;
    }
    endTileDrag(true);
  };
  const auto motion = [this, index](float localX, float localY) { updateTileDrag(index, localX, localY); };
  if (tile.iconButton != nullptr) {
    tile.iconButton->setOnPress(press);
    tile.iconButton->setOnPointerMotion(motion);
  }
  if (tile.titleButton != nullptr) {
    tile.titleButton->setOnPress(press);
    tile.titleButton->setOnPointerMotion(motion);
  }
}

void QuickSettingsPanel::resetTileDrag() {
  m_tileDrag.holdTimer.stop();
  if (m_tileDrag.sourceIndex < m_tiles.size() && m_tiles[m_tileDrag.sourceIndex].cell != nullptr) {
    m_tiles[m_tileDrag.sourceIndex].cell->setOpacity(1.0F);
    m_tiles[m_tileDrag.sourceIndex].cell->clearBorder();
  }
  for (auto& tile : m_tiles) {
    if (tile.cell != nullptr) {
      tile.cell->clearBorder();
    }
  }
  m_tileDrag.armed = false;
  m_tileDrag.active = false;
  m_tileDrag.sourceIndex = 0;
  m_tileDrag.targetIndex = 0;
  m_tileDrag.startX = 0.0F;
  m_tileDrag.startY = 0.0F;
}

void QuickSettingsPanel::beginTileDrag(std::size_t index, float localX, float localY) {
  resetTileDrag();
  if (index >= m_tiles.size()) {
    return;
  }
  m_tileDrag.sourceIndex = index;
  m_tileDrag.targetIndex = index;
  m_tileDrag.startX = localX;
  m_tileDrag.startY = localY;
  m_tileDrag.holdTimer.start(std::chrono::milliseconds(300), [this, index]() {
    if (m_tileDrag.sourceIndex == index && !m_tileDrag.active) {
      m_tileDrag.armed = true;
    }
  });
}

void QuickSettingsPanel::updateTileDrag(std::size_t index, float localX, float localY) {
  if (m_tileDrag.sourceIndex != index || index >= m_tiles.size()) {
    return;
  }
  const float scale = contentScale();
  const float dx = localX - m_tileDrag.startX;
  const float dy = localY - m_tileDrag.startY;
  if (!m_tileDrag.active) {
    const bool travelled = std::hypot(dx, dy) >= Style::dragStartThreshold * scale;
    if (!m_tileDrag.armed && !travelled) {
      return;
    }
    m_tileDrag.holdTimer.stop();
    m_tileDrag.armed = true;
    m_tileDrag.active = true;
    m_tileDrag.suppressClick = true;
    if (m_tiles[index].cell != nullptr) {
      m_tiles[index].cell->setOpacity(0.85F);
    }
  }
  float absX = 0.0F;
  float absY = 0.0F;
  if (m_tiles[index].cell != nullptr) {
    Node::absolutePosition(m_tiles[index].cell, absX, absY);
  }
  const auto target = tileIndexAtScene(absX + localX, absY + localY);
  if (!target.has_value() || *target == m_tileDrag.targetIndex) {
    return;
  }
  for (auto& tile : m_tiles) {
    if (tile.cell != nullptr) {
      tile.cell->clearBorder();
    }
  }
  m_tileDrag.targetIndex = *target;
  if (m_tiles[*target].cell != nullptr && *target != m_tileDrag.sourceIndex) {
    m_tiles[*target].cell->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
  }
  PanelManager::instance().requestUpdateOnly();
}

void QuickSettingsPanel::endTileDrag(bool commit) {
  const bool wasActive = m_tileDrag.active;
  const std::size_t from = m_tileDrag.sourceIndex;
  const std::size_t to = m_tileDrag.targetIndex;
  resetTileDrag();
  if (wasActive) {
    m_tileDrag.suppressClick = true;
    DeferredCall::callLater([this]() { m_tileDrag.suppressClick = false; });
  }
  if (commit && wasActive && from != to) {
    commitTileReorder(from, to);
  } else {
    PanelManager::instance().requestUpdateOnly();
  }
}

std::optional<std::size_t> QuickSettingsPanel::tileIndexAtScene(float sceneX, float sceneY) const {
  for (std::size_t i = 0; i < m_tiles.size(); ++i) {
    Flex* cell = m_tiles[i].cell;
    if (cell == nullptr) {
      continue;
    }
    float absX = 0.0F;
    float absY = 0.0F;
    Node::absolutePosition(cell, absX, absY);
    if (sceneX >= absX && sceneX <= absX + cell->width() && sceneY >= absY && sceneY <= absY + cell->height()) {
      return i;
    }
  }
  return std::nullopt;
}

void QuickSettingsPanel::commitTileReorder(std::size_t from, std::size_t to) {
  if (m_config == nullptr || from >= m_tiles.size() || to >= m_tiles.size() || from == to) {
    return;
  }
  std::vector<ShortcutConfig> next;
  next.reserve(m_tiles.size());
  for (const auto& tile : m_tiles) {
    if (tile.shortcut == nullptr) {
      continue;
    }
    next.push_back(ShortcutConfig{.type = std::string(tile.shortcut->id()), .size = tile.size});
  }
  if (from >= next.size() || to >= next.size()) {
    return;
  }
  ShortcutConfig moved = std::move(next[from]);
  next.erase(next.begin() + static_cast<std::ptrdiff_t>(from));
  next.insert(next.begin() + static_cast<std::ptrdiff_t>(std::min(to, next.size())), std::move(moved));
  if (!m_config->setOverride({"control_center", "shortcuts"}, std::move(next))) {
    return;
  }
  refreshAfterTileReorder();
}

void QuickSettingsPanel::refreshAfterTileReorder() {
  buildTiles();
  populateTileGrid();
  syncTiles();
  PanelManager::instance().relayoutActivePanelPreferredSize();
  PanelManager::instance().requestUpdateOnly();
}

void QuickSettingsPanel::onConfigReloaded() {
  if (m_tileGrid == nullptr || m_tileDrag.active || m_tileDrag.armed) {
    return;
  }
  refreshAfterTileReorder();
}

void QuickSettingsPanel::onOpen(std::string_view context) {
  m_mode = shadeModeForContext(context.empty() ? "home" : context);
  if (m_mode == ShadeMode::Notifications) {
    m_unreadAtOpen.clear();
    if (m_shortcuts.notifications != nullptr) {
      for (const auto& entry : m_shortcuts.notifications->history()) {
        if (!entry.seen) {
          m_unreadAtOpen.insert(entry.notification.id);
        }
      }
      m_shortcuts.notifications->markNotificationHistorySeen();
    }
    rebuildNotificationList();
    return;
  }
  if (m_volumeShortcut != nullptr) {
    m_volumeShortcut->onPanelOpen();
  }
  if (m_micShortcut != nullptr) {
    m_micShortcut->onPanelOpen();
  }
  for (auto& tile : m_tiles) {
    if (tile.shortcut != nullptr) {
      tile.shortcut->onPanelOpen();
    }
  }
}

void QuickSettingsPanel::onClose() {
  m_unreadAtOpen.clear();
  resetTileDrag();
  m_tileDrag.suppressClick = false;
  if (m_volumeShortcut != nullptr) {
    m_volumeShortcut->onPanelClose();
    m_volumeShortcut.reset();
  }
  if (m_micShortcut != nullptr) {
    m_micShortcut->onPanelClose();
    m_micShortcut.reset();
  }
  for (auto& tile : m_tiles) {
    if (tile.shortcut != nullptr) {
      tile.shortcut->onPanelClose();
    }
  }
  m_root = nullptr;
  m_notifColumn = nullptr;
  m_notifList = nullptr;
  m_qsColumn = nullptr;
  m_tileGrid = nullptr;
  m_mediaWell = nullptr;
  m_micRow = nullptr;
  m_brightnessRow = nullptr;
  m_sessionWell = nullptr;
  m_notifEmpty = nullptr;
  m_mediaTitle = nullptr;
  m_mediaArtist = nullptr;
  m_mediaPrev = nullptr;
  m_mediaPlayPause = nullptr;
  m_mediaNext = nullptr;
  m_volumeButton = nullptr;
  m_micButton = nullptr;
  m_settingsButton = nullptr;
  m_lockButton = nullptr;
  m_suspendButton = nullptr;
  m_clearNotifications = nullptr;
  m_notifScroll = nullptr;
  m_volume = nullptr;
  m_mic = nullptr;
  m_brightnessSlider = nullptr;
  m_tiles.clear();
  m_footerButtons.clear();
  m_notificationSerial = 0;
  clearReleasedRoot();
}

void QuickSettingsPanel::onPanelCardOpacityChanged(float opacity) {
  for (auto& tile : m_tiles) {
    if (tile.iconButton != nullptr) {
      tile.iconButton->setSurfaceOpacity(opacity);
    }
  }
}

void QuickSettingsPanel::syncSliders() {
  if (m_volumeButton != nullptr && m_volumeShortcut != nullptr) {
    m_volumeButton->setGlyph(m_volumeShortcut->displayIcon());
    m_volumeButton->setVariant(ButtonVariant::Ghost);
    m_volumeButton->setCustomPalette(tileIconPalette(false));
    m_volumeButton->setSoftness(1.0F);
    m_volumeButton->setSelected(false);
  }
  if (m_volume != nullptr && m_audio != nullptr && !m_volume->dragging()) {
    const AudioNode* sink = m_audio->defaultSink();
    m_syncingSliders = true;
    if (sink != nullptr) {
      m_volume->setEnabled(true);
      m_volume->setValue(std::clamp(static_cast<double>(sink->volume), 0.0, 1.5));
    } else {
      m_volume->setEnabled(false);
    }
    m_syncingSliders = false;
  }

  if (m_micButton != nullptr && m_micShortcut != nullptr) {
    m_micButton->setGlyph(m_micShortcut->displayIcon());
    m_micButton->setVariant(ButtonVariant::Ghost);
    m_micButton->setCustomPalette(tileIconPalette(false));
    m_micButton->setSoftness(1.0F);
    m_micButton->setSelected(false);
  }
  if (m_micRow != nullptr) {
    const bool visible = hasMicrophone();
    m_micRow->setVisible(visible);
    m_micRow->setParticipatesInLayout(visible);
  }
  if (m_mic != nullptr && m_audio != nullptr && !m_mic->dragging()) {
    const AudioNode* source = m_audio->defaultSource();
    m_syncingSliders = true;
    if (source != nullptr) {
      m_mic->setEnabled(true);
      m_mic->setValue(std::clamp(static_cast<double>(source->volume), 0.0, 1.0));
    } else {
      m_mic->setEnabled(false);
    }
    m_syncingSliders = false;
  }

  const BrightnessDisplay* shown = nullptr;
  if (m_brightness != nullptr) {
    for (const BrightnessDisplay& display : m_brightness->displays()) {
      if (display.controllable) {
        shown = &display;
        break;
      }
    }
  }
  if (m_brightnessRow != nullptr) {
    const bool visible = shown != nullptr;
    m_brightnessRow->setVisible(visible);
    m_brightnessRow->setParticipatesInLayout(visible);
  }
  if (shown != nullptr && m_brightnessSlider != nullptr && !m_brightnessSlider->dragging()) {
    m_syncingSliders = true;
    m_brightnessSlider->setValue(shown->brightness);
    m_syncingSliders = false;
  }
}

void QuickSettingsPanel::syncTiles() {
  for (auto& tile : m_tiles) {
    if (tile.shortcut == nullptr) {
      continue;
    }
    const std::string label = tileTitle(*tile.shortcut);
    const std::string icon = tile.shortcut->displayIcon();
    const std::string status = statusCaption(*tile.shortcut);
    if (label != tile.label && tile.titleButton != nullptr) {
      tile.label = label;
      tile.titleButton->setText(label);
    }
    if (icon != tile.icon && tile.iconButton != nullptr) {
      tile.icon = icon;
      tile.iconButton->setGlyph(icon);
    }
    if (status != tile.statusText && tile.status != nullptr) {
      tile.statusText = status;
      tile.status->setText(status);
    }
    if (tile.iconButton != nullptr) {
      tile.iconButton->setTooltip(label + ": " + status);
    }
    if (tile.titleButton != nullptr) {
      tile.titleButton->setTooltip(label + ": " + status);
    }
    applyTileStyle(tile);
  }
}

void QuickSettingsPanel::syncMedia() {
  const auto active = m_shortcuts.mpris != nullptr ? m_shortcuts.mpris->activePlayer() : std::nullopt;
  const bool visible = active.has_value();
  if (m_mediaWell != nullptr) {
    m_mediaWell->setVisible(visible);
    m_mediaWell->setParticipatesInLayout(visible);
  }
  if (!visible) {
    return;
  }
  const std::string title = !active->title.empty() ? active->title : active->identity;
  const std::string artist = joinedArtists(active->artists);
  if (m_mediaTitle != nullptr) {
    m_mediaTitle->setText(title);
  }
  if (m_mediaArtist != nullptr) {
    m_mediaArtist->setText(artist.empty() ? active->identity : artist);
    m_mediaArtist->setVisible(!artist.empty() || !active->identity.empty());
  }
  const bool playing = active->playbackStatus == "Playing";
  if (m_mediaPlayPause != nullptr) {
    m_mediaPlayPause->setGlyph(playing ? "media-pause" : "media-play");
    m_mediaPlayPause->setEnabled(active->canPlay || active->canPause || active->canControl);
  }
  if (m_mediaPrev != nullptr) {
    m_mediaPrev->setEnabled(active->canGoPrevious);
  }
  if (m_mediaNext != nullptr) {
    m_mediaNext->setEnabled(active->canGoNext);
  }
}

void QuickSettingsPanel::rebuildNotificationList() {
  if (m_notifList == nullptr) {
    return;
  }
  while (!m_notifList->children().empty()) {
    m_notifList->removeChild(m_notifList->children().back().get());
  }
  NotificationManager* manager = m_shortcuts.notifications;
  m_notificationSerial = manager != nullptr ? manager->changeSerial() : 0;

  const auto& history = manager != nullptr ? manager->history() : std::deque<NotificationHistoryEntry>{};
  constexpr std::size_t kMaxVisible = 24;
  std::size_t shown = 0;
  for (auto it = history.rbegin(); it != history.rend() && shown < kMaxVisible; ++it, ++shown) {
    const Notification& n = it->notification;
    const std::uint32_t id = n.id;
    const std::string app = notificationDisplayAppName(n);
    const std::string title = n.summary.empty() ? i18n::tr("control-center.notifications.untitled") : n.summary;
    const float scale = contentScale();
    // Bound titles to the column so long summaries wrap instead of painting past the card.
    const float textMax = std::max(
        96.0F, (kNotifColumnWidth - Style::controlHeightSm - Style::spaceLg * 3.0F) * scale
    );
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
        .configure =
            [scale](Flex& host) {
              applyModuleWellStyle(host, scale);
              host.setPadding(Style::spaceSm * scale);
              host.setGap(Style::spaceSm * scale);
            },
    });
    auto text = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * 0.5F * scale,
        .flexGrow = 1.0F,
    });
    if (!it->seen || m_unreadAtOpen.contains(n.id)) {
      row->addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = Style::spaceXs * scale,
          .width = Style::spaceSm * scale,
          .height = Style::spaceSm * scale,
      }));
    }
    text->addChild(ui::label({
        .text = title,
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxWidth = textMax,
        .maxLines = 2,
        .ellipsize = TextEllipsize::End,
    }));
    text->addChild(ui::label({
        .text = app.empty() ? n.body : (n.body.empty() ? app : app + " — " + n.body),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = textMax,
        .maxLines = 2,
        .ellipsize = TextEllipsize::End,
    }));
    row->addChild(std::move(text));
    row->addChild(ui::button({
        .glyph = "x",
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("control-center.notifications.remove"),
        .minWidth = Style::controlHeightSm * scale,
        .minHeight = Style::controlHeightSm * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick =
            [this, id]() {
              if (m_shortcuts.notifications != nullptr) {
                m_shortcuts.notifications->removeHistoryEntry(id, CloseReason::Dismissed);
              }
              PanelManager::instance().requestUpdateOnly();
            },
        .configure = [](Button& button) { button.setSoftness(1.0F); },
    }));
    m_notifList->addChild(std::move(row));
  }

  const bool empty = shown == 0;
  if (m_notifEmpty != nullptr) {
    m_notifEmpty->setVisible(empty);
    m_notifEmpty->setParticipatesInLayout(empty);
    m_notifEmpty->setText(i18n::tr("control-center.notifications.empty-title"));
  }
  if (m_clearNotifications != nullptr) {
    m_clearNotifications->setEnabled(!empty);
  }
  if (m_notifScroll != nullptr) {
    m_notifScroll->setVisible(!empty);
    m_notifScroll->setParticipatesInLayout(!empty);
  }
}

void QuickSettingsPanel::syncNotifications() {
  NotificationManager* manager = m_shortcuts.notifications;
  const std::uint64_t serial = manager != nullptr ? manager->changeSerial() : 0;
  if (serial == m_notificationSerial && m_notifList != nullptr) {
    return;
  }
  rebuildNotificationList();
}

void QuickSettingsPanel::syncSessionVisibility() {
  const bool showSession = m_config == nullptr || m_config->config().controlCenter.showSessionButton;
  if (m_sessionWell != nullptr) {
    m_sessionWell->setVisible(showSession);
    m_sessionWell->setParticipatesInLayout(showSession);
  }
  if (m_settingsButton != nullptr) {
    m_settingsButton->setVisible(showSession);
    m_settingsButton->setParticipatesInLayout(showSession);
  }
  const bool showLock = showSession && m_session != nullptr && m_config != nullptr && m_config->isLockScreenEnabled();
  if (m_lockButton != nullptr) {
    m_lockButton->setVisible(showLock);
    m_lockButton->setParticipatesInLayout(showLock);
  }
  const bool showSuspend = showSession && m_session != nullptr;
  if (m_suspendButton != nullptr) {
    m_suspendButton->setVisible(showSuspend);
    m_suspendButton->setParticipatesInLayout(showSuspend);
  }
}

void QuickSettingsPanel::doUpdate(Renderer& /*renderer*/) {
  syncSliders();
  syncTiles();
  syncMedia();
  syncNotifications();
  syncSessionVisibility();
}

void QuickSettingsPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_root == nullptr) {
    return;
  }
  m_root->setClipChildren(true);
  m_root->setSize(width, height);
  m_root->layout(renderer);
  bool recap = false;
  for (auto& tile : m_tiles) {
    // Cap title/status to the space left beside the icon and chevron, then lay out again
    // so the button ellipsizes instead of painting over the tile edge.
    if (tile.cell != nullptr && tile.titleButton != nullptr && tile.cell->width() > 0.0F) {
      float reserved = Style::spaceSm * contentScale() * 2.0F;
      if (tile.iconButton != nullptr) {
        reserved += tile.iconButton->width() + Style::spaceXs * contentScale();
      }
      if (tile.menuButton != nullptr) {
        reserved += tile.menuButton->width() + Style::spaceXs * contentScale();
      }
      const float nameW = std::max(0.0F, tile.cell->width() - reserved);
      if (nameW > 0.0F) {
        tile.titleButton->setMaxWidth(nameW);
        if (tile.status != nullptr) {
          tile.status->setMaxWidth(nameW);
        }
        recap = true;
      }
    }
  }
  if (recap) {
    m_root->layout(renderer);
  }
  for (auto& tile : m_tiles) {
    if (tile.titleButton != nullptr) {
      tile.titleButton->updateInputArea();
    }
    if (tile.iconButton != nullptr) {
      tile.iconButton->updateInputArea();
    }
    if (tile.menuButton != nullptr) {
      tile.menuButton->updateInputArea();
    }
  }
  for (Button* button : m_footerButtons) {
    button->updateInputArea();
  }
  if (m_volumeButton != nullptr) {
    m_volumeButton->updateInputArea();
  }
  if (m_micButton != nullptr) {
    m_micButton->updateInputArea();
  }
}
