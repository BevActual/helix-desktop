#include "shell/control_center/tabs/home_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/directory_scanner.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
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
#include "shell/control_center/shortcut_registry.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "shell/wallpaper/wallpaper.h"
#include "capture/screen_recorder_service.h"
#include "idle/idle_inhibitor.h"
#include "system/distro_info.h"
#include "system/gamma_service.h"
#include "system/package_kit_service.h"
#include "system/weather_service.h"
#include "net/localsend_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "render/scene/node.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
  // Bottom row split: media/clock column grows more than the shortcuts column so the row feels balanced
  // (tweak either value slightly if needed).
  constexpr float kHomeMainColumnFlexGrow = 1.32F;
  constexpr float kHomeShortcutsFlexGrow = 1.12F;
  // Left-column card height split (media above, clock/weather below).
  constexpr float kHomeMediaCardFlexGrow = 1.4F;
  constexpr float kHomeDateTimeCardFlexGrow = 1.0F;
  constexpr std::size_t kHomeShortcutGridColumns = 2;
  // At or below this count the shortcuts stack in a single narrow column instead of the 2-column grid.
  constexpr std::size_t kHomeStackedShortcutMax = 2;
  // Extra-large Control Center tiles stay square.
  constexpr float kHomeShortcutSquareTrim = 1.0F;

  enum class HomeShortcutCluster : std::uint8_t {
    Connectivity,
    Focus,
    Display,
    Sound,
    Capture,
    Power,
    Sharing,
    Other,
  };
  constexpr std::size_t kHomeShortcutClusterCount = 8;

  [[nodiscard]] HomeShortcutCluster homeClusterFor(std::string_view type) {
    if (type == "wifi" || type == "bluetooth" || type == "tailscale" || type == "localsend" || type == "phone") {
      return HomeShortcutCluster::Connectivity;
    }
    if (type == "notification" || type == "caffeine" || type == "dark_mode") {
      return HomeShortcutCluster::Focus;
    }
    if (type == "nightlight" || type == "wallpaper") {
      return HomeShortcutCluster::Display;
    }
    if (type == "audio" || type == "mic_mute" || type == "media") {
      return HomeShortcutCluster::Sound;
    }
    if (type == "screenshot"
        || type == "pick_color"
        || type == "ocr"
        || type == "scan_qr"
        || type == "screen_recorder"
        || type == "screen_replay") {
      return HomeShortcutCluster::Capture;
    }
    if (type == "power_profile" || type == "session" || type == "system" || type == "screen_time") {
      return HomeShortcutCluster::Power;
    }
    if (type == "clipboard"
        || type == "printers"
        || type == "cast"
        || type == "syncthing"
        || type == "firewall"
        || type == "privacy"
        || type == "weather"
        || type == "keyboard_layout"
        || type == "ai") {
      return HomeShortcutCluster::Sharing;
    }
    return HomeShortcutCluster::Other;
  }

  float homeConcentricRadius(float outerRadius, float padding, float floorRadius) {
    return std::max(floorRadius, outerRadius - padding);
  }

  float homeMediaArtRadius(float scale) {
    return homeConcentricRadius(
        Style::scaledRadius3xl(scale), Style::spaceMd * scale, Style::scaledRadiusLg(scale)
    );
  }

  float homeShortcutTileRadius(float scale) {
    return homeConcentricRadius(
        Style::scaledRadius3xl(scale), Style::spaceXs * scale, Style::scaledRadiusLg(scale)
    );
  }

  void applyHomeClusterStyle(Flex& well, float scale, float /*fillOpacity*/) {
    applyModuleWellStyle(well, scale);
  }

  constexpr auto kHomeTransientPositionRegressionWindow = std::chrono::milliseconds(1500);
  constexpr std::int64_t kHomeTransientPositionRegressionFloorUs = 5'000'000;
  constexpr std::int64_t kHomeTransientPositionRegressionCeilingUs = 1'500'000;
  constexpr std::int64_t kHomeTransientPositionRegressionDeltaUs = 5'000'000;
  constexpr int kHomeMediaArtLayoutPassLimit = 8;

  float homeAvatarSize(float scale) { return Style::controlHeightLg * kHomeAvatarScale * scale; }

  float homeTimeFontSize(float scale, float cardInnerHeight) {
    const float minSize = Style::fontSizeHeader * 1.75F * scale;
    const float maxSize = Style::fontSizeBody * 3.8F * scale;
    if (cardInnerHeight <= 0.0F) {
      return minSize;
    }
    const float dateH = Style::fontSizeBody * 0.95F * scale;
    const float weatherH = Style::fontSizeBody * scale;
    const float gaps = Style::spaceSm * 2.0F * scale;
    const float leftover = cardInnerHeight - dateH - weatherH - gaps;
    return std::clamp(leftover * 0.78F, minSize, maxSize);
  }

  // Width for the stacked (single-column) shortcuts so each tile keeps the same width it would have
  // as one cell of the standard 2-column grid, rather than stretching across the whole column.
  float homeStackedShortcutsWidth(float contentWidth, float bottomRowGap, float wellPadding, float cellGap) {
    const float totalGrow = kHomeMainColumnFlexGrow + kHomeShortcutsFlexGrow;
    const float fullGridWidth = std::max(1.0F, contentWidth - bottomRowGap) * (kHomeShortcutsFlexGrow / totalGrow);
    const float fullGridInnerWidth = std::max(1.0F, fullGridWidth - 2.0F * wellPadding);
    const float cellWidth = std::max(
        1.0F,
        (fullGridInnerWidth - cellGap * static_cast<float>(kHomeShortcutGridColumns - 1))
            / static_cast<float>(kHomeShortcutGridColumns)
    );
    return cellWidth + 2.0F * wellPadding;
  }

  // The 2-column shortcuts grid's natural width = its flex share of the bottom row.
  float homeShortcutsGridNaturalWidth(float contentWidth, float bottomRowGap) {
    const float totalGrow = kHomeMainColumnFlexGrow + kHomeShortcutsFlexGrow;
    return std::max(1.0F, contentWidth - bottomRowGap) * (kHomeShortcutsFlexGrow / totalGrow);
  }

  std::size_t homeShortcutColumnSpan(ShortcutSize size) noexcept {
    return size == ShortcutSize::Small ? 1 : 2;
  }

  std::size_t homeShortcutRowSpan(ShortcutSize size) noexcept {
    return size == ShortcutSize::Large ? 2 : 1;
  }

  std::filesystem::path avatarStartDirectory(const AccountsService* accounts, const ConfigService* config) {
    const std::string currentPath =
        config != nullptr ? shell::resolvedAvatarPath(accounts, config->config()) : std::string{};
    const std::filesystem::path current(currentPath);
    std::error_code ec;
    if (!current.empty() && std::filesystem::exists(current, ec) && current.has_parent_path()) {
      return current.parent_path();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / "Pictures";
    }
    return {};
  }

  void openControlCenterTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  void setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return;
    }
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
  }

  std::unique_ptr<Button> makeStatusChip(
      Button** out, float scale, float surfaceOpacity, std::function<void()> onClick
  ) {
    const float height = Style::controlHeightSm * scale;
    return ui::button({
        .out = out,
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .contentAlign = ButtonContentAlign::Center,
        .variant = ButtonVariant::Ghost,
        .surfaceOpacity = surfaceOpacity,
        .minHeight = height,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceSm * scale,
        .gap = Style::spaceXs * scale,
        .radius = height * 0.5F,
        .visible = false,
        .participatesInLayout = false,
        .onClick = std::move(onClick),
        .configure = [](Button& button) { button.setSoftness(1.0F); },
    });
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

  void applyAvatarChrome(Image* avatar, bool highlighted) {
    if (avatar == nullptr) {
      return;
    }
    const float borderWidth = Style::emphasizedBorderWidth;
    if (highlighted) {
      avatar->setBorder(colorSpecFromRole(ColorRole::Hover), borderWidth);
      return;
    }
    avatar->setBorder(colorSpecFromRole(ColorRole::OnSurface, 0.45F), borderWidth);
  }

} // namespace

HomeTab::HomeTab(const ControlCenterServices& services)
    : m_mpris(services.mpris), m_httpClient(services.httpClient), m_weather(services.weather),
      m_config(services.config), m_accounts(services.accounts), m_wallpaper(services.wallpaper),
      m_thumbnails(services.thumbnails), m_asyncTextures(services.asyncTextures),
      m_services(services.shortcutServices()),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "control-center", .includedGroups = {"home"}}
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

std::unique_ptr<Flex> HomeTab::create() {
  const float scale = contentScale();
  const std::string displayName = sessionDisplayName();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
  });

  // --- User card ---
  auto userCard = ui::column({
      .out = &m_userCard,
      .justify = FlexJustify::Center,
      .fillHeight = false,
      .clipChildren = true,
      .flexGrow = 0.0F,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applyHomeCardStyle(card, scale, opacity); },
  });

  {
    const float wallpaperRadius = std::max(0.0F, Style::scaledRadius3xl(scale) - Style::borderWidth);
    userCard->addChild(
        ui::image({
            .out = &m_wallpaperPlaceholder,
            .fit = ImageFit::Cover,
            .radius = wallpaperRadius,
            .participatesInLayout = false,
            .configure = [](Image& image) { image.setZIndex(-2); },
        })
    );

    userCard->addChild(
        ui::image({
            .out = &m_wallpaperBg,
            .fit = ImageFit::Cover,
            .radius = wallpaperRadius,
            .participatesInLayout = false,
            .configure = [](Image& image) {
              image.setZIndex(-1);
              image.setOpacity(0.0F);
            },
        })
    );
  }

  const float avatarSize = homeAvatarSize(scale);
  const auto openAvatarPicker = [this]() {
    if (m_config == nullptr) {
      return;
    }

    FileDialogOptions options;
    options.mode = FileDialogMode::Open;
    options.defaultViewMode = FileDialogViewMode::Grid;
    options.title = i18n::tr("control-center.home.select-avatar");
    options.extensions = DirectoryScanner::imageExtensionFilter(false);
    options.startDirectory = avatarStartDirectory(m_accounts, m_config);

    (void)FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> pickedPath) {
      if (!pickedPath.has_value() || m_config == nullptr) {
        return;
      }
      const auto applyResult = shell::applyAvatarPath(m_accounts, m_config, pickedPath->string());
      if (applyResult.success()) {
        DeferredCall::callLater([]() {
          PanelManager::instance().refresh();
          PanelManager::instance().requestRedraw();
        });
        return;
      }
      notify::error(
          "Helix", i18n::tr("control-center.home.avatar-error-title"),
          i18n::tr(shell::avatarApplyErrorTranslationKey(applyResult.error))
      );
    });
  };

  auto avatarArea = ui::inputArea({});
  avatarArea->setSize(avatarSize, avatarSize);
  avatarArea->setHitShape(InputArea::HitShape::Circle);
  avatarArea->setFocusable(true);
  avatarArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  avatarArea->setOnClick([openAvatarPicker](const InputArea::PointerData&) { openAvatarPicker(); });
  avatarArea->setOnKeyDown([openAvatarPicker](const InputArea::KeyData& key) {
    if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
      openAvatarPicker();
    }
  });
  m_userAvatarArea = avatarArea.get();
  avatarArea->addChild(
      ui::image({
          .out = &m_userAvatar,
          .fit = ImageFit::Cover,
          .radius = avatarSize * 0.5F,
          .padding = 1.0F * scale,
          .width = avatarSize,
          .height = avatarSize,
          .configure = [](Image& image) {
            image.setBorder(colorSpecFromRole(ColorRole::OnSurface, 0.45F), Style::emphasizedBorderWidth);
            image.setHitTestVisible(false);
            image.setAsyncReadyCallback([]() { PanelManager::instance().refresh(); });
          },
      })
  );
  const auto syncAvatarChrome = [this]() {
    const bool highlighted =
        m_userAvatarArea != nullptr && (m_userAvatarArea->focused() || m_userAvatarArea->hovered());
    applyAvatarChrome(m_userAvatar, highlighted);
    PanelManager::instance().requestRedraw();
  };
  avatarArea->setOnEnter([syncAvatarChrome](const InputArea::PointerData&) { syncAvatarChrome(); });
  avatarArea->setOnLeave([syncAvatarChrome]() { syncAvatarChrome(); });
  avatarArea->setOnFocusGain(syncAvatarChrome);
  avatarArea->setOnFocusLoss(syncAvatarChrome);
  const auto configureUserDetailLabel = [scale](Label& label) {
    label.setShadow(colorSpecFromRole(ColorRole::Shadow, 0.36F), 0.0F, 1.0F * scale);
  };
  auto userRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale}, std::move(avatarArea),
      ui::column(
          {.out = &m_userMain,
           .align = FlexAlign::Stretch,
           .justify = FlexJustify::Center,
           .gap = Style::spaceXs * 0.5F * scale,
           .minHeight = avatarSize,
           .width = 0.0F,
           .height = avatarSize,
           .flexGrow = 1.0F},
          ui::label({
              .text = displayName,
              .fontSize = Style::fontSizeTitle * 1.12F * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .configure = [scale](
                               Label& label
                           ) { label.setShadow(colorSpecFromRole(ColorRole::Shadow, 0.42F), 0.0F, 1.0F * scale); },
          }),
          ui::label({
              .out = &m_userHost,
              .text = userHostLine(),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = configureUserDetailLabel,
          }),
          ui::label({
              .out = &m_userUptime,
              .text = "…",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = configureUserDetailLabel,
          })
      )
  );
  userCard->addChild(std::move(userRow));

  // Wallpaper panel: full-card keyboard target; carved pointer target leaves the avatar clickable.
  const auto openWallpaperPanel = []() { PanelManager::instance().togglePanel("wallpaper"); };
  m_userCardKeyboardArea =
      addCardOverlay(*m_userCard, openWallpaperPanel, {.keyboardFocus = true, .pointerHitTest = false});
  m_userCardArea = addCardOverlay(*m_userCard, openWallpaperPanel, {.keyboardFocus = false, .pointerHitTest = true});
  if (m_userAvatarArea != nullptr) {
    m_userAvatarArea->setTooltip(i18n::tr("control-center.home.change-avatar"));
  }
  if (m_userCardArea != nullptr) {
    m_userCardArea->setTooltip(i18n::tr("control-center.home.change-wallpaper"));
  }
  if (m_userCardKeyboardArea != nullptr) {
    m_userCardKeyboardArea->setTooltip(i18n::tr("control-center.home.change-wallpaper"));
  }

  tab->addChild(std::move(userCard));

  auto statusWell = ui::column({
      .out = &m_statusStrip,
      .fillWidth = true,
      .flexGrow = 0.0F,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale](Flex& well) { applyModuleWellStyle(well, scale); },
  });
  auto statusChips = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });
  const float chipOpacity = panelCardOpacity();
  statusChips->addChild(makeStatusChip(&m_statusUpdates, scale, chipOpacity, []() {
    openControlCenterTab("packages");
  }));
  statusChips->addChild(makeStatusChip(&m_statusCapture, scale, chipOpacity, []() {
    openControlCenterTab("screen-record");
  }));
  statusChips->addChild(makeStatusChip(&m_statusNightLight, scale, chipOpacity, []() {
    openControlCenterTab("monitor");
  }));
  statusChips->addChild(makeStatusChip(&m_statusCaffeine, scale, chipOpacity, []() {
    openControlCenterTab("power");
  }));
  statusChips->addChild(makeStatusChip(&m_statusLocalSend, scale, chipOpacity, []() {
    openControlCenterTab("localsend");
  }));
  statusWell->addChild(std::move(statusChips));
  tab->addChild(std::move(statusWell));

  auto bottomRow = ui::row({
      .out = &m_bottomRow,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });

  auto leftColumn = ui::column({
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
      .flexGrow = kHomeMainColumnFlexGrow,
  });

  // --- Media (top of left column) ---
  auto mediaCard = ui::column({
      .out = &m_mediaCard,
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = kHomeMediaCardFlexGrow,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applyHomeCardStyle(card, scale, opacity); },
  });

  const float artSize = Style::controlHeightLg * 1.22F * scale;
  auto mediaContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::column(
          {.out = &m_mediaArtSlot,
           .align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .width = artSize,
           .height = artSize},
          ui::glyph({
              .out = &m_mediaArtFallback,
              .glyph = "disc-filled",
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
      ui::column(
          {.out = &m_mediaText, .align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5F * scale, .flexGrow = 1.0F},
          ui::label({
              .out = &m_mediaTrack,
              .text = i18n::tr("control-center.home.media.nothing-playing"),
              .fontSize = Style::fontSizeBody * 0.95F * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          }),
          ui::label({
              .out = &m_mediaArtist,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .visible = false,
          })
      )
  );
  mediaCard->addChild(std::move(mediaContent));
  mediaCard->addChild(
      ui::progressBar({
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
      })
  );
  auto transport = ui::row({
      .out = &m_mediaTransport,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
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
  mediaCard->addChild(std::move(transport));

  // Clicking anywhere on the media card opens the media tab. Transport sits above this overlay.
  m_mediaCardArea = addCardOverlay(*m_mediaCard, []() { openControlCenterTab("media"); });
  if (m_mediaCardArea != nullptr) {
    m_mediaCardArea->setTooltip(i18n::tr("control-center.home.open-media"));
  }

  // --- Date/Time + Weather (below media): stacked glanceable widget ---
  auto dateTimeCard = ui::column(
      {.out = &m_dateTimeCard,
       .align = FlexAlign::Start,
       .justify = FlexJustify::Center,
       .gap = Style::spaceSm * scale,
       .fillWidth = true,
       .fillHeight = true,
       .flexGrow = kHomeDateTimeCardFlexGrow,
       .configure =
           [scale, opacity = panelCardOpacity()](Flex& card) { applyHomeClockCardStyle(card, scale, opacity); }},
      ui::label({
          .out = &m_timeLabel,
          .text = formatShellTime(m_config),
          .fontSize = homeTimeFontSize(scale, 0.0F),
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      }),
      ui::label({
          .out = &m_dateLabel,
          .text = formatShellDate(m_config),
          .fontSize = Style::fontSizeBody * 0.95F * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      }),
      ui::row(
          {.out = &m_weatherRow,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm * scale,
           .fillWidth = true},
          ui::glyph({
              .out = &m_weatherGlyph,
              .glyph = "weather-cloud-sun",
              .glyphSize = Style::fontSizeTitle * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          }),
          ui::label({
              .out = &m_weatherLine,
              .text = "",
              .fontSize = Style::fontSizeBody * 0.95F * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .flexGrow = 1.0F,
          })
      )
  );

  // Date and time open Calendar; the weather line keeps Weather.
  m_dateTimeCardArea = addCardOverlay(*m_dateTimeCard, []() { openControlCenterTab("calendar"); });
  m_weatherCardArea = addCardOverlay(
      *m_dateTimeCard, []() { openControlCenterTab("weather"); },
      {.keyboardFocus = false, .pointerHitTest = true}
  );
  if (m_dateTimeCardArea != nullptr) {
    m_dateTimeCardArea->setTooltip(i18n::tr("control-center.home.open-calendar"));
  }
  if (m_weatherCardArea != nullptr) {
    m_weatherCardArea->setZIndex(4);
    m_weatherCardArea->setTooltip(i18n::tr("control-center.home.open-weather"));
  }

  leftColumn->addChild(std::move(mediaCard));
  leftColumn->addChild(std::move(dateTimeCard));
  bottomRow->addChild(std::move(leftColumn));

  // --- Shortcuts (right of media + clock), grouped into leftover islands ---
  const auto& shortcuts =
      m_config != nullptr ? m_config->config().controlCenter.shortcuts : std::vector<ShortcutConfig>{};
  const std::size_t count = shortcuts.size();

  // Keep Shortcut instances across open/close so plugin file watches and runtimes
  // are not recreated on every Control Center open. Match by id/type from config.
  std::vector<std::unique_ptr<Shortcut>> previous;
  previous.reserve(m_shortcutPads.size());
  for (auto& pad : m_shortcutPads) {
    previous.push_back(std::move(pad.shortcut));
  }
  m_shortcutPads.clear();
  m_shortcutClusters.clear();
  m_shortcutsColumn = nullptr;

  auto takeReusable = [&previous](std::string_view type) -> std::unique_ptr<Shortcut> {
    for (auto it = previous.begin(); it != previous.end(); ++it) {
      if (*it != nullptr && (*it)->id() == type) {
        auto found = std::move(*it);
        previous.erase(it);
        return found;
      }
    }
    return nullptr;
  };

  // A plugin shortcut seeds its Luau runtime once, at construction. Plugin settings changed
  // since the last build means every reusable plugin instance is stale, so drop it and let
  // ShortcutRegistry::create() re-seed from the current config.
  const bool pluginsChanged = m_config != nullptr && !(m_config->config().plugins == m_lastPlugins);
  if (m_config != nullptr) {
    m_lastPlugins = m_config->config().plugins;
  }

  struct PendingTile {
    std::unique_ptr<Button> button;
    std::string type;
    ShortcutSize size = ShortcutSize::Small;
  };
  std::vector<PendingTile> tiles;

  for (std::size_t i = 0; i < count; ++i) {
    const auto& sc = shortcuts[i];
    auto shortcut = takeReusable(sc.type);
    if (shortcut != nullptr
        && pluginsChanged
        && scripting::isPluginEntryOfKind(sc.type, scripting::PluginEntryKind::Shortcut)) {
      shortcut.reset();
    }
    const bool reused = shortcut != nullptr;
    if (!reused) {
      shortcut = ShortcutRegistry::create(sc.type, m_services);
    }
    if (shortcut == nullptr) {
      continue;
    }
    if (reused) {
      shortcut->onPanelOpen();
    }

    const bool showLabels = m_config != nullptr ? m_config->config().controlCenter.showShortcutLabels : true;
    const std::string label = shortcut->displayLabel();
    const bool enabled = shortcut->enabled();
    const bool isActive = shortcut->active();
    const float glyphMul = sc.size == ShortcutSize::Large ? 1.45F : (sc.size == ShortcutSize::Wide ? 1.12F : 1.0F);

    const std::size_t padIdx = m_shortcutPads.size();
    auto btn = ui::button({
        .text = showLabels ? std::optional<std::string>{label} : std::nullopt,
        .glyph = shortcut->displayIcon(),
        .glyphSize = Style::fontSizeHeader * 1.4F * scale * glyphMul,
        .contentAlign = ButtonContentAlign::Center,
        .tooltip = label,
        .minHeight = 0.0F,
        .padding = Style::spaceSm * scale,
        .gap = Style::spaceXs * scale,
        .radius = homeShortcutTileRadius(scale),
        .onClick =
            [this, padIdx]() {
              if (padIdx < m_shortcutPads.size()) {
                m_shortcutPads[padIdx].shortcut->onClick();
              }
            },
        .onRightClick =
            [this, padIdx]() {
              if (padIdx < m_shortcutPads.size()) {
                m_shortcutPads[padIdx].shortcut->onRightClick();
              }
            },
        .configure =
            [enabled, isActive, showLabels, scale](Button& button) {
              button.setDirection(FlexDirection::Vertical);
              button.setJustify(FlexJustify::Center);
              if (showLabels) {
                button.setAlign(FlexAlign::Stretch);
                if (button.label() != nullptr) {
                  button.label()->setFontSize(Style::fontSizeCaption * scale);
                  button.label()->setMaxLines(1);
                  button.label()->setTextAlign(TextAlign::Center);
                }
              } else {
                button.setAlign(FlexAlign::Center);
              }
              applyShortcutButtonStyle(button, enabled, isActive);
            },
    });

    Button* btnPtr = btn.get();
    if (auto* ia = btnPtr->inputArea(); ia != nullptr) {
      ia->setOnAxisHandler([this, padIdx](const InputArea::PointerData& data) -> bool {
        if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || padIdx >= m_shortcutPads.size()) {
          return false;
        }
        const float steps = data.scrollSteps();
        if (steps == 0.0F) {
          return false;
        }
        return m_shortcutPads[padIdx].shortcut->onScroll(steps > 0.0F ? -1 : 1);
      });
    }
    ShortcutPad pad;
    pad.shortcut = std::move(shortcut);
    pad.button = btnPtr;
    pad.glyph = btnPtr->glyph();
    pad.label = btnPtr->label();
    pad.size = sc.size;
    m_shortcutPads.push_back(std::move(pad));
    tiles.push_back(PendingTile{.button = std::move(btn), .type = sc.type, .size = sc.size});
  }

  if (!tiles.empty()) {
    auto column = ui::column({
        .out = &m_shortcutsColumn,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .flexGrow = kHomeShortcutsFlexGrow,
    });

    std::array<std::vector<std::size_t>, kHomeShortcutClusterCount> buckets{};
    for (std::size_t i = 0; i < tiles.size(); ++i) {
      const auto cluster = homeClusterFor(tiles[i].type);
      buckets[static_cast<std::size_t>(cluster)].push_back(i);
    }

    std::vector<std::vector<std::size_t>> wells;
    std::vector<std::size_t> leftovers;
    wells.reserve(kHomeShortcutClusterCount);
    for (auto& bucket : buckets) {
      if (bucket.size() >= 2) {
        wells.push_back(std::move(bucket));
      } else {
        leftovers.insert(leftovers.end(), bucket.begin(), bucket.end());
      }
    }
    if (!leftovers.empty()) {
      wells.push_back(std::move(leftovers));
    }

    bool anyWideTile = false;
    for (const auto& tile : tiles) {
      if (homeShortcutColumnSpan(tile.size) > 1) {
        anyWideTile = true;
        break;
      }
    }
    const bool stacked = tiles.size() <= kHomeStackedShortcutMax && !anyWideTile;
    for (const auto& indices : wells) {
      Flex* well = nullptr;
      auto wellCard = ui::column({
          .out = &well,
          .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
            applyHomeClusterStyle(card, scale, opacity);
          },
      });
      auto grid = std::make_unique<GridView>();
      bool anyWide = false;
      for (std::size_t idx : indices) {
        if (homeShortcutColumnSpan(tiles[idx].size) > 1) {
          anyWide = true;
          break;
        }
      }
      const std::size_t cols =
          (stacked || (indices.size() == 1 && !anyWide)) ? 1U : kHomeShortcutGridColumns;
      grid->setColumns(cols);
      grid->setColumnGap(Style::spaceXs * scale);
      grid->setRowGap(Style::spaceXs * scale);
      grid->setPadding(0.0F);
      grid->setUniformCellSize(true);
      grid->setStretchItems(true);
      grid->setSquareCells(true);
      grid->setMinCellHeight(0.0F);
      grid->setSpanLastItem(!anyWide);
      GridView* gridPtr = grid.get();
      for (std::size_t idx : indices) {
        Button* tileButton = tiles[idx].button.get();
        grid->addChild(std::move(tiles[idx].button));
        grid->setChildSpan(
            tileButton, homeShortcutColumnSpan(tiles[idx].size), homeShortcutRowSpan(tiles[idx].size)
        );
      }
      well->addChild(std::move(grid));
      m_shortcutClusters.push_back(ShortcutCluster{.well = well, .grid = gridPtr, .count = indices.size()});
      column->addChild(std::move(wellCard));
    }

    if (stacked) {
      column->setFlexGrow(0.0F);
    }
    bottomRow->addChild(std::move(column));
  }
  tab->addChild(std::move(bottomRow));

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
          .out = &m_sessionButton,
          .glyph = "shutdown",
          .tooltip = i18n::tr("control-center.home.open-power"),
          .onClick = []() { openControlCenterTab("power"); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  syncHeaderActions();
  return m_settings->embedHeader(std::move(live), scale);
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
  const float userCardHeight = homeAvatarSize(scale) + 2.0F * Style::spaceMd * scale;
  const bool statusVisible = m_statusStrip != nullptr && m_statusStrip->visible();
  const float statusHeight =
      statusVisible ? Style::controlHeightSm * scale + 2.0F * Style::spaceXs * scale : 0.0F;
  if (m_statusStrip != nullptr) {
    m_statusStrip->setParticipatesInLayout(statusVisible);
    m_statusStrip->setFlexGrow(0.0F);
    m_statusStrip->setMinHeight(statusHeight);
  }
  if (m_userCard != nullptr) {
    m_userCard->setFlexGrow(0.0F);
    m_userCard->setFillHeight(false);
    m_userCard->setMinHeight(userCardHeight);
    m_userCard->setMaxHeight(userCardHeight);
  }
  if (m_bottomRow != nullptr) {
    m_bottomRow->setFlexGrow(1.0F);
  }

  if (m_shortcutsColumn != nullptr && !m_shortcutPads.empty()) {
    const float bottomRowGap = m_bottomRow != nullptr ? m_bottomRow->gap() : 0.0F;
    bool anyWideTile = false;
    for (const auto& pad : m_shortcutPads) {
      if (homeShortcutColumnSpan(pad.size) > 1) {
        anyWideTile = true;
        break;
      }
    }
    const bool stacked = m_shortcutPads.size() <= kHomeStackedShortcutMax && !anyWideTile;
    const float wellPad = Style::spaceXs * scale;
    const float cellGap = Style::spaceXs * scale;

    float columnWidth = stacked ? homeStackedShortcutsWidth(contentWidth, bottomRowGap, wellPad, cellGap)
                                : homeShortcutsGridNaturalWidth(contentWidth, bottomRowGap);

    if (stacked) {
      m_shortcutsColumn->setFlexGrow(0.0F);
      m_shortcutsColumn->setSize(columnWidth, m_shortcutsColumn->height());
    } else {
      m_shortcutsColumn->setFlexGrow(kHomeShortcutsFlexGrow);
    }
  }
  m_rootLayout->setMinHeight(0.0F);
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  // Cap shortcut labels to the button's content width after cells are sized (avoids elide from grid math mismatch).
  if (!m_shortcutPads.empty()) {
    for (auto& pad : m_shortcutPads) {
      if (pad.label == nullptr) {
        continue;
      }
      float inner = 1.0F;
      if (pad.button != nullptr && pad.button->width() > 1.0F) {
        inner = std::max(1.0F, pad.button->width() - pad.button->paddingLeft() - pad.button->paddingRight());
      } else if (!m_shortcutClusters.empty() && m_shortcutClusters.front().grid != nullptr) {
        GridView* grid = m_shortcutClusters.front().grid;
        const float gridW = grid->width();
        const float innerGrid = std::max(1.0F, gridW - grid->paddingLeft() - grid->paddingRight());
        const std::size_t cols = std::max<std::size_t>(1, std::min(grid->columns(), m_shortcutClusters.front().count));
        const float cellWidth =
            (innerGrid - static_cast<float>(cols - 1) * grid->columnGap()) / static_cast<float>(cols);
        inner = std::max(1.0F, cellWidth - 2.0F * Style::spaceXs * scale);
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
  // Grow the album art square to fill the media card height so the row feels balanced
  // when the card flex-grows. A later bottom-row min-height pass can change the card
  // height, so this runs again after that final layout pass below.
  resizeMediaArtToCard();

  // Keep the media card height stable: one ellipsized line each, capped to the
  // text column width. Wrapping a long title used to grow this card and stretch
  // the adjacent shortcut buttons (same bottom-row height).
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

  // Lock the shortcuts grid height to its square-cell natural size so it does not vary
  // when the media or clock cards change. The leftColumn stretches to match this height.
  float contentH = bodyHeight;
  if (m_shortcutsColumn != nullptr && !m_shortcutPads.empty()) {
    float columnH = 0.0F;
    float cellSide = 0.0F;
    const float clusterGap = m_shortcutsColumn->gap();
    for (std::size_t i = 0; i < m_shortcutClusters.size(); ++i) {
      auto& cluster = m_shortcutClusters[i];
      if (cluster.grid == nullptr || cluster.well == nullptr || cluster.count == 0) {
        continue;
      }
      const float innerGridW =
          std::max(1.0F, cluster.well->width() - cluster.well->paddingLeft() - cluster.well->paddingRight());
      const std::size_t cols = std::max<std::size_t>(1, cluster.grid->columns());
      const std::size_t rows = std::max<std::size_t>(1, cluster.grid->packedRowCount());
      const float cellWidth = std::max(
          1.0F, (innerGridW - static_cast<float>(cols - 1) * cluster.grid->columnGap()) / static_cast<float>(cols)
      );
      cellSide = cellWidth * kHomeShortcutSquareTrim;
      const float gridH = std::round(
          static_cast<float>(rows) * cellSide
          + static_cast<float>(rows > 0 ? rows - 1 : 0) * cluster.grid->rowGap()
      );
      const float wellH = gridH + cluster.well->paddingTop() + cluster.well->paddingBottom();
      cluster.well->setMinHeight(wellH);
      cluster.well->setMaxHeight(wellH);
      columnH += wellH;
      if (i + 1 < m_shortcutClusters.size()) {
        columnH += clusterGap;
      }
    }

    const bool showLabels = m_config != nullptr ? m_config->config().controlCenter.showShortcutLabels : true;
    for (auto& pad : m_shortcutPads) {
      if (pad.glyph == nullptr) {
        continue;
      }
      const float glyphMul = pad.size == ShortcutSize::Large ? 1.45F : (pad.size == ShortcutSize::Wide ? 1.12F : 1.0F);
      if (showLabels) {
        pad.glyph->setGlyphSize(Style::fontSizeHeader * 1.4F * scale * glyphMul);
      } else if (cellSide > 0.0F) {
        const float dynamicGlyphSize = std::clamp(cellSide * 0.38F * glyphMul, 24.0F * scale, 72.0F * scale);
        pad.glyph->setGlyphSize(dynamicGlyphSize);
      }
      if (pad.button != nullptr && cellSide > 0.0F) {
        pad.button->setRadius(cellSide * 0.5F);
      }
    }

    if (m_bottomRow != nullptr) {
      m_bottomRow->setMinHeight(std::round(columnH));
    }

    if (m_mediaCard != nullptr && m_dateTimeCard != nullptr) {
      const float colGap = Style::spaceMd * contentScale();
      const float avail = std::max(0.0F, std::round(columnH) - colGap);
      float mediaH = 0.0F;
      float dateH = 0.0F;
      if (mediaExpanded()) {
        const float cardGrowTotal = kHomeMediaCardFlexGrow + kHomeDateTimeCardFlexGrow;
        mediaH = std::round(avail * (kHomeMediaCardFlexGrow / cardGrowTotal));
        dateH = std::max(0.0F, avail - mediaH);
      } else {
        const float compact = std::round(
            Style::controlHeightLg * 1.22F * scale + m_mediaCard->paddingTop() + m_mediaCard->paddingBottom()
        );
        mediaH = std::min(compact, std::round(avail * 0.42F));
        dateH = std::max(0.0F, avail - mediaH);
      }

      m_mediaCard->setMinHeight(mediaH);
      m_mediaCard->setMaxHeight(mediaH);
      m_dateTimeCard->setMinHeight(dateH);
      m_dateTimeCard->setMaxHeight(dateH);
    }

    float naturalH = userCardHeight;
    if (statusVisible) {
      naturalH += statusHeight;
    }
    naturalH += std::round(columnH);
    const float parts = 2.0F + (statusVisible ? 1.0F : 0.0F);
    naturalH += (parts - 1.0F) * m_rootLayout->gap();
    contentH = std::max(bodyHeight, naturalH);
  }
  m_rootLayout->setMinHeight(contentH);
  m_rootLayout->setSize(contentWidth, contentH);

  if (m_timeLabel != nullptr && m_dateTimeCard != nullptr) {
    const float clockH = std::max(m_dateTimeCard->minHeight(), m_dateTimeCard->height());
    const float innerH =
        std::max(0.0F, clockH - m_dateTimeCard->paddingTop() - m_dateTimeCard->paddingBottom());
    float size = homeTimeFontSize(scale, innerH);
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
  const float minArt = Style::controlHeightLg * 1.22F * scale;
  if (!mediaExpanded()) {
    if (std::abs(m_mediaArtSlot->width() - minArt) <= 0.5F) {
      return false;
    }
    m_mediaArtSlot->setSize(minArt, minArt);
    m_mediaArt->setSize(minArt, minArt);
    m_mediaArt->setRadius(homeMediaArtRadius(scale));
    if (m_mediaArtFallback != nullptr) {
      m_mediaArtFallback->setGlyphSize(minArt * 0.55F);
    }
    return true;
  }

  const float maxArt = Style::controlHeightLg * 2.6F * scale;
  const float available =
      std::max(0.0F, m_mediaCard->height() - m_mediaCard->paddingTop() - m_mediaCard->paddingBottom());
  const float desired = std::clamp(available, minArt, maxArt);
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
    // Ready on the first look (cached) — snap in without a crossfade.
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

  const bool any = (m_statusUpdates != nullptr && m_statusUpdates->visible())
      || (m_statusCapture != nullptr && m_statusCapture->visible())
      || (m_statusNightLight != nullptr && m_statusNightLight->visible())
      || (m_statusCaffeine != nullptr && m_statusCaffeine->visible())
      || (m_statusLocalSend != nullptr && m_statusLocalSend->visible());
  if (m_statusStrip != nullptr) {
    const bool wasShown = m_statusStrip->visible();
    setShown(m_statusStrip, any);
    m_statusStrip->setMinHeight(any ? Style::controlHeightSm * contentScale() + 2.0F * Style::spaceXs * contentScale() : 0.0F);
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
  m_bottomRow = nullptr;
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
  m_userHost = nullptr;
  m_userUptime = nullptr;
  m_sessionButton = nullptr;
  m_statusUpdates = nullptr;
  m_statusCapture = nullptr;
  m_statusNightLight = nullptr;
  m_statusCaffeine = nullptr;
  m_statusLocalSend = nullptr;
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
  m_shortcutClusters.clear();
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
  for (auto& cluster : m_shortcutClusters) {
    if (cluster.well != nullptr) {
      applyHomeClusterStyle(*cluster.well, scale, opacity);
    }
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
  for (Label* label : {m_userHost, m_userUptime}) {
    if (label != nullptr) {
      label->setFontSize(Style::fontSizeCaption * s);
    }
  }
  if (m_mediaTrack != nullptr) {
    m_mediaTrack->setFontSize(Style::fontSizeBody * 0.95F * s);
  }
  if (m_mediaArtist != nullptr) {
    m_mediaArtist->setFontSize(Style::fontSizeCaption * s);
  }
  for (auto& pad : m_shortcutPads) {
    if (pad.label != nullptr) {
      pad.label->setFontSize(Style::fontSizeCaption * s);
    }
    if (pad.glyph != nullptr) {
      // Icon-only glyphs are sized from cell side in doLayout.
      if (m_config == nullptr || m_config->config().controlCenter.showShortcutLabels) {
        pad.glyph->setGlyphSize(Style::fontSizeHeader * 1.4F * s);
      }
    }
  }
}

void HomeTab::sync(Renderer& renderer) {
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
