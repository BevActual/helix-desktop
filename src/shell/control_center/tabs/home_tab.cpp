#include "shell/control_center/tabs/home_tab.h"

#include "config/config_service.h"
#include "compositors/compositor_platform.h"
#include "calendar/calendar_service.h"
#include "core/deferred_call.h"
#include "core/files/directory_scanner.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/animation/animation_manager.h"
#include "render/core/async_texture_cache.h"
#include "render/scene/input_area.h"
#include "scripting/plugin_registry.h"
#include "shell/control_center/pin_reorder.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/panel/panel_button_style.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/wallpaper/wallpaper.h"
#include "capture/screen_recorder_service.h"
#include "dbus/cast/cast_service.h"
#include "dbus/firewall/firewall_service.h"
#include "dbus/kdeconnect/phone_connect_service.h"
#include "dbus/upower/upower_service.h"
#include "idle/idle_inhibitor.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_service.h"
#include "system/ai_usage_service.h"
#include "system/battery_warning_monitor.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/brightness_service.h"
#include "system/distro_info.h"
#include "system/gamma_service.h"
#include "system/package_kit_service.h"
#include "system/weather_service.h"
#include "net/localsend_service.h"
#include "net/tailscale_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "render/scene/node.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

using namespace control_center;

namespace {

  constexpr Logger kLog("control-center");

  constexpr float kHomeAvatarScale = 2.6F;
  // Avatar sources above this size decode noticeably slowly; warn so users understand why.
  constexpr std::uintmax_t kHomeAvatarSourceWarnBytes = 8ULL * 1024ULL * 1024ULL;

  float homeConcentricRadius(float outerRadius, float padding, float floorRadius) {
    return std::max(floorRadius, outerRadius - padding);
  }

  float homeMediaArtRadius(float scale) {
    return homeConcentricRadius(
        Style::scaledRadius3xl(scale), Style::spaceMd * scale, Style::scaledRadiusLg(scale)
    );
  }

  // Now Playing is a compact widget: album art beside track, progress, and transport.
  [[nodiscard]] float homeNowPlayingHeight(float scale) {
    const float art = Style::controlHeightLg * 1.6F * scale;
    const float text = Style::fontSizeTitle * scale + Style::fontSizeCaption * scale + 4.0F * scale
        + Style::controlHeight * scale + Style::spaceXs * 3.0F * scale;
    const float pad = Style::spaceMd * 2.0F * scale;
    return std::max(art, text) + pad;
  }

  [[nodiscard]] float homeClockMinHeight(float scale, bool eventRow) {
    const float time = Style::fontSizeHeader * 1.75F * scale;
    const float date = Style::fontSizeBody * 0.95F * scale;
    const float weather = Style::fontSizeBody * scale;
    const float event = eventRow ? Style::fontSizeBody * scale : 0.0F;
    const float gaps = Style::spaceSm * (eventRow ? 3.0F : 2.0F) * scale;
    const float pad = Style::spaceMd * 2.0F * scale;
    return time + date + weather + event + gaps + pad;
  }

  float homeTimeFontSize(float scale, float cardInnerHeight, bool eventRow) {
    const float minSize = Style::fontSizeHeader * 1.75F * scale;
    const float maxSize = Style::fontSizeBody * 3.8F * scale;
    if (cardInnerHeight <= 0.0F) {
      return minSize;
    }
    const float dateH = Style::fontSizeBody * 0.95F * scale;
    const float weatherH = Style::fontSizeBody * scale;
    const float eventH = eventRow ? Style::fontSizeBody * scale : 0.0F;
    const float gaps = Style::spaceSm * (eventRow ? 3.0F : 2.0F) * scale;
    const float leftover = cardInnerHeight - dateH - weatherH - eventH - gaps;
    return std::clamp(leftover * 0.78F, minSize, maxSize);
  }

  std::chrono::system_clock::time_point homeNextLocalMidnight(std::chrono::system_clock::time_point time) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    if (localtime_r(&raw, &local) == nullptr) {
      return time + std::chrono::hours{24};
    }
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    ++local.tm_mday;
    local.tm_isdst = -1;
    const std::time_t next = std::mktime(&local);
    return next == static_cast<std::time_t>(-1) ? time + std::chrono::hours{24}
                                                : std::chrono::system_clock::from_time_t(next);
  }

  std::chrono::system_clock::time_point homeEventEndInstant(const CalendarEvent& event) {
    if (!event.allDay) {
      return std::max(event.end, event.start);
    }
    if (event.end > event.start) {
      return event.end;
    }
    return homeNextLocalMidnight(event.start);
  }

  const CalendarEvent* homeNextCalendarEvent(const CalendarSnapshot& snapshot) {
    const auto now = std::chrono::system_clock::now();
    const CalendarEvent* current = nullptr;
    const CalendarEvent* upcoming = nullptr;
    for (const CalendarEvent& event : snapshot.events) {
      if (homeEventEndInstant(event) <= now) {
        continue;
      }
      if (!event.allDay && event.start <= now) {
        if (current == nullptr || event.start > current->start) {
          current = &event;
        }
      } else if (upcoming == nullptr || event.start < upcoming->start) {
        upcoming = &event;
      }
    }
    return current != nullptr ? current : upcoming;
  }

  constexpr auto kHomeTransientPositionRegressionWindow = std::chrono::milliseconds(1500);
  constexpr std::int64_t kHomeTransientPositionRegressionFloorUs = 5'000'000;
  constexpr std::int64_t kHomeTransientPositionRegressionCeilingUs = 1'500'000;
  constexpr std::int64_t kHomeTransientPositionRegressionDeltaUs = 5'000'000;
  constexpr int kHomeMediaArtLayoutPassLimit = 8;

  float homeAvatarSize(float scale) { return Style::controlHeightLg * kHomeAvatarScale * scale; }

  void openControlCenterTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  void openDetails(std::string_view page, std::string_view fallbackTab, CompositorPlatform* platform) {
    if (process::commandExists("helix-settings")) {
      DesktopEntry entry;
      entry.id = "org.helix.Settings";
      entry.name = "Helix Settings";
      entry.exec = "helix-settings --page " + std::string(page);
      desktop_entry_launch::LaunchOptions options;
      if (platform != nullptr && platform->hasXdgActivation()) {
        options.activationToken = platform->requestActivationToken(nullptr);
      }
      if (desktop_entry_launch::launchEntry(entry, options)) {
        PanelManager::instance().close();
        return;
      }
    }
    openControlCenterTab(fallbackTab);
  }

  void setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return;
    }
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
  }

  std::string formatShellTime(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.timeFormat.c_str() : "{:%H:%M}";
    return formatLocalTime(format);
  }

  std::string formatShellDate(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.dateFormat.c_str() : "%A, %x";
    return formatLocalTime(format);
  }

  std::string userHostLine() { return std::format("{}@{}", sessionUserName(), hostName()); }

  void applyHomeCardStyle(Flex& card, float scale, float /*fillOpacity*/) {
    applyContentCardStyle(card, scale);
  }

  void applyHomeClockCardStyle(Flex& card, float scale, float fillOpacity) {
    applyHomeCardStyle(card, scale, fillOpacity);
    card.setDirection(FlexDirection::Vertical);
    card.setAlign(FlexAlign::Start);
    card.setJustify(FlexJustify::Center);
    card.setGap(Style::spaceSm * scale);
  }

  Button::ButtonStateColors homeShortcutState(ColorSpec bg, ColorSpec label) {
    return Button::ButtonStateColors{
        .bg = bg,
        .border = clearColorSpec(),
        .label = label,
    };
  }

  Button::ButtonPalette homeTilePalette(bool on) {
    const auto label = on ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::OnSurface);
    const auto rest = on ? colorSpecFromRole(ColorRole::OnSurface, 0.16F) : clearColorSpec();
    const auto hover = colorSpecFromRole(ColorRole::OnSurface, on ? 0.22F : 0.12F);
    return Button::ButtonPalette{
        .borderWidth = 0.0F,
        .normal = homeShortcutState(rest, label),
        .hover = homeShortcutState(hover, label),
        .pressed = homeShortcutState(colorSpecFromRole(ColorRole::OnSurface, 0.28F), label),
        .disabled = homeShortcutState(clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface, 0.55F)),
        .selected = homeShortcutState(rest, label),
    };
  }

  Button::ButtonPalette homeStatusPalette() {
    const auto fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.84F);
    const auto label = colorSpecFromRole(ColorRole::OnSurface);
    return Button::ButtonPalette{
        .borderWidth = 0.0F,
        .normal = homeShortcutState(fill, label),
        .hover = homeShortcutState(colorSpecFromRole(ColorRole::OnSurface, 0.12F), colorSpecFromRole(ColorRole::OnSurface)),
        .pressed = homeShortcutState(colorSpecFromRole(ColorRole::OnSurface, 0.20F), label),
        .disabled = homeShortcutState(clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface, 0.55F)),
        .selected = homeShortcutState(fill, label),
    };
  }

  void applyShortcutButtonStyle(Button& button, bool enabled, bool active) {
    const bool on = enabled && active;
    const bool wasGhost = button.variant() == ButtonVariant::Ghost;
    if (!wasGhost) {
      button.setVariant(ButtonVariant::Ghost);
    }
    if (!wasGhost || button.enabled() != enabled || button.selected() != on) {
      button.setCustomPalette(homeTilePalette(on));
      button.setSoftness(1.0F);
    }
    button.setSelected(on);
    button.setEnabled(enabled);
  }

  void applyHomeStatusChipStyle(Button& button, bool emphasis) {
    if (emphasis) {
      if (button.variant() != ButtonVariant::Primary) {
        button.setVariant(ButtonVariant::Primary);
        button.setSoftness(1.0F);
      }
      return;
    }
    if (button.variant() != ButtonVariant::Ghost) {
      button.setVariant(ButtonVariant::Ghost);
    }
    button.setCustomPalette(homeStatusPalette());
    button.setSoftness(1.0F);
  }

  // Glanceable modules illuminate from within on hover. Leftover cards keep their
  // surface at rest; hover is inner light, not a stroke. The user card stays
  // border-only so the wallpaper is not covered by a fill.
  void applyHomeCardHover(Flex& card, bool hovered, bool wallpaperCard) {
    if (hovered) {
      if (wallpaperCard) {
        card.setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
        return;
      }
      card.setFill(colorSpecFromRole(ColorRole::OnSurface, 0.12F));
      if (Style::cardBordersEnabled()) {
        card.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
      } else {
        card.clearBorder();
      }
      return;
    }
    if (Style::cardBordersEnabled()) {
      card.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
    } else {
      card.clearBorder();
    }
    if (!wallpaperCard) {
      card.setFill(colorSpecFromRole(ColorRole::Surface));
    }
  }

  std::unique_ptr<Button> makeHomeTransportButton(
      Button** out, float scale, std::string_view glyph, std::string_view tooltip, bool primary,
      std::function<void()> onClick
  ) {
    const float size = (primary ? Style::controlHeight : Style::controlHeightSm) * scale;
    return ui::button({
        .out = out,
        .glyph = std::string(glyph),
        .glyphSize = (primary ? Style::fontSizeTitle : Style::fontSizeHeader) * scale,
        .contentAlign = ButtonContentAlign::Center,
        .variant = ButtonVariant::Ghost,
        .tooltip = std::string(tooltip),
        .minWidth = size,
        .minHeight = size,
        .padding = Style::spaceXs * scale,
        .radius = size * 0.5F,
        .onClick = std::move(onClick),
        .configure =
            [](Button& button) {
              button.setCustomPalette(homeTilePalette(false));
              button.setSoftness(1.0F);
            },
    });
  }

} // namespace

HomeTab::HomeTab(const ControlCenterServices& services)
    : m_mpris(services.mpris), m_audio(services.audio), m_brightness(services.brightness),
      m_session(services.session),
      m_httpClient(services.httpClient), m_weather(services.weather), m_calendar(services.calendar),
      m_config(services.config), m_accounts(services.accounts), m_wallpaper(services.wallpaper),
      m_thumbnails(services.thumbnails), m_asyncTextures(services.asyncTextures), m_upower(services.upower),
      m_services(services.shortcutServices()),
      m_settings(std::make_unique<SettingsPane>(
          services,
          SettingsPaneOptions{
              .section = "control-center",
              .includedGroups = {"home"},
              .showConfigureButton = false,
          }
      )) {
  if (m_thumbnails != nullptr) {
    m_thumbnailPendingSub = m_thumbnails->subscribePendingUpload([this]() {
      if (m_wallpaperBg == nullptr) {
        return;
      }
      PanelManager::instance().requestUpdateOnly();
    });
  }

  // Pre-warm the wallpaper preview thumbnail as soon as the wallpaper changes,
  // even while the control center is closed, so it is already decoded by the
  // time the panel opens. Uses the last preview size the home card requested.
  if (m_wallpaper != nullptr) {
    m_wallpaperChangedConn = m_wallpaper->changed().connect([this]() {
      if (m_thumbnails != nullptr && m_loadedWallpaperSize > 0) {
        ensureWallpaperThumbnail(m_wallpaper->currentPath(), m_loadedWallpaperSize);
      }
      if (m_wallpaperBg != nullptr) {
        PanelManager::instance().requestUpdateOnly();
      }
    });
  }
}

HomeTab::~HomeTab() {
  if (m_thumbnails != nullptr && !m_loadedWallpaperPath.empty()) {
    m_thumbnails->release(m_loadedWallpaperPath, m_loadedWallpaperSize);
  }
}

void HomeTab::buildQuickSurface(Flex& tab, float scale) {
  for (auto& row : m_quickToggles) {
    if (row.shortcut != nullptr) {
      row.shortcut->onPanelClose();
    }
  }
  m_quickToggles.clear();
  if (m_volumeShortcut != nullptr) {
    m_volumeShortcut->onPanelClose();
    m_volumeShortcut.reset();
  }

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

  m_volumeShortcut = ShortcutRegistry::create("audio", m_services);
  if (m_volumeShortcut != nullptr) {
    m_volumeShortcut->onPanelOpen();
  }
  auto volumeRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  volumeRow->addChild(
      ui::button({
          .out = &m_volumeButton,
          .glyph = m_volumeShortcut != nullptr ? m_volumeShortcut->displayIcon() : std::string("volume-high"),
          .tooltip = i18n::tr("control-center.shortcuts.audio"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this]() {
                if (m_volumeShortcut != nullptr) {
                  m_volumeShortcut->onClick();
                }
                PanelManager::instance().refresh();
              },
          .onRightClick = [this]() { openDetails("sound", "audio", m_services.platform); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  volumeRow->addChild(ui::label({
      .text = i18n::tr("control-center.tabs.audio"),
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .minWidth = Style::controlHeightLg * 2.0F * scale,
      .maxWidth = Style::controlHeightLg * 2.0F * scale,
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));
  volumeRow->addChild(
      ui::slider({
          .out = &m_volumeSlider,
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
                if (m_syncingQuickSliders || m_audio == nullptr) {
                  return;
                }
                m_audio->setVolume(static_cast<float>(value));
                if (m_volumeValue != nullptr) {
                  m_volumeValue->setText(std::format("{:.0F}%", value * 100.0));
                }
              },
      })
  );
  volumeRow->addChild(
      ui::label({
          .out = &m_volumeValue,
          .text = "0%",
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .minWidth = 48.0F * scale,
          .textAlign = TextAlign::End,
      })
  );
  well->addChild(std::move(volumeRow));

  auto brightness = ui::column({
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
  brightnessRow->addChild(
      ui::button({
          .glyph = "brightness-high",
          .tooltip = i18n::tr("control-center.display.brightness"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = []() { openControlCenterTab("monitor"); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  brightnessRow->addChild(ui::label({
      .text = i18n::tr("control-center.display.brightness"),
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .minWidth = Style::controlHeightLg * 2.0F * scale,
      .maxWidth = Style::controlHeightLg * 2.0F * scale,
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));
  brightnessRow->addChild(
      ui::slider({
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
                if (m_syncingQuickSliders || m_brightness == nullptr) {
                  return;
                }
                m_brightness->setAllBrightness(static_cast<float>(value));
                if (m_brightnessValue != nullptr) {
                  m_brightnessValue->setText(std::format("{:.0F}%", value * 100.0));
                }
              },
      })
  );
  brightnessRow->addChild(
      ui::label({
          .out = &m_brightnessValue,
          .text = "100%",
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .minWidth = 48.0F * scale,
          .textAlign = TextAlign::End,
      })
  );
  brightness->addChild(std::move(brightnessRow));
  well->addChild(std::move(brightness));
  tab.addChild(std::move(well));

  auto list = ui::column({
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .configure = [scale](Flex& host) { applyGroupedListStyle(host, scale); },
  });
  const auto addToggle = [&](std::string_view type, std::string openTab, std::string tooltipKey) {
    auto shortcut = ShortcutRegistry::create(type, m_services);
    if (shortcut == nullptr) {
      return;
    }
    shortcut->onPanelOpen();
    if (!m_quickToggles.empty()) {
      list->addChild(ui::separator());
    }
    Button* button = nullptr;
    Label* status = nullptr;
    Shortcut* const shortcutPtr = shortcut.get();
    const std::string tabId = openTab;
    const std::string settingsPage = tabId == "bluetooth" ? "bluetooth" : "network";
    const bool nativeDetails = tabId == "network" || tabId == "bluetooth" || tabId == "tailscale";
    const auto showDetails = [this, tabId, settingsPage, nativeDetails]() {
      if (nativeDetails) openDetails(settingsPage, tabId, m_services.platform);
      else openControlCenterTab(tabId);
    };
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceXs * scale,
        .fillWidth = true,
    });
    row->addChild(ui::button({
        .out = &button,
        .glyph = shortcut->displayIcon(),
        .glyphSize = Style::fontSizeHeader * scale,
        .tooltip = shortcut->defaultLabel(),
        .minWidth = Style::controlHeight * scale,
        .minHeight = Style::controlHeight * scale,
        .radius = Style::scaledRadiusXl(scale),
        .onClick = [shortcutPtr]() {
          shortcutPtr->onClick();
          PanelManager::instance().refresh();
        },
        .onRightClick = showDetails,
    }));
    auto names = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * 0.5F * scale,
        .flexGrow = 1.0F,
    });
    names->addChild(ui::label({
        .text = type == "notification" ? i18n::tr("control-center.notifications.dnd") : shortcut->defaultLabel(),
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    }));
    names->addChild(ui::label({
        .out = &status,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    }));
    row->addChild(std::move(names));
    row->addChild(ui::button({
        .glyph = Style::rtl() ? "chevron-left" : "chevron-right",
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr(tooltipKey),
        .minWidth = Style::controlHeight * scale,
        .minHeight = Style::controlHeight * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = showDetails,
    }));
    list->addChild(std::move(row));
    m_quickToggles.push_back({
        .shortcut = std::move(shortcut),
        .button = button,
        .status = status,
        .openTab = std::move(openTab),
    });
  };
  addToggle("wifi", "network", "control-center.tabs.network");
  addToggle("bluetooth", "bluetooth", "control-center.tabs.bluetooth");
  if (m_services.tailscale != nullptr && (m_services.tailscale->installed() || m_services.tailscale->running())) {
    addToggle("tailscale", "tailscale", "control-center.tabs.tailscale");
  }
  addToggle("nightlight", "monitor", "control-center.tabs.monitor");
  addToggle("notification", "notifications", "control-center.tabs.notifications");
  tab.addChild(std::move(list));

  auto destinations = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .configure = [scale](Flex& host) {
        applyModuleWellStyle(host, scale);
        host.setGap(Style::spaceSm * scale);
        host.setPadding(Style::spaceSm * scale);
      },
  });
  const auto destinationButton = [scale](std::string_view tabId, std::string label, std::string glyph) {
    return ui::button({
        .text = label,
        .glyph = std::move(glyph),
        .fontSize = Style::fontSizeBody * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = label,
        .minHeight = Style::controlHeight * scale,
        .flexGrow = 1.0F,
        .onClick = [tab = std::string(tabId)]() { openControlCenterTab(tab); },
    });
  };
  destinations->addChild(destinationButton("capture", i18n::tr("control-center.tabs.capture"), "screenshot"));
  if (m_services.phone != nullptr) {
    destinations->addChild(destinationButton("phone", i18n::tr("control-center.tabs.phone"), "device-mobile"));
  }
  tab.addChild(std::move(destinations));

  const float actionSize = Style::controlHeight * scale;
  const auto sessionButton = [scale, actionSize](Button** out, std::string glyph, std::string tooltip,
                                                  ButtonVariant variant, std::function<void()> onClick) {
    return ui::button({
        .out = out,
        .text = tooltip,
        .glyph = std::move(glyph),
        .fontSize = Style::fontSizeBody * scale,
        .glyphSize = Style::fontSizeHeader * scale,
        .variant = variant,
        .tooltip = std::move(tooltip),
        .minWidth = actionSize,
        .minHeight = actionSize,
        .flexGrow = 1.0F,
        .onClick = std::move(onClick),
        .configure =
            [](Button& button) {
              button.setSoftness(1.0F);
              button.setJustify(FlexJustify::Center);
            },
    });
  };
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
      &m_lockButton, std::string(session_action::defaultGlyph("lock")), i18n::tr("session.actions.lock"),
      ButtonVariant::Ghost,
      [this]() {
        PanelManager::instance().close();
        if (m_session != nullptr) {
          (void)m_session->lock();
        }
      }
  ));
  session->addChild(sessionButton(
      &m_suspendButton, std::string(session_action::defaultGlyph("suspend")), i18n::tr("session.actions.suspend"),
      ButtonVariant::Ghost, [this]() { runSessionAction("suspend"); }
  ));
  session->addChild(sessionButton(
      nullptr, "power", i18n::tr("control-center.tabs.power"), ButtonVariant::Ghost,
      []() { openControlCenterTab("power"); }
  ));
  tab.addChild(std::move(session));
}

void HomeTab::buildPinList(Flex& tab, float scale) {
  const auto& shortcuts =
      m_config != nullptr ? m_config->config().controlCenter.shortcuts : std::vector<ShortcutConfig>{};
  std::vector<std::unique_ptr<Shortcut>> previous;
  previous.reserve(m_shortcutPads.size());
  for (auto& pad : m_shortcutPads) {
    if (pad.shortcut != nullptr) {
      pad.shortcut->onPanelClose();
    }
    previous.push_back(std::move(pad.shortcut));
  }
  m_shortcutPads.clear();
  m_shortcutsColumn = nullptr;
  if (shortcuts.empty()) {
    return;
  }

  const bool pluginsChanged = m_config != nullptr && !(m_config->config().plugins == m_lastPlugins);
  if (m_config != nullptr) {
    m_lastPlugins = m_config->config().plugins;
  }
  auto takeReusable = [&previous, pluginsChanged](std::string_view type) -> std::unique_ptr<Shortcut> {
    for (auto it = previous.begin(); it != previous.end(); ++it) {
      if (*it == nullptr || (*it)->id() != type) {
        continue;
      }
      if (pluginsChanged && scripting::isPluginEntryOfKind(type, scripting::PluginEntryKind::Shortcut)) {
        continue;
      }
      auto found = std::move(*it);
      previous.erase(it);
      return found;
    }
    return nullptr;
  };

  auto column = ui::column({
      .out = &m_shortcutsColumn,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .configure = [scale](Flex& host) { applyGroupedListStyle(host, scale); },
  });
  bool first = true;
  for (const auto& sc : shortcuts) {
    auto shortcut = takeReusable(sc.type);
    const bool reused = shortcut != nullptr;
    if (!reused) {
      shortcut = ShortcutRegistry::create(sc.type, m_services);
    }
    if (shortcut == nullptr) {
      continue;
    }
    shortcut->onPanelOpen();
    if (!first) {
      column->addChild(ui::separator());
    }
    first = false;
    Button* button = nullptr;
    Shortcut* const shortcutPtr = shortcut.get();
    column->addChild(
        ui::button({
            .out = &button,
            .text = shortcut->displayLabel(),
            .glyph = shortcut->displayIcon(),
            .glyphSize = Style::fontSizeHeader * scale,
            .contentAlign = ButtonContentAlign::Start,
            .tooltip = shortcut->displayLabel(),
            .minHeight = Style::controlHeightLg * scale,
            .paddingV = Style::spaceSm * scale,
            .paddingH = Style::spaceMd * scale,
            .gap = Style::spaceSm * scale,
            .radius = 0.0F,
            .onClick =
                [shortcutPtr]() {
                  if (shortcutPtr != nullptr) {
                    shortcutPtr->onClick();
                  }
                  PanelManager::instance().refresh();
                },
            .onRightClick =
                [shortcutPtr]() {
                  if (shortcutPtr != nullptr) {
                    shortcutPtr->onRightClick();
                  }
                },
            .configure =
                [scale](Button& control) {
                  control.setDirection(FlexDirection::Horizontal);
                  control.setAlign(FlexAlign::Center);
                  control.setJustify(FlexJustify::Start);
                  control.setFillWidth(true);
                  if (control.label() != nullptr) {
                    control.label()->setFontSize(Style::fontSizeBody * scale);
                    control.label()->setMaxLines(1);
                    control.label()->setTextAlign(TextAlign::Start);
                    control.label()->setEllipsize(TextEllipsize::End);
                  }
                },
        })
    );
    if (auto* area = button != nullptr ? button->inputArea() : nullptr; area != nullptr) {
      area->setOnAxisHandler([shortcutPtr](const InputArea::PointerData& data) -> bool {
        if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || shortcutPtr == nullptr) {
          return false;
        }
        const float steps = data.scrollSteps();
        if (steps == 0.0F) {
          return false;
        }
        return shortcutPtr->onScroll(steps > 0.0F ? -1 : 1);
      });
    }
    ShortcutPad pad;
    pad.shortcut = std::move(shortcut);
    pad.type = sc.type;
    pad.button = button;
    pad.glyph = button != nullptr ? button->glyph() : nullptr;
    pad.label = button != nullptr ? button->label() : nullptr;
    m_shortcutPads.push_back(std::move(pad));
  }
  if (m_shortcutPads.empty()) {
    m_shortcutsColumn = nullptr;
    return;
  }
  tab.addChild(std::move(column));
}

void HomeTab::runSessionAction(std::string_view action) {
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

std::unique_ptr<Flex> HomeTab::create() {
  const float scale = contentScale();
  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
  });

  buildQuickSurface(*tab, scale);
  buildPinList(*tab, scale);

  // --- Now Playing: only participates when a player is active ---
  auto mediaCard = ui::column({
      .out = &m_mediaCard,
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
      .fillHeight = false,
      .flexGrow = 0.0F,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applyHomeCardStyle(card, scale, opacity); },
  });

  const float artSize = Style::controlHeightLg * 1.6F * scale;
  auto mediaText = ui::column({
      .out = &m_mediaText,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * 0.5F * scale,
      .flexGrow = 1.0F,
  });
  mediaText->addChild(ui::label({
      .out = &m_mediaTrack,
      .text = i18n::tr("control-center.home.media.nothing-playing"),
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));
  mediaText->addChild(ui::label({
      .out = &m_mediaArtist,
      .text = "",
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
      .visible = false,
  }));
  mediaText->addChild(ui::progressBar({
      .out = &m_mediaProgressBar,
      .fill = colorSpecFromRole(ColorRole::Primary),
      .track = colorSpecFromRole(ColorRole::OnSurface, 0.12F),
      .radius = 2.0F * scale,
      .softness = 1.0F,
      .progress = 0.0F,
      .height = 4.0F * scale,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](ProgressBar& bar) { bar.setHitTestVisible(false); },
  }));
  auto transport = ui::row({
      .out = &m_mediaTransport,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Flex& row) { row.setZIndex(5); },
  });
  transport->addChild(makeHomeTransportButton(
      &m_mediaPrev, scale, "media-prev", i18n::tr("control-center.media.previous"), false,
      [this]() {
        const std::weak_ptr<void> aliveGuard = m_aliveGuard;
        DeferredCall::callLater([this, aliveGuard]() {
          if (aliveGuard.expired() || m_mpris == nullptr) {
            return;
          }
          (void)m_mpris->previousActive();
          PanelManager::instance().refresh();
        });
      }
  ));
  transport->addChild(makeHomeTransportButton(
      &m_mediaPlayPause, scale, "media-play", i18n::tr("control-center.media.play"), true,
      [this]() {
        const std::weak_ptr<void> aliveGuard = m_aliveGuard;
        DeferredCall::callLater([this, aliveGuard]() {
          if (aliveGuard.expired() || m_mpris == nullptr) {
            return;
          }
          (void)m_mpris->playPauseActive();
          PanelManager::instance().refresh();
        });
      }
  ));
  transport->addChild(makeHomeTransportButton(
      &m_mediaNext, scale, "media-next", i18n::tr("control-center.media.next"), false,
      [this]() {
        const std::weak_ptr<void> aliveGuard = m_aliveGuard;
        DeferredCall::callLater([this, aliveGuard]() {
          if (aliveGuard.expired() || m_mpris == nullptr) {
            return;
          }
          (void)m_mpris->nextActive();
          PanelManager::instance().refresh();
        });
      }
  ));
  mediaText->addChild(std::move(transport));

  auto mediaContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
      ui::column(
          {.out = &m_mediaArtSlot,
           .align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .width = artSize,
           .height = artSize},
          ui::glyph({
              .out = &m_mediaArtFallback,
              .glyph = "disc",
              .glyphSize = artSize * 0.55F,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::image({
              .out = &m_mediaArt,
              .fit = ImageFit::Cover,
              .radius = homeMediaArtRadius(scale),
              .width = artSize,
              .height = artSize,
              .participatesInLayout = false,
              .configure = [](Image& image) { image.setZIndex(1); },
          })
      ),
      std::move(mediaText)
  );
  mediaCard->addChild(std::move(mediaContent));

  // Clicking anywhere on the media card opens the media tab. Transport sits above this overlay.
  m_mediaCardArea = addCardOverlay(*m_mediaCard, []() { openControlCenterTab("media"); });
  if (m_mediaCardArea != nullptr) {
    m_mediaCardArea->setTooltip(i18n::tr("control-center.home.open-media"));
  }

  tab->addChild(std::move(mediaCard));

  auto liveScroll = ui::scrollView({
      .out = &m_liveScroll,
      .state = &m_liveScrollState,
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
  if (Flex* content = liveScroll->content(); content != nullptr) {
    content->setDirection(FlexDirection::Vertical);
    content->setAlign(FlexAlign::Stretch);
    content->addChild(std::move(tab));
  }
  return m_settings->wrapLive(std::move(liveScroll), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> HomeTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "settings",
          .tooltip = i18n::tr("control-center.settings.configure"),
          .onClick =
              []() {
                PanelManager::instance().togglePanel(
                    "control-center", PanelOpenRequest{.context = "settings:control-center"}
                );
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  syncHeaderActions();
  return live;
}

void HomeTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool HomeTab::configureMode() const { return m_settings->configureMode(); }

void HomeTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

void HomeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }

  if (m_mediaCard != nullptr) {
    m_mediaCard->setMinHeight(0.0F);
    m_mediaCard->setMaxHeight(0.0F);
  }
  if (m_dateTimeCard != nullptr) {
    m_dateTimeCard->setMinHeight(0.0F);
    m_dateTimeCard->setMaxHeight(0.0F);
  }
  if (m_userAvatar != nullptr && m_userMain != nullptr) {
    const float userMainHeight = std::max(1.0F, m_userAvatar->height());
    m_userMain->setMinHeight(userMainHeight);
    m_userMain->setSize(m_userMain->width(), userMainHeight);
  }

  // Chips must be shown before this pass measures the column, otherwise the
  // wallpaper card eats the leftover and the strip paints on top of it.
  syncStatusStrip();

  const float scale = contentScale();
  const bool playing = mediaExpanded();
  const bool mediaWasShown = m_mediaCard != nullptr && m_mediaCard->visible();
  setShown(m_mediaCard, playing);
  if (mediaWasShown != playing && PanelManager::instance().isOpenPanel("control-center")) {
    PanelManager::instance().relayoutActivePanelPreferredSize();
  }

  const float userCardHeight = homeAvatarSize(scale) + 2.0F * Style::spaceMd * scale;
  const bool statusVisible = m_statusStrip != nullptr && m_statusStrip->visible();
  if (m_statusStrip != nullptr) {
    m_statusStrip->setParticipatesInLayout(statusVisible);
    m_statusStrip->setFlexGrow(0.0F);
    m_statusStrip->setMinHeight(0.0F);
    m_statusStrip->setMaxHeight(0.0F);
  }
  if (m_userCard != nullptr) {
    m_userCard->setFlexGrow(0.0F);
    m_userCard->setFillHeight(false);
    m_userCard->setMinHeight(userCardHeight);
    m_userCard->setMaxHeight(userCardHeight);
  }

  const float mediaH = playing ? homeNowPlayingHeight(scale) : 0.0F;
  if (m_mediaCard != nullptr) {
    m_mediaCard->setFlexGrow(0.0F);
    m_mediaCard->setFillHeight(false);
    m_mediaCard->setMinHeight(mediaH);
    m_mediaCard->setMaxHeight(mediaH);
  }

  const bool eventVisible = m_eventRow != nullptr && m_eventRow->visible();
  const float clockMin = homeClockMinHeight(scale, eventVisible);
  if (m_dateTimeCard != nullptr) {
    m_dateTimeCard->setFlexGrow(1.0F);
    m_dateTimeCard->setFillHeight(true);
    m_dateTimeCard->setMinHeight(clockMin);
    m_dateTimeCard->setMaxHeight(0.0F);
  }

  if (m_shortcutsColumn != nullptr) {
    m_shortcutsColumn->setFlexGrow(0.0F);
    m_shortcutsColumn->setFillHeight(false);
  }

  m_rootLayout->setMinHeight(0.0F);
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  // Cap pin labels to the row's content width after the list is sized.
  if (!m_shortcutPads.empty()) {
    for (auto& pad : m_shortcutPads) {
      if (pad.label == nullptr) {
        continue;
      }
      float inner = 1.0F;
      if (pad.button != nullptr && pad.button->width() > 1.0F) {
        const float glyphW = pad.glyph != nullptr ? pad.glyph->width() : 0.0F;
        inner = std::max(
            1.0F,
            pad.button->width() - pad.button->paddingLeft() - pad.button->paddingRight() - glyphW
                - pad.button->gap()
        );
      }
      pad.label->setMaxWidth(inner);
    }
  }

  const auto innerWidth = [](Flex* card) {
    if (card == nullptr) {
      return 1.0F;
    }
    return std::max(1.0F, card->width() - (card->paddingLeft() + card->paddingRight()));
  };
  const float dateTimeWrap = innerWidth(m_dateTimeCard);
  if (m_timeLabel != nullptr) {
    m_timeLabel->setMaxWidth(dateTimeWrap);
    m_timeLabel->setMaxLines(1);
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setMaxWidth(dateTimeWrap);
    m_dateLabel->setMaxLines(1);
  }
  const float weatherMetaWrap = std::max(
      1.0F,
      dateTimeWrap
          - (m_weatherGlyph != nullptr ? m_weatherGlyph->width() : 0.0F)
          - Style::spaceSm * scale
  );
  if (m_weatherLine != nullptr) {
    m_weatherLine->setMaxWidth(weatherMetaWrap);
    m_weatherLine->setMaxLines(1);
    m_weatherLine->setEllipsize(TextEllipsize::End);
  }
  const float eventMetaWrap = std::max(
      1.0F,
      dateTimeWrap
          - (m_eventGlyph != nullptr ? m_eventGlyph->width() : 0.0F)
          - Style::spaceSm * scale
  );
  if (m_eventLine != nullptr) {
    m_eventLine->setMaxWidth(eventMetaWrap);
    m_eventLine->setMaxLines(1);
    m_eventLine->setEllipsize(TextEllipsize::End);
  }
  // Grow the album art square to the Now Playing card height so transport stays balanced.
  resizeMediaArtToCard();

  // Keep the media card height stable: one ellipsized line each, capped to the
  // text column width so a long title does not grow Now Playing.
  const float mediaTextWrap = m_mediaText != nullptr ? innerWidth(m_mediaText) : 1.0F;
  for (Label* label : {m_mediaTrack, m_mediaArtist}) {
    if (label != nullptr) {
      label->setMaxWidth(mediaTextWrap);
      label->setMaxLines(1);
      label->setEllipsize(TextEllipsize::End);
    }
  }

  if (m_userCard != nullptr) {
    const float userWrap = innerWidth(m_userCard);
    for (Label* label : {m_userHost, m_userUptime}) {
      if (label != nullptr) {
        label->setMaxWidth(userWrap);
        label->setMaxLines(1);
      }
    }
  }

  if (m_userAvatar != nullptr && m_userMain != nullptr) {
    const float minAvatar = homeAvatarSize(scale);
    const float desiredAvatar = std::max(minAvatar, m_userMain->height());
    if (std::abs(m_userAvatar->width() - desiredAvatar) > 0.5F) {
      m_userAvatar->setSize(desiredAvatar, desiredAvatar);
      m_userAvatar->setRadius(desiredAvatar * 0.5F);
      m_userAvatar->setPadding(1.0F * scale);
    }
    m_userMain->setMinHeight(desiredAvatar);
    m_userMain->setSize(m_userMain->width(), desiredAvatar);
  }

  // Pins keep intrinsic row height; clock takes leftover. If identity + clock min + pins
  // overflow the panel, the live scroll grows.
  float statusHeight = 0.0F;
  if (statusVisible && m_statusStrip != nullptr) {
    const float oneRow = Style::controlHeightSm * scale + 2.0F * Style::spaceXs * scale;
    statusHeight = std::max(m_statusStrip->height(), oneRow);
    m_statusStrip->setMinHeight(statusHeight);
    m_statusStrip->setMaxHeight(statusHeight);
  }

  float contentH = bodyHeight;
  float pinsH = 0.0F;
  if (m_shortcutsColumn != nullptr && !m_shortcutPads.empty()) {
    pinsH = m_shortcutsColumn->height();
    if (pinsH <= 1.0F) {
      pinsH = static_cast<float>(m_shortcutPads.size()) * Style::controlHeightLg * scale;
    }
    m_shortcutsColumn->setMinHeight(pinsH);
    m_shortcutsColumn->setMaxHeight(pinsH);
  }

  float naturalH = userCardHeight;
  if (statusVisible) {
    naturalH += statusHeight;
  }
  if (playing) {
    naturalH += mediaH;
  }
  naturalH += clockMin;
  naturalH += pinsH;
  float parts = 2.0F;
  if (statusVisible) {
    parts += 1.0F;
  }
  if (playing) {
    parts += 1.0F;
  }
  if (pinsH > 0.0F) {
    parts += 1.0F;
  }
  naturalH += (parts - 1.0F) * m_rootLayout->gap();
  contentH = std::max(bodyHeight, naturalH);
  m_rootLayout->setMinHeight(contentH);
  m_rootLayout->setSize(contentWidth, contentH);

  if (m_timeLabel != nullptr && m_dateTimeCard != nullptr) {
    const float clockH = std::max(m_dateTimeCard->minHeight(), m_dateTimeCard->height());
    const float innerH =
        std::max(0.0F, clockH - m_dateTimeCard->paddingTop() - m_dateTimeCard->paddingBottom());
    float size = homeTimeFontSize(scale, innerH, eventVisible);
    m_timeLabel->setFontSize(size);
    m_timeLabel->measure(renderer);
    const float wrap = innerWidth(m_dateTimeCard);
    if (wrap > 1.0F && m_timeLabel->width() > wrap) {
      const float minSize = Style::fontSizeHeader * 1.75F * scale;
      size = std::max(minSize, size * (wrap / m_timeLabel->width()));
      m_timeLabel->setFontSize(size);
    }
  }

  bool artSizeChanged = false;
  for (int pass = 0; pass < kHomeMediaArtLayoutPassLimit; ++pass) {
    m_rootLayout->layout(renderer);
    artSizeChanged = resizeMediaArtToCard();
    if (!artSizeChanged) {
      break;
    }
  }
  if (artSizeChanged) {
    // Keep the final tree consistent even if an unusual layout combination hits the pass cap.
    m_rootLayout->layout(renderer);
  }
  if (m_liveScroll != nullptr) {
    m_liveScroll->setSize(contentWidth, bodyHeight);
    m_liveScroll->layout(renderer);
  }
  layoutWallpaperBackground(renderer);
  layoutCardOverlays();
  if (m_weatherGlyph != nullptr) {
    m_weatherGlyph->measure(renderer);
  }
  if (m_eventGlyph != nullptr) {
    m_eventGlyph->measure(renderer);
  }
  // Flex layout just reset every pin to its rest slot; a lifted pin keeps its drag offset.
  if (m_pinDrag.active) {
    applyPinDragVisuals();
  }
}

// ── Pin drag-to-reorder ─────────────────────────────────────────────────────────

void HomeTab::onPinPress(std::size_t index, float localY, bool pressed) {
  if (index >= m_shortcutPads.size() || m_shortcutPads[index].button == nullptr) {
    return;
  }
  Button* button = m_shortcutPads[index].button;
  if (pressed) {
    // Any new press clears leftovers, including a suppressClick whose click never came
    // because the drag was released outside the pin.
    m_pinDrag = {};
    const auto* ia = button->inputArea();
    if (ia == nullptr || ia->pressedButton() != BTN_LEFT) {
      return;
    }
    m_pinDrag.pressed = true;
    m_pinDrag.source = index;
    m_pinDrag.target = index;
    m_pinDrag.startPointerY = button->y() + localY;
    m_pinDrag.pointerY = m_pinDrag.startPointerY;
    return;
  }
  if (!m_pinDrag.pressed || m_pinDrag.source != index) {
    return;
  }
  const bool wasActive = m_pinDrag.active;
  m_pinDrag.pressed = false;
  if (!wasActive) {
    m_pinDrag = {};
    return;
  }
  // The click that follows this release belongs to the drag, not the shortcut.
  m_pinDrag.suppressClick = true;
  m_pinDrag.active = false;
  clearPinDragVisuals();
  commitPinDrag();
}

void HomeTab::onPinMotion(std::size_t index, float localY) {
  if (!m_pinDrag.pressed || m_pinDrag.source != index || index >= m_shortcutPads.size()) {
    return;
  }
  Button* button = m_shortcutPads[index].button;
  if (button == nullptr) {
    return;
  }
  // `button->y()` already carries the drag offset, so pointer stays in column coordinates.
  m_pinDrag.pointerY = button->y() + localY;
  if (!m_pinDrag.active) {
    if (std::fabs(m_pinDrag.pointerY - m_pinDrag.startPointerY) < Style::dragStartThreshold * contentScale()) {
      return;
    }
    liftPin();
  }
  const std::size_t slot = pinDropSlot();
  if (slot != m_pinDrag.target) {
    m_pinDrag.target = slot;
  }
  applyPinDragVisuals();
  PanelManager::instance().requestRedraw();
}

void HomeTab::liftPin() {
  m_pinDrag.active = true;
  m_pinDrag.restY.clear();
  m_pinDrag.restY.reserve(m_shortcutPads.size());
  for (const auto& pad : m_shortcutPads) {
    m_pinDrag.restY.push_back(pad.button != nullptr ? pad.button->y() : 0.0F);
  }
  TooltipManager::instance().onHoverChange(nullptr, static_cast<zwlr_layer_surface_v1*>(nullptr), nullptr);
}

std::size_t HomeTab::pinDropSlot() const {
  const std::size_t count = m_shortcutPads.size();
  if (!m_pinDrag.active || m_pinDrag.restY.size() != count || count == 0) {
    return m_pinDrag.source;
  }
  std::vector<float> heights;
  heights.reserve(count);
  for (const auto& pad : m_shortcutPads) {
    heights.push_back(pad.button != nullptr ? pad.button->height() : 0.0F);
  }
  return shell::control_center::pin_reorder::dropSlot(
      m_pinDrag.restY, heights, m_pinDrag.source, m_pinDrag.pointerY - m_pinDrag.startPointerY
  );
}

void HomeTab::applyPinDragVisuals() {
  const std::size_t count = m_shortcutPads.size();
  if (!m_pinDrag.active || m_shortcutsColumn == nullptr || m_pinDrag.restY.size() != count) {
    return;
  }
  const std::size_t source = m_pinDrag.source;
  const std::size_t target = m_pinDrag.target;
  Button* dragged = m_shortcutPads[source].button;
  if (dragged == nullptr) {
    return;
  }
  const float h = dragged->height();
  const float columnH = m_shortcutsColumn->height();
  float offset = m_pinDrag.pointerY - m_pinDrag.startPointerY;
  const float minY = 0.0F;
  const float maxY = std::max(0.0F, columnH - h);
  offset = std::clamp(m_pinDrag.restY[source] + offset, minY, maxY) - m_pinDrag.restY[source];

  for (std::size_t i = 0; i < count; ++i) {
    Button* button = m_shortcutPads[i].button;
    if (button == nullptr) {
      continue;
    }
    float y = m_pinDrag.restY[i];
    if (i == source) {
      y += offset;
      button->setZIndex(200);
      button->setOpacity(0.88F);
    } else {
      // Neighbours between the lifted pin's old and new slots slide by one row.
      y += h * static_cast<float>(shell::control_center::pin_reorder::rowShift(i, source, target));
      button->setZIndex(0);
      button->setOpacity(1.0F);
    }
    button->setPosition(button->x(), y);
  }
}

void HomeTab::clearPinDragVisuals() {
  const std::size_t count = m_shortcutPads.size();
  for (std::size_t i = 0; i < count; ++i) {
    Button* button = m_shortcutPads[i].button;
    if (button == nullptr) {
      continue;
    }
    button->setZIndex(0);
    button->setOpacity(1.0F);
    if (i < m_pinDrag.restY.size()) {
      button->setPosition(button->x(), m_pinDrag.restY[i]);
    }
  }
  m_pinDrag.restY.clear();
  PanelManager::instance().requestRedraw();
}

void HomeTab::commitPinDrag() {
  if (m_config == nullptr) {
    return;
  }
  // Pads only include shortcuts that ShortcutRegistry could create; reorder by config type so
  // an unknown type in config keeps its place.
  std::vector<std::string> padIds;
  padIds.reserve(m_shortcutPads.size());
  for (const auto& pad : m_shortcutPads) {
    padIds.push_back(pad.type);
  }
  auto next = shell::control_center::pin_reorder::reorder(
      m_config->config().controlCenter.shortcuts, padIds, m_pinDrag.source, m_pinDrag.target
  );
  if (!next.has_value()) {
    return;
  }
  ConfigService* config = m_config;
  // Defer past the release dispatch that called us: moving the row re-parents the node whose
  // InputArea is still mid-dispatch, and the override write reloads config.
  DeferredCall::callLater([this, alive = std::weak_ptr<void>(m_aliveGuard), config, source = m_pinDrag.source,
                           slot = m_pinDrag.target, order = std::move(*next)]() mutable {
    if (!alive.expired()) {
      movePinInPlace(source, slot);
    }
    if (config != nullptr) {
      (void)config->setOverride({"control_center", "shortcuts"}, std::move(order));
    }
  });
}

void HomeTab::movePinInPlace(std::size_t source, std::size_t slot) {
  const std::size_t count = m_shortcutPads.size();
  if (m_shortcutsColumn == nullptr || source >= count || slot >= count || slot == source) {
    return;
  }
  Button* button = m_shortcutPads[source].button;
  if (button == nullptr) {
    return;
  }
  // The column owns non-pad children too (its background box), so place the row relative to
  // the pad that currently occupies the target slot rather than by pad index.
  const auto childIndexOf = [this](const Node* target) -> std::optional<std::size_t> {
    const auto& kids = m_shortcutsColumn->children();
    for (std::size_t i = 0; i < kids.size(); ++i) {
      if (kids[i].get() == target) {
        return i;
      }
    }
    return std::nullopt;
  };
  const std::size_t anchorPad = slot < source ? slot : slot + 1; // pad the row lands in front of
  std::optional<std::size_t> insertAt;
  if (anchorPad < count) {
    insertAt = childIndexOf(m_shortcutPads[anchorPad].button);
  } else if (const auto last = childIndexOf(m_shortcutPads[count - 1].button); last.has_value()) {
    insertAt = *last + 1;
  }
  const auto sourceChild = childIndexOf(button);
  if (!insertAt.has_value() || !sourceChild.has_value()) {
    return;
  }
  if (*sourceChild < *insertAt) {
    --*insertAt; // removing the row first shifts everything below it up by one
  }
  auto node = m_shortcutsColumn->removeChild(button);
  if (node == nullptr) {
    return;
  }
  m_shortcutsColumn->insertChildAt(*insertAt, std::move(node));
  ShortcutPad pad = std::move(m_shortcutPads[source]);
  m_shortcutPads.erase(m_shortcutPads.begin() + static_cast<std::ptrdiff_t>(source));
  m_shortcutPads.insert(m_shortcutPads.begin() + static_cast<std::ptrdiff_t>(slot), std::move(pad));
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestRedraw();
}

std::size_t HomeTab::padIndexForButton(const Button* button) const noexcept {
  for (std::size_t i = 0; i < m_shortcutPads.size(); ++i) {
    if (m_shortcutPads[i].button == button) {
      return i;
    }
  }
  return m_shortcutPads.size();
}

ShortcutPad* HomeTab::padForButton(const Button* button) noexcept {
  const std::size_t index = padIndexForButton(button);
  return index < m_shortcutPads.size() ? &m_shortcutPads[index] : nullptr;
}

InputArea* HomeTab::addCardOverlay(Flex& card, std::function<void()> onActivate) {
  return addCardOverlay(card, std::move(onActivate), CardOverlayOptions{});
}

InputArea* HomeTab::addCardOverlay(Flex& card, std::function<void()> onActivate, CardOverlayOptions options) {
  auto area = ui::inputArea({});
  area->setParticipatesInLayout(false);
  area->setZIndex(3);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  if (!options.pointerHitTest) {
    area->setHitTestVisible(false);
  }
  if (options.keyboardFocus) {
    area->setFocusable(true);
  } else {
    area->setFocusable(false);
    area->setTabStop(false);
  }

  Flex* cardPtr = &card;
  InputArea* areaPtr = area.get();
  std::function<void()> activate = std::move(onActivate);

  const auto setHovered = [this, cardPtr](bool hovered) {
    applyHomeCardHover(*cardPtr, hovered, cardPtr == m_userCard);
    PanelManager::instance().requestRedraw();
  };

  if (options.pointerHitTest) {
    area->setOnEnter([setHovered](const InputArea::PointerData&) { setHovered(true); });
    area->setOnLeave([setHovered, areaPtr]() {
      if (areaPtr->focused()) {
        return;
      }
      setHovered(false);
    });
    area->setOnClick([activate](const InputArea::PointerData&) { activate(); });
  }
  if (options.keyboardFocus) {
    area->setOnFocusGain([setHovered]() { setHovered(true); });
    area->setOnFocusLoss([setHovered, areaPtr]() {
      if (areaPtr->hovered()) {
        return;
      }
      setHovered(false);
    });
    area->setOnKeyDown([activate](const InputArea::KeyData& key) {
      if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
        activate();
      }
    });
  }

  return static_cast<InputArea*>(card.addChild(std::move(area)));
}

void HomeTab::layoutCardOverlays() {
  const auto cover = [](Flex* card, InputArea* area) {
    if (card == nullptr || area == nullptr) {
      return;
    }
    area->setPosition(0.0F, 0.0F);
    area->setSize(card->width(), card->height());
  };
  cover(m_mediaCard, m_mediaCardArea);
  cover(m_dateTimeCard, m_dateTimeCardArea);
  cover(m_userCard, m_userCardKeyboardArea);

  if (m_dateTimeCard != nullptr && m_weatherRow != nullptr && m_weatherCardArea != nullptr) {
    float wx = 0.0F;
    float wy = 0.0F;
    float cx = 0.0F;
    float cy = 0.0F;
    Node::absolutePosition(m_weatherRow, wx, wy);
    Node::absolutePosition(m_dateTimeCard, cx, cy);
    const float pad = Style::spaceXs * contentScale();
    m_weatherCardArea->setPosition(std::max(0.0F, wx - cx - pad), std::max(0.0F, wy - cy - pad));
    m_weatherCardArea->setSize(
        m_weatherRow->width() + pad * 2.0F, m_weatherRow->height() + pad * 2.0F
    );
  }

  // The pointer overlay must not swallow the avatar's own click, so start it just past the
  // avatar's right edge; the avatar (a nested InputArea) keeps the carved-out left region.
  if (m_userCard != nullptr && m_userCardArea != nullptr) {
    float left = 0.0F;
    if (m_userAvatar != nullptr) {
      float ax = 0.0F, ay = 0.0F, cx = 0.0F, cy = 0.0F;
      Node::absolutePosition(m_userAvatar, ax, ay);
      Node::absolutePosition(m_userCard, cx, cy);
      left = std::max(0.0F, (ax - cx) + m_userAvatar->width() + Style::spaceMd * contentScale());
    }
    m_userCardArea->setPosition(left, 0.0F);
    m_userCardArea->setSize(std::max(0.0F, m_userCard->width() - left), m_userCard->height());
  }
}

bool HomeTab::mediaExpanded() const {
  return m_mpris != nullptr && m_mpris->activePlayer().has_value();
}

bool HomeTab::resizeMediaArtToCard() {
  if (m_mediaCard == nullptr || m_mediaArt == nullptr || m_mediaArtSlot == nullptr) {
    return false;
  }

  const float scale = contentScale();
  const float desired = Style::controlHeightLg * 1.6F * scale;
  if (std::abs(m_mediaArtSlot->width() - desired) <= 0.5F) {
    return false;
  }

  m_mediaArtSlot->setSize(desired, desired);
  m_mediaArt->setSize(desired, desired);
  m_mediaArt->setRadius(homeMediaArtRadius(scale));
  if (m_mediaArtFallback != nullptr) {
    m_mediaArtFallback->setGlyphSize(desired * 0.55F);
  }
  return true;
}

void HomeTab::layoutWallpaperBackground(Renderer& renderer) {
  if (m_userCard == nullptr || m_wallpaperBg == nullptr) {
    return;
  }

  // The wallpaper sits one border width inside the card so a hover or focus stroke has a gap of
  // its own to fill; the card fill is cleared behind it, so that gap reads as panel background.
  const float bw = Style::borderWidth;
  const float cw = std::max(0.0F, m_userCard->width() - bw * 2.0F);
  const float ch = std::max(0.0F, m_userCard->height() - bw * 2.0F);
  const float radius = std::max(0.0F, Style::scaledRadius3xl(contentScale()) - bw);

  const Color surface = colorForRole(ColorRole::Surface);
  const Color translucentSurface = rgba(surface.r, surface.g, surface.b, surface.a * 0.9F);
  const Color transparentSurface = rgba(surface.r, surface.g, surface.b, 0.0F);
  const ImageScrim scrim{
      .direction = GradientDirection::Horizontal,
      .stops =
          {GradientStop{0.0F, translucentSurface}, GradientStop{0.25F, translucentSurface},
           GradientStop{0.9F, transparentSurface}, GradientStop{1.0F, transparentSurface}},
      .enabled = true,
  };

  for (Image* layer : {m_wallpaperPlaceholder, m_wallpaperBg}) {
    if (layer == nullptr) {
      continue;
    }
    layer->setPosition(bw, bw);
    layer->setSize(cw, ch);
    layer->setRadius(radius);
    layer->setScrim(scrim);
  }

  syncWallpaperBackground(renderer);
}

void HomeTab::syncUserCardFill() {
  if (m_userCard == nullptr) {
    return;
  }
  // Only a fully opaque layer counts: keeping the fill through a crossfade avoids a flash of
  // panel background under a half-faded wallpaper.
  const bool wallpaperCovers = m_placeholderReady || m_crispOpaque;
  if (wallpaperCovers) {
    m_userCard->clearFill();
    return;
  }
  m_userCard->setFill(colorSpecFromRole(ColorRole::Surface));
}

void HomeTab::syncWallpaperLayerVisibility() {
  if (m_wallpaperPlaceholder != nullptr) {
    m_wallpaperPlaceholder->setVisible(m_placeholderReady && !m_crispOpaque);
  }
  syncUserCardFill();
}

void HomeTab::ensureWallpaperThumbnail(const std::string& path, int targetPx) {
  if (m_thumbnails == nullptr) {
    return;
  }
  if (path == m_loadedWallpaperPath && targetPx == m_loadedWallpaperSize) {
    return;
  }
  if (!m_loadedWallpaperPath.empty() && m_loadedWallpaperSize > 0) {
    m_thumbnails->release(m_loadedWallpaperPath, m_loadedWallpaperSize);
  }
  if (!path.empty() && targetPx > 0) {
    (void)m_thumbnails->acquire(path, targetPx);
  }
  m_loadedWallpaperPath = path;
  m_loadedWallpaperSize = targetPx;
}

void HomeTab::syncWallpaperBackground(Renderer& renderer) {
  if (m_wallpaperBg == nullptr || m_wallpaperPlaceholder == nullptr) {
    return;
  }

  const std::string path = m_wallpaper != nullptr ? m_wallpaper->currentPath() : std::string{};
  const float renderScale = std::max(1.0F, renderer.renderScale());
  const int targetPx =
      static_cast<int>(std::lround(std::max(m_wallpaperBg->width(), m_wallpaperBg->height()) * renderScale));

  ensureWallpaperThumbnail(path, targetPx);

  if (path.empty()) {
    m_placeholderReady = false;
    m_crispOpaque = false;
    m_wallpaperBg->setVisible(false);
    cancelCrispFade();
    m_wallpaperBg->setOpacity(0.0F);
    m_crispWorkingPath.clear();
    m_crispWorkingSize = 0;
    m_crispShown = false;
    m_crispNeedsFade = false;
    syncWallpaperLayerVisibility();
    return;
  }

  // Instant placeholder: show the resident full-screen wallpaper texture (already
  // in VRAM, mipmapped) so the correct wallpaper appears with no decode wait.
  const TextureHandle resident = m_wallpaper != nullptr ? m_wallpaper->currentTexture() : TextureHandle{};
  m_placeholderReady = resident.valid();
  if (m_placeholderReady) {
    m_wallpaperPlaceholder->setExternalTexture(renderer, resident);
  }

  // Reset the crisp layer when the wallpaper identity or target size changes; the
  // placeholder carries the view until the new card-sized thumbnail is decoded.
  if (path != m_crispWorkingPath || targetPx != m_crispWorkingSize) {
    m_crispWorkingPath = path;
    m_crispWorkingSize = targetPx;
    m_crispShown = false;
    m_crispNeedsFade = false;
    m_crispOpaque = false;
    cancelCrispFade();
    m_wallpaperBg->setOpacity(0.0F);
    m_wallpaperBg->setVisible(false);
  }

  if (m_thumbnails == nullptr || targetPx <= 0 || m_crispShown) {
    syncWallpaperLayerVisibility();
    return;
  }

  (void)m_thumbnails->uploadPending(renderer.textureManager());
  const TextureHandle crisp = m_thumbnails->peek(path, targetPx);
  if (!crisp.valid()) {
    // Still decoding: keep the placeholder; fade the crisp layer in once it lands.
    m_crispNeedsFade = true;
    syncWallpaperLayerVisibility();
    return;
  }

  m_wallpaperBg->setExternalTexture(renderer, crisp);
  m_wallpaperBg->setVisible(true);
  m_crispShown = true;
  if (m_crispNeedsFade) {
    startCrispFade();
  } else {
    // Ready on the first look (cached), so snap in without a crossfade.
    cancelCrispFade();
    m_wallpaperBg->setOpacity(1.0F);
    m_crispOpaque = true;
  }
  syncWallpaperLayerVisibility();
}

void HomeTab::startCrispFade() {
  if (m_wallpaperBg == nullptr) {
    return;
  }
  AnimationManager* animations = m_wallpaperBg->animationManager();
  if (animations == nullptr) {
    m_wallpaperBg->setOpacity(1.0F);
    m_crispOpaque = true;
    syncWallpaperLayerVisibility();
    return;
  }
  cancelCrispFade();
  Image* crisp = m_wallpaperBg;
  m_wallpaperCrispAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
      [crisp](float v) { crisp->setOpacity(v); },
      [this]() {
        m_wallpaperCrispAnimId = 0;
        m_crispOpaque = true;
        syncWallpaperLayerVisibility();
        PanelManager::instance().requestRedraw();
      },
      crisp
  );
}

void HomeTab::cancelCrispFade() {
  if (m_wallpaperCrispAnimId != 0 && m_wallpaperBg != nullptr) {
    if (AnimationManager* animations = m_wallpaperBg->animationManager()) {
      animations->cancel(m_wallpaperCrispAnimId);
    }
  }
  m_wallpaperCrispAnimId = 0;
}

void HomeTab::syncHeaderActions() {
  setShown(m_sessionButton, m_config == nullptr || m_config->config().controlCenter.showSessionButton);
}

void HomeTab::syncStatusStrip() {
  const auto applyChip = [](Button* button, bool show, std::string_view text, std::string_view glyph,
                            bool emphasis) {
    if (button == nullptr) {
      return false;
    }
    const bool wasShown = button->visible();
    setShown(button, show);
    if (show) {
      button->setText(text);
      button->setGlyph(glyph);
      const bool needStyle = wasShown != show
          || (emphasis && button->variant() != ButtonVariant::Primary)
          || (!emphasis && button->variant() != ButtonVariant::Ghost);
      if (needStyle) {
        applyHomeStatusChipStyle(*button, emphasis);
      }
    }
    return wasShown != show;
  };

  bool layoutDirty = false;
  const std::size_t updates = noctalia::packages::pendingUpdateCount();
  layoutDirty = applyChip(
                    m_statusUpdates, updates > 0,
                    i18n::tr("control-center.home.status.updates", "count", std::to_string(updates)), "package", false
                )
      || layoutDirty;

  auto* aiUsage = m_services.aiUsage;
  if (aiUsage != nullptr) {
    const auto hit = noctalia::ai_usage::hottestQuota(aiUsage->snapshot());
    const bool hot =
        hit.has_value() && hit->window.percent + 0.5F >= static_cast<float>(aiUsage->attentionPercent());
    if (hot) {
      layoutDirty = applyChip(
                        m_statusAi, true,
                        i18n::tr(
                            "control-center.home.status.ai", "percent",
                            std::format("{:.0f}", static_cast<double>(hit->window.percent))
                        ),
                        noctalia::ai_usage::providerGlyph(hit->provider), true
                    )
          || layoutDirty;
    } else {
      layoutDirty = applyChip(m_statusAi, false, {}, "sparkles", false) || layoutDirty;
    }
  } else {
    layoutDirty = applyChip(m_statusAi, false, {}, "sparkles", false) || layoutDirty;
  }

  if (m_upower != nullptr && m_config != nullptr) {
    const auto* battery = m_upower->defaultSystemBattery();
    bool low = false;
    int pct = 0;
    BatteryState state = BatteryState::Unknown;
    if (battery != nullptr && battery->state.isPresent) {
      pct = static_cast<int>(std::lround(battery->state.percentage));
      state = battery->state.state;
      const int warn = batteryWarningThresholdForDevice(m_config->config().battery, *battery, battery);
      const bool discharging = state == BatteryState::Discharging || state == BatteryState::PendingDischarge
          || state == BatteryState::Empty || battery->state.onBattery;
      low = warn > 0 && pct <= warn && discharging;
    }
    if (low) {
      layoutDirty = applyChip(
                        m_statusBattery, true,
                        i18n::tr("control-center.home.status.battery", "percent", std::to_string(pct)),
                        batteryGlyphName(static_cast<double>(pct), state), true
                    )
          || layoutDirty;
    } else {
      layoutDirty = applyChip(m_statusBattery, false, {}, "battery-2", false) || layoutDirty;
    }
  } else {
    layoutDirty = applyChip(m_statusBattery, false, {}, "battery-2", false) || layoutDirty;
  }

  auto* recorder = m_services.screenRecorder;
  const bool recording =
      recorder != nullptr
      && (recorder->isRecording() || recorder->isStopping() || recorder->isReplaying() || recorder->isReplayStopping());
  if (recording) {
    const bool replay = recorder->isReplaying() || recorder->isReplayStopping();
    const char* key =
        replay ? "control-center.home.status.replay" : "control-center.home.status.recording";
    layoutDirty = applyChip(
                      m_statusCapture, true,
                      i18n::tr(key, "elapsed", recorder->elapsedLabel()), replay ? "history" : "player-stop", true
                  )
        || layoutDirty;
  } else {
    layoutDirty = applyChip(m_statusCapture, false, {}, "video", false) || layoutDirty;
  }

  auto* nightLight = m_services.nightLight;
  const bool nightOn =
      nightLight != nullptr && (nightLight->forceEnabled() || nightLight->active());
  layoutDirty = applyChip(
                    m_statusNightLight, nightOn, i18n::tr("control-center.home.status.night-light"),
                    nightLight != nullptr && nightLight->forceEnabled() ? "nightlight-forced" : "nightlight-on",
                    false
                )
      || layoutDirty;

  auto* caffeine = m_services.idleInhibitor;
  const bool caffeineOn = caffeine != nullptr && caffeine->available() && caffeine->enabled();
  layoutDirty = applyChip(
                    m_statusCaffeine, caffeineOn, i18n::tr("control-center.home.status.caffeine"), "caffeine-on", false
                )
      || layoutDirty;

  auto* notifications = m_services.notifications;
  const bool dnd = notifications != nullptr && notifications->doNotDisturb();
  layoutDirty = applyChip(
                    m_statusDnd, dnd, i18n::tr("control-center.home.status.dnd"), "bell-off", false
                )
      || layoutDirty;

  auto* localSend = m_services.localSend;
  const bool incoming = localSend != nullptr && localSend->snapshot().incoming.has_value();
  if (incoming) {
    layoutDirty = applyChip(
                      m_statusLocalSend, true,
                      i18n::tr("control-center.home.status.localsend", "name", localSend->snapshot().incoming->alias),
                      "share", true
                  )
        || layoutDirty;
  } else {
    layoutDirty = applyChip(m_statusLocalSend, false, {}, "share", false) || layoutDirty;
  }

  auto* phone = m_services.phone;
  const auto* peer = phone != nullptr ? phone->peerDevice() : nullptr;
  if (peer != nullptr) {
    const std::string name = peer->name.empty() ? peer->id : peer->name;
    std::string text;
    if (!peer->reachable) {
      text = i18n::tr("control-center.home.status.phone-offline", "name", name);
    } else if (peer->hasBattery && peer->charge >= 0) {
      text = i18n::tr("control-center.home.status.phone-battery", "name", name, "percent", peer->charge);
    } else {
      text = i18n::tr("control-center.home.status.phone", "name", name);
    }
    layoutDirty = applyChip(m_statusPhone, true, text, "device-mobile", peer->reachable) || layoutDirty;
  } else {
    layoutDirty = applyChip(m_statusPhone, false, {}, "device-mobile", false) || layoutDirty;
  }

  auto* audio = m_services.audio;
  const PrivacyState* privacy = audio != nullptr ? &audio->privacyState() : nullptr;
  bool privacyMic = false;
  bool privacyCamera = false;
  bool privacyScreen = false;
  if (privacy != nullptr) {
    for (const auto& capture : privacy->captures) {
      switch (capture.kind) {
      case PrivacyCaptureKind::Microphone:
        privacyMic = true;
        break;
      case PrivacyCaptureKind::Camera:
        privacyCamera = true;
        break;
      case PrivacyCaptureKind::Screen:
        privacyScreen = true;
        break;
      }
    }
  }
  const int privacyKinds = static_cast<int>(privacyMic) + static_cast<int>(privacyCamera)
      + static_cast<int>(privacyScreen);
  if (privacyKinds > 0) {
    const char* glyph = "eye-off";
    const char* key = "control-center.home.status.privacy";
    if (privacyKinds == 1) {
      if (privacyMic) {
        glyph = "microphone";
        key = "control-center.home.status.privacy-microphone";
      } else if (privacyCamera) {
        glyph = "camera";
        key = "control-center.home.status.privacy-camera";
      } else {
        glyph = "screen-share";
        key = "control-center.home.status.privacy-screen";
      }
    }
    layoutDirty = applyChip(m_statusPrivacy, true, i18n::tr(key), glyph, true) || layoutDirty;
  } else {
    layoutDirty = applyChip(m_statusPrivacy, false, {}, "eye-off", false) || layoutDirty;
  }

  auto* cast = m_services.cast;
  const bool sharing = cast != nullptr && cast->sharing();
  if (sharing) {
    layoutDirty = applyChip(
                      m_statusCast, true, i18n::tr("control-center.home.status.cast"), "screen-share", true
                  )
        || layoutDirty;
  } else {
    layoutDirty = applyChip(m_statusCast, false, {}, "screen-share", false) || layoutDirty;
  }

  auto* firewall = m_services.firewall;
  const bool panic = firewall != nullptr && firewall->snapshot().panic;
  if (panic) {
    layoutDirty = applyChip(
                      m_statusFirewall, true, i18n::tr("control-center.home.status.firewall"), "shield-lock", true
                  )
        || layoutDirty;
  } else {
    layoutDirty = applyChip(m_statusFirewall, false, {}, "shield-lock", false) || layoutDirty;
  }

  const bool any = (m_statusUpdates != nullptr && m_statusUpdates->visible())
      || (m_statusAi != nullptr && m_statusAi->visible())
      || (m_statusBattery != nullptr && m_statusBattery->visible())
      || (m_statusCapture != nullptr && m_statusCapture->visible())
      || (m_statusNightLight != nullptr && m_statusNightLight->visible())
      || (m_statusCaffeine != nullptr && m_statusCaffeine->visible())
      || (m_statusDnd != nullptr && m_statusDnd->visible())
      || (m_statusLocalSend != nullptr && m_statusLocalSend->visible())
      || (m_statusPhone != nullptr && m_statusPhone->visible())
      || (m_statusPrivacy != nullptr && m_statusPrivacy->visible())
      || (m_statusCast != nullptr && m_statusCast->visible())
      || (m_statusFirewall != nullptr && m_statusFirewall->visible());
  if (m_statusStrip != nullptr) {
    const bool wasShown = m_statusStrip->visible();
    setShown(m_statusStrip, any);
    layoutDirty = layoutDirty || wasShown != any;
  }
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
    PanelManager::instance().requestRedraw();
  }
}

void HomeTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  syncHeaderActions();
  if (!m_active) {
    m_progressTimer.stop();
    return;
  }

  const bool playing =
      m_mpris != nullptr && m_mpris->activePlayer().has_value() && m_mpris->activePlayer()->playbackStatus == "Playing";
  if (playing) {
    if (!m_progressTimer.active()) {
      m_progressTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
        if (!m_active) {
          return;
        }
        // refresh() schedules update+layout (Surface::requestUpdate); update-only ticks skipped
        // HomeTab::doLayout so album art never picked up the final media card height.
        PanelManager::instance().refresh();
        PanelManager::instance().requestRedraw();
      });
    }
  } else {
    m_progressTimer.stop();
  }
  sync(renderer);
}

void HomeTab::onFrameTick(float /*deltaMs*/) {}

void HomeTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (!active) {
    m_progressTimer.stop();
    m_clockTimer.stop();
    m_nextRealtimeUpdateAt = {};
    m_lastRealtimeMprisPollAt = {};
    m_mediaPositionBusName.clear();
    m_mediaPositionTrackId.clear();
    m_mediaPositionTrackSignature.clear();
    m_mediaLastPlaybackStatus.clear();
    m_mediaPositionUs = 0;
    m_mediaPositionSampleAt = {};
    return;
  }

  if (becameActive) {
    // Other tabs were laid out while this body was hidden; flex sizes for the media row can be stale.
    // Defer so the tab container receives its configure size before HomeTab::doLayout runs.
    DeferredCall::callLater([]() {
      PanelManager::instance().requestLayout();
      PanelManager::instance().requestUpdateOnly();
    });

    // Tick the clock once per second so the time/date labels keep moving while the panel is
    // open and idle. Without this, the clock only refreshes when an unrelated service happens
    // to force a redraw (see issue #4046). setText() is a no-op when the text is unchanged, so
    // formats without seconds cost nothing beyond the cheap format + comparison each second.
    m_clockTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
      if (!m_active) {
        return;
      }
      bool changed = false;
      if (m_timeLabel != nullptr) {
        changed = m_timeLabel->setText(formatShellTime(m_config)) || changed;
      }
      if (m_dateLabel != nullptr) {
        changed = m_dateLabel->setText(formatShellDate(m_config)) || changed;
      }
      syncStatusStrip();
      if (changed) {
        PanelManager::instance().requestLayout();
        PanelManager::instance().requestRedraw();
      }
    });
  }
}

void HomeTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_progressTimer.stop();
  m_clockTimer.stop();
  m_rootLayout = nullptr;
  m_liveScroll = nullptr;
  m_liveScrollState.offset = 0.0F;
  m_statusStrip = nullptr;
  m_dateTimeCard = nullptr;
  m_mediaCard = nullptr;
  m_mediaText = nullptr;
  m_userCard = nullptr;
  m_userMain = nullptr;
  m_userAvatar = nullptr;
  m_timeLabel = nullptr;
  m_dateLabel = nullptr;
  m_weatherRow = nullptr;
  m_weatherGlyph = nullptr;
  m_weatherLine = nullptr;
  m_eventRow = nullptr;
  m_eventGlyph = nullptr;
  m_eventLine = nullptr;
  m_userHost = nullptr;
  m_userUptime = nullptr;
  m_sessionButton = nullptr;
  m_volumeSlider = nullptr;
  m_brightnessSlider = nullptr;
  m_volumeValue = nullptr;
  m_brightnessValue = nullptr;
  m_volumeButton = nullptr;
  m_brightnessRow = nullptr;
  m_sessionWell = nullptr;
  m_lockButton = nullptr;
  m_suspendButton = nullptr;
  for (auto& pad : m_shortcutPads) {
    pad.button = nullptr;
    pad.glyph = nullptr;
    pad.label = nullptr;
  }
  for (auto& row : m_quickToggles) {
    if (row.shortcut != nullptr) {
      row.shortcut->onPanelClose();
    }
    row.button = nullptr;
  }
  if (m_volumeShortcut != nullptr) {
    m_volumeShortcut->onPanelClose();
  }
  m_statusUpdates = nullptr;
  m_statusAi = nullptr;
  m_statusBattery = nullptr;
  m_statusCapture = nullptr;
  m_statusNightLight = nullptr;
  m_statusCaffeine = nullptr;
  m_statusDnd = nullptr;
  m_statusLocalSend = nullptr;
  m_statusPhone = nullptr;
  m_statusPrivacy = nullptr;
  m_statusCast = nullptr;
  m_statusFirewall = nullptr;
  m_userCardKeyboardArea = nullptr;
  m_userCardArea = nullptr;
  m_mediaCardArea = nullptr;
  m_dateTimeCardArea = nullptr;
  m_weatherCardArea = nullptr;
  // The crisp fade animation is tagged with the m_wallpaperBg node as owner, so
  // it is cancelled automatically when the node tree is destroyed on close.
  m_wallpaperCrispAnimId = 0;
  m_crispWorkingPath.clear();
  m_crispWorkingSize = 0;
  m_crispShown = false;
  m_crispNeedsFade = false;
  m_crispOpaque = false;
  m_placeholderReady = false;
  m_wallpaperPlaceholder = nullptr;
  m_wallpaperBg = nullptr;
  m_mediaTrack = nullptr;
  m_mediaArtist = nullptr;
  m_mediaTransport = nullptr;
  m_mediaPrev = nullptr;
  m_mediaPlayPause = nullptr;
  m_mediaNext = nullptr;
  m_mediaProgressBar = nullptr;
  m_mediaArt = nullptr;
  m_mediaArtSlot = nullptr;
  m_mediaArtFallback = nullptr;
  m_loadedMediaArtUrl.clear();
  m_mediaPositionBusName.clear();
  m_mediaPositionTrackId.clear();
  m_mediaPositionTrackSignature.clear();
  m_mediaLastPlaybackStatus.clear();
  m_mediaPositionUs = 0;
  m_mediaPositionSampleAt = {};
  m_nextRealtimeUpdateAt = {};
  m_lastRealtimeMprisPollAt = {};
  m_shortcutsColumn = nullptr;
  m_pinDrag = {};
  // Keep Shortcut instances alive; drop only the UI pointers destroyed with the scene.
  for (auto& pad : m_shortcutPads) {
    if (pad.shortcut != nullptr) {
      pad.shortcut->onPanelClose();
    }
    pad.button = nullptr;
    pad.glyph = nullptr;
    pad.label = nullptr;
  }
}

void HomeTab::onPanelCardOpacityChanged(float opacity) {
  const float scale = contentScale();
  if (m_userCard != nullptr) {
    applyHomeCardStyle(*m_userCard, scale, opacity);
    syncUserCardFill();
  }
  if (m_mediaCard != nullptr) {
    applyHomeCardStyle(*m_mediaCard, scale, opacity);
  }
  if (m_dateTimeCard != nullptr) {
    applyHomeClockCardStyle(*m_dateTimeCard, scale, opacity);
  }
  if (m_statusStrip != nullptr) {
    applyModuleWellStyle(*m_statusStrip, scale);
  }
  if (m_shortcutsColumn != nullptr) {
    applyGroupedListStyle(*m_shortcutsColumn, scale);
  }
  syncShortcuts();
}

void HomeTab::syncScaledFonts() {
  const float s = contentScale();
  if (m_dateLabel != nullptr) {
    m_dateLabel->setFontSize(Style::fontSizeBody * 0.95F * s);
  }
  if (m_weatherGlyph != nullptr) {
    m_weatherGlyph->setGlyphSize(Style::fontSizeTitle * s);
  }
  if (m_weatherLine != nullptr) {
    m_weatherLine->setFontSize(Style::fontSizeBody * 0.95F * s);
  }
  if (m_eventGlyph != nullptr) {
    m_eventGlyph->setGlyphSize(Style::fontSizeTitle * s);
  }
  if (m_eventLine != nullptr) {
    m_eventLine->setFontSize(Style::fontSizeBody * 0.95F * s);
  }
  for (Label* label : {m_userHost, m_userUptime}) {
    if (label != nullptr) {
      label->setFontSize(Style::fontSizeCaption * s);
    }
  }
  if (m_mediaTrack != nullptr) {
    const bool expanded = mediaExpanded();
    m_mediaTrack->setFontSize((expanded ? Style::fontSizeTitle : Style::fontSizeBody * 0.95F) * s);
    m_mediaTrack->setFontWeight(expanded ? FontWeight::Bold : FontWeight::Normal);
  }
  if (m_mediaArtist != nullptr) {
    m_mediaArtist->setFontSize(Style::fontSizeCaption * s);
  }
  for (auto& pad : m_shortcutPads) {
    if (pad.label != nullptr) {
      pad.label->setFontSize(Style::fontSizeBody * s);
    }
    if (pad.glyph != nullptr) {
      pad.glyph->setGlyphSize(Style::fontSizeHeader * s);
    }
  }
}

void HomeTab::syncQuickControls() {
  if (m_volumeButton != nullptr && m_volumeShortcut != nullptr) {
    m_volumeButton->setGlyph(m_volumeShortcut->displayIcon());
    applyShortcutButtonStyle(*m_volumeButton, m_volumeShortcut->enabled(), m_volumeShortcut->active());
  }
  if (m_volumeSlider != nullptr && !m_volumeSlider->dragging()) {
    const AudioNode* sink = m_audio != nullptr ? m_audio->defaultSink() : nullptr;
    m_syncingQuickSliders = true;
    if (sink != nullptr) {
      m_volumeSlider->setEnabled(true);
      m_volumeSlider->setValue(std::clamp(static_cast<double>(sink->volume), 0.0, 1.5));
      if (m_volumeValue != nullptr) {
        m_volumeValue->setText(std::format("{:.0F}%", sink->volume * 100.0F));
      }
    } else {
      m_volumeSlider->setEnabled(false);
    }
    m_syncingQuickSliders = false;
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
    m_brightnessRow->setVisible(shown != nullptr);
    m_brightnessRow->setParticipatesInLayout(shown != nullptr);
  }
  if (shown != nullptr && m_brightnessSlider != nullptr && !m_brightnessSlider->dragging()) {
    m_syncingQuickSliders = true;
    m_brightnessSlider->setValue(shown->brightness);
    m_syncingQuickSliders = false;
    if (m_brightnessValue != nullptr) {
      m_brightnessValue->setText(std::format("{:.0F}%", shown->brightness * 100.0F));
    }
  }

  for (auto& row : m_quickToggles) {
    if (row.shortcut == nullptr || row.button == nullptr) {
      continue;
    }
    const std::string label = row.shortcut->displayLabel();
    const std::string state = !row.shortcut->enabled()
        ? i18n::tr("control-center.home.quick-unavailable")
        : label != row.shortcut->defaultLabel() ? label
        : i18n::tr(row.shortcut->active() ? "control-center.home.quick-on" : "control-center.home.quick-off");
    if (row.status != nullptr) {
      row.status->setText(state);
    }
    row.button->setTooltip(row.shortcut->defaultLabel() + ": " + state);
    row.button->setGlyph(row.shortcut->displayIcon());
    applyShortcutButtonStyle(*row.button, row.shortcut->enabled(), row.shortcut->active());
  }

  const bool showSession = m_config == nullptr || m_config->config().controlCenter.showSessionButton;
  setShown(m_sessionWell, showSession);
  setShown(m_lockButton, showSession && m_session != nullptr && m_config != nullptr && m_config->isLockScreenEnabled());
  setShown(m_suspendButton, showSession && m_session != nullptr);

}

void HomeTab::sync(Renderer& renderer) {
  syncQuickControls();
  syncScaledFonts();
  syncShortcuts();
  syncStatusStrip();

  if (m_timeLabel != nullptr) {
    m_timeLabel->setText(formatShellTime(m_config));
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setText(formatShellDate(m_config));
  }

  syncWallpaperBackground(renderer);

  if (m_userAvatar != nullptr && m_config != nullptr && m_asyncTextures != nullptr) {
    const std::string displayPath = shell::avatarDisplayPath(m_accounts, m_config->config());
    if (displayPath.empty()) {
      m_userAvatar->clear(renderer);
    } else {
      warnOnOversizedAvatarSource(displayPath);
      // Decode at the avatar's final on-screen size with no mipmaps: layout grows the
      // avatar to match the user text block, and trilinear mipmap sampling softens an
      // image displayed near 1:1. Both made the avatar look blurry. The decode runs on
      // the async cache workers so a large source never stalls the panel open.
      const int avatarSize = static_cast<int>(std::round(m_userAvatar->width()));
      (void)m_userAvatar->setSourceFileAsync(renderer, *m_asyncTextures, displayPath, avatarSize, false);
    }
  }

  if (m_userHost != nullptr) {
    m_userHost->setText(userHostLine());
  }
  if (m_userUptime != nullptr) {
    const auto uptime = systemUptime();
    const std::string uptimeText =
        uptime.has_value() ? formatDuration(*uptime) : i18n::tr("control-center.home.unknown");
    m_userUptime->setText(i18n::tr("control-center.home.uptime", "uptime", uptimeText));
  }

  if (m_weatherGlyph != nullptr && m_weatherLine != nullptr) {
    const auto setWeatherUnavailable = [this](std::string_view glyph, const std::string& text) {
      m_weatherGlyph->setGlyph(glyph);
      m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_weatherLine->setText(text);
    };
    if (m_weather == nullptr || !m_weather->enabled()) {
      setWeatherUnavailable("weather-cloud-off", i18n::tr("control-center.home.weather.disabled"));
    } else if (!m_weather->locationConfigured()) {
      setWeatherUnavailable("weather-cloud", i18n::tr("control-center.home.weather.no-location"));
    } else {
      const auto& snapshot = m_weather->snapshot();
      if (!snapshot.valid) {
        setWeatherUnavailable(
            "weather-cloud",
            m_weather->loading() ? i18n::tr("control-center.home.weather.fetching")
                                 : i18n::tr("control-center.home.weather.data-unavailable")
        );
      } else {
        m_weatherGlyph->setGlyph(WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay));
        m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::Primary));
        const int t = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
        const std::string temp = std::format("{}{}", t, m_weather->displayTemperatureUnit());
        const std::string description = WeatherService::descriptionForCode(snapshot.current.weatherCode);
        m_weatherLine->setText(description.empty() ? temp : std::format("{} · {}", temp, description));
      }
    }
  }

  if (m_eventRow != nullptr && m_eventLine != nullptr && m_eventGlyph != nullptr) {
    const CalendarEvent* next = nullptr;
    if (m_calendar != nullptr && m_calendar->enabled() && m_calendar->hasData()) {
      next = homeNextCalendarEvent(m_calendar->snapshot());
    }
    const bool show = next != nullptr;
    if (m_eventRow->visible() != show) {
      setShown(m_eventRow, show);
      PanelManager::instance().requestLayout();
    }
    if (show) {
      const std::string title =
          next->title.empty() ? i18n::tr("control-center.calendar.events") : next->title;
      std::string when;
      if (next->allDay) {
        when = i18n::tr("control-center.calendar.all-day");
      } else {
        const std::time_t raw = std::chrono::system_clock::to_time_t(next->start);
        const char* format = m_config != nullptr ? m_config->config().shell.timeFormat.c_str() : "{:%H:%M}";
        when = formatLocalUnixTime(static_cast<std::int64_t>(raw), format);
      }
      m_eventGlyph->setGlyph("calendar-event");
      m_eventGlyph->setColor(colorSpecFromRole(ColorRole::Primary));
      m_eventLine->setText(std::format("{} · {}", when, title));
    }
  }

  if (m_mediaTrack != nullptr && m_mediaArtist != nullptr) {
    const auto setIdleMedia = [this, &renderer](std::string_view title) {
      bool dirty = m_mediaTrack->setText(title);
      if (m_mediaArtist->visible()) {
        m_mediaArtist->setText("");
        m_mediaArtist->setVisible(false);
        dirty = true;
      }
      if (m_mediaTransport != nullptr && m_mediaTransport->visible()) {
        setShown(m_mediaTransport, false);
        dirty = true;
      }
      if (m_mediaProgressBar != nullptr && m_mediaProgressBar->visible()) {
        setShown(m_mediaProgressBar, false);
        dirty = true;
      }
      if (m_mediaArt != nullptr && (m_mediaArt->visible() || m_mediaArt->hasImage())) {
        m_mediaArt->clear(renderer);
        m_mediaArt->setVisible(false);
        dirty = true;
      }
      if (m_mediaArtFallback != nullptr && !m_mediaArtFallback->visible()) {
        m_mediaArtFallback->setVisible(true);
        dirty = true;
      }
      if (!m_loadedMediaArtUrl.empty()) {
        m_loadedMediaArtUrl.clear();
      }
      if (m_mediaCard != nullptr && m_mediaCard->visible()) {
        setShown(m_mediaCard, false);
        dirty = true;
      }
      if (dirty) {
        PanelManager::instance().requestLayout();
      }
    };

    const auto syncTransport = [this](const MprisPlayerInfo* player) {
      const bool show = player != nullptr;
      if (m_mediaTransport != nullptr && m_mediaTransport->visible() != show) {
        setShown(m_mediaTransport, show);
        PanelManager::instance().requestLayout();
      }
      if (!show) {
        return;
      }
      const bool playing = player->playbackStatus == "Playing";
      if (m_mediaPrev != nullptr) {
        m_mediaPrev->setEnabled(player->canGoPrevious);
      }
      if (m_mediaNext != nullptr) {
        m_mediaNext->setEnabled(player->canGoNext);
      }
      if (m_mediaPlayPause != nullptr) {
        const bool canToggle = player->canPlay || player->canPause || player->canControl;
        m_mediaPlayPause->setGlyph(playing ? "media-pause" : "media-play");
        m_mediaPlayPause->setTooltip(
            i18n::tr(playing ? "control-center.media.pause" : "control-center.media.play")
        );
        if (m_mediaPlayPause->selected() != (canToggle && playing) || m_mediaPlayPause->enabled() != canToggle) {
          applyShortcutButtonStyle(*m_mediaPlayPause, canToggle, playing);
        }
      }
    };

    if (m_mpris == nullptr) {
      setIdleMedia(i18n::tr("control-center.home.media.playback-unavailable"));
      syncTransport(nullptr);
    } else {
      const auto active = m_mpris->activePlayer();
      if (!active.has_value()) {
        m_mediaPositionBusName.clear();
        m_mediaPositionTrackId.clear();
        m_mediaPositionTrackSignature.clear();
        m_mediaLastPlaybackStatus.clear();
        m_mediaPositionUs = 0;
        m_mediaPositionSampleAt = {};
        setIdleMedia(i18n::tr("control-center.home.media.nothing-playing"));
        syncTransport(nullptr);
      } else {
        if (m_mediaCard != nullptr && !m_mediaCard->visible()) {
          setShown(m_mediaCard, true);
          PanelManager::instance().requestLayout();
        }
        const std::string trackText =
            active->title.empty() ? i18n::tr("control-center.home.media.unknown-track") : active->title;
        const std::string artists = mpris::joinArtists(active->artists);
        const std::string artistText = artists.empty() ? i18n::tr("control-center.home.media.unknown-artist") : artists;
        if (m_mediaTrack->text() != trackText || m_mediaArtist->text() != artistText || !m_mediaArtist->visible()) {
          m_mediaTrack->setText(trackText);
          m_mediaArtist->setText(artistText);
          m_mediaArtist->setVisible(true);
          PanelManager::instance().requestLayout();
        }
        const std::string trackSignature = std::format(
            "{}\n{}\n{}\n{}\n{}", active->trackId, active->title, artists, active->album, active->sourceUrl
        );
        float progress = 0.0F;
        bool showProgress = false;
        if (active->lengthUs > 0) {
          const auto now = std::chrono::steady_clock::now();
          std::int64_t livePositionUs = std::max<std::int64_t>(0, active->positionUs);
          livePositionUs = std::clamp<std::int64_t>(livePositionUs, 0, active->lengthUs);
          const bool sameDisplayedTrack =
              m_mediaPositionBusName == active->busName && m_mediaPositionTrackSignature == trackSignature;
          const bool withinTransientRegressionWindow =
              m_mediaPositionSampleAt != std::chrono::steady_clock::time_point{}
              && now - m_mediaPositionSampleAt <= kHomeTransientPositionRegressionWindow;
          const bool preserveDisplayedPosition = sameDisplayedTrack
              && m_mediaLastPlaybackStatus == "Playing"
              && active->playbackStatus == "Playing"
              && m_mediaPositionUs >= kHomeTransientPositionRegressionFloorUs
              && livePositionUs <= kHomeTransientPositionRegressionCeilingUs
              && livePositionUs + kHomeTransientPositionRegressionDeltaUs < m_mediaPositionUs
              && withinTransientRegressionWindow;
          if (preserveDisplayedPosition) {
            livePositionUs = m_mediaPositionUs;
          }

          m_mediaPositionBusName = active->busName;
          m_mediaPositionTrackId = active->trackId;
          m_mediaPositionTrackSignature = trackSignature;
          m_mediaLastPlaybackStatus = active->playbackStatus;
          if (!preserveDisplayedPosition) {
            m_mediaPositionUs = livePositionUs;
            m_mediaPositionSampleAt = now;
          }
          progress = static_cast<float>(livePositionUs) / static_cast<float>(active->lengthUs);
          showProgress = true;
        } else {
          m_mediaPositionBusName.clear();
          m_mediaPositionTrackId.clear();
          m_mediaPositionTrackSignature.clear();
          m_mediaLastPlaybackStatus.clear();
          m_mediaPositionUs = 0;
          m_mediaPositionSampleAt = {};
        }
        if (m_mediaProgressBar != nullptr) {
          if (m_mediaProgressBar->visible() != showProgress) {
            setShown(m_mediaProgressBar, showProgress);
            PanelManager::instance().requestLayout();
          }
          if (showProgress) {
            m_mediaProgressBar->setProgress(progress);
          }
        }
        syncTransport(&*active);
        if (m_mediaArt != nullptr) {
          const std::string artUrl = mpris::effectiveArtUrl(*active);
          const bool artRetry = !artUrl.empty() && !m_mediaArt->hasImage();
          if (artUrl != m_loadedMediaArtUrl || artRetry) {
            const std::string artPath = mpris::resolveArtworkSource(
                m_httpClient, m_pendingArtDownloads, artUrl,
                [this] {
                  m_loadedMediaArtUrl.clear();
                  PanelManager::instance().refresh();
                },
                m_aliveGuard
            );
            bool loaded = false;
            if (!artPath.empty()) {
              const int decodeSize = static_cast<int>(std::round(Style::controlHeightLg * 2.6F * contentScale()));
              loaded = m_mediaArt->setSourceFile(renderer, artPath, decodeSize, true, true);
              if (!loaded) {
                m_mediaArt->clear(renderer);
              }
            } else {
              m_mediaArt->clear(renderer);
            }
            m_mediaArt->setVisible(loaded);
            m_mediaArtFallback->setVisible(!loaded);
            m_loadedMediaArtUrl = loaded ? artUrl : std::string{};
            PanelManager::instance().requestLayout();
          }
        }
      }
    }
  }
}

void HomeTab::warnOnOversizedAvatarSource(const std::string& path) {
  if (path == m_sizeCheckedAvatarPath) {
    return;
  }
  m_sizeCheckedAvatarPath = path;

  std::error_code ec;
  const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
  if (ec || bytes <= kHomeAvatarSourceWarnBytes) {
    return;
  }
  kLog.warn(
      "avatar source '{}' is {} MiB; large images decode slowly, consider a smaller image", path,
      bytes / (1024ULL * 1024ULL)
  );
}

void HomeTab::syncShortcuts() {
  for (auto& pad : m_shortcutPads) {
    auto& sc = *pad.shortcut;
    const bool enabled = sc.enabled();
    const bool on = sc.active();

    if (pad.button != nullptr) {
      applyShortcutButtonStyle(*pad.button, enabled, on);
      pad.button->setTooltip(sc.displayLabel());
    }
    if (pad.glyph != nullptr) {
      pad.glyph->setGlyph(sc.displayIcon());
    }
    if (pad.button != nullptr && pad.label != nullptr) {
      const std::string label = sc.displayLabel();
      if (pad.label->text() != label) {
        pad.button->setText(label);
      }
    }
  }
}
