#include "shell/quick_settings/quick_settings_panel.h"

#include "config/config_service.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "render/core/renderer.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "shell/session/session_action_runner.h"
#include "system/brightness_service.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

  using Clock = std::chrono::steady_clock;
  constexpr auto kSliderHold = std::chrono::milliseconds(600);

  // Shown when [control_center].shortcuts is empty.
  const std::vector<std::string> kDefaultTiles = {
      "wifi", "bluetooth", "power_profile", "nightlight", "dark_mode", "notification", "caffeine", "mic_mute",
  };

  [[nodiscard]] bool recently(Clock::time_point stamp) { return Clock::now() - stamp < kSliderHold; }

} // namespace

QuickSettingsPanel::QuickSettingsPanel(
    ConfigService* config, ShortcutServices shortcuts, PipeWireService* audio, BrightnessService* brightness,
    SessionActionRunner* session
)
    : m_config(config), m_shortcuts(shortcuts), m_audio(audio), m_brightness(brightness), m_session(session) {}

QuickSettingsPanel::~QuickSettingsPanel() = default;

bool QuickSettingsPanel::hasBrightness() const { return m_brightness != nullptr && m_brightness->available(); }

std::vector<std::string> QuickSettingsPanel::tileTypes() const {
  std::vector<std::string> types;
  if (m_config != nullptr) {
    for (const auto& configured : m_config->config().controlCenter.shortcuts) {
      types.push_back(configured.type);
    }
  }
  return types.empty() ? kDefaultTiles : types;
}

void QuickSettingsPanel::buildTiles() {
  m_tiles.clear();
  for (const auto& type : tileTypes()) {
    if (m_config != nullptr && !ShortcutRegistry::isAvailable(type, m_config->config())) {
      continue;
    }
    auto shortcut = ShortcutRegistry::create(type, m_shortcuts);
    if (shortcut == nullptr || !shortcut->enabled()) {
      continue;
    }
    Tile tile;
    tile.shortcut = std::move(shortcut);
    m_tiles.push_back(std::move(tile));
  }
}

float QuickSettingsPanel::preferredWidth() const { return scaled(kWidth); }

float QuickSettingsPanel::preferredHeight() const {
  const std::size_t tiles = m_tiles.empty() ? tileTypes().size() : m_tiles.size();
  const std::size_t rows = (tiles + 1) / 2;
  float h = kPad * 2.0F;
  h += kRow + kGap; // volume
  if (hasBrightness()) {
    h += kRow + kGap;
  }
  if (rows > 0) {
    h += static_cast<float>(rows) * kTile + static_cast<float>(rows - 1) * kGap + kGap;
  }
  h += kRow; // footer
  return std::ceil(scaled(h));
}

void QuickSettingsPanel::create() {
  const float scale = contentScale();
  buildTiles();
  m_footerButtons.clear();

  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = kGap * scale,
      .padding = kPad * scale,
  });

  const auto sliderRow = [&](const char* glyph, Glyph** glyphOut, Slider** out, std::function<void(double)> onChange) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = kRow * scale},
        ui::glyph({.out = glyphOut, .glyph = glyph, .glyphSize = Style::baseGlyphSize * scale}),
        ui::slider({
            .out = out,
            .minValue = 0.0,
            .maxValue = 100.0,
            .step = 1.0,
            .value = 0.0,
            .flexGrow = 1.0F,
            .onValueChanged = std::move(onChange),
        })
    );
  };

  if (m_audio != nullptr) {
    root->addChild(sliderRow("volume", &m_volumeGlyph, &m_volume, [this](double value) {
      m_volumeTouched = Clock::now();
      if (m_audio != nullptr) {
        m_audio->setVolume(static_cast<float>(value / 100.0));
      }
    }));
  }
  if (hasBrightness()) {
    root->addChild(sliderRow("sun", nullptr, &m_brightnessSlider, [this](double value) {
      m_brightnessTouched = Clock::now();
      if (m_brightness != nullptr) {
        m_brightness->setAllBrightness(static_cast<float>(value / 100.0));
      }
    }));
  }

  if (!m_tiles.empty()) {
    auto grid = ui::column({.align = FlexAlign::Stretch, .gap = kGap * scale});
    for (std::size_t i = 0; i < m_tiles.size(); i += 2) {
      auto row = ui::row({.align = FlexAlign::Stretch, .gap = kGap * scale});
      for (std::size_t j = i; j < std::min(i + 2, m_tiles.size()); ++j) {
        Tile& tile = m_tiles[j];
        Shortcut* shortcut = tile.shortcut.get();
        auto button = ui::button({
            .text = shortcut->displayLabel(),
            .glyph = shortcut->displayIcon(),
            .glyphSize = Style::baseGlyphSize * scale,
            .contentAlign = ButtonContentAlign::Start,
            .variant = shortcut->active() ? ButtonVariant::Primary : ButtonVariant::Default,
            .surfaceOpacity = panelCardOpacity(),
            .minHeight = kTile * scale,
            .paddingH = Style::spaceMd * scale,
            .gap = Style::spaceSm * scale,
            .radius = Style::scaledRadiusLg(scale),
            .flexGrow = 1.0F,
            .onClick =
                [shortcut]() {
                  shortcut->onClick();
                  PanelManager::instance().requestUpdateOnly();
                },
            .onRightClick = [shortcut]() { shortcut->onRightClick(); },
        });
        tile.button = button.get();
        tile.label = shortcut->displayLabel();
        tile.icon = shortcut->displayIcon();
        tile.active = shortcut->active();
        row->addChild(std::move(button));
      }
      if (m_tiles.size() - i == 1) {
        row->addChild(ui::box({.flexGrow = 1.0F}));
      }
      grid->addChild(std::move(row));
    }
    root->addChild(std::move(grid));
  }

  auto footer = ui::row({.align = FlexAlign::Center, .gap = kGap * scale, .minHeight = kRow * scale});
  const auto footerButton = [&](const char* glyph, std::string tooltip, std::function<void()> onClick) {
    auto button = ui::button({
        .glyph = glyph,
        .glyphSize = Style::baseGlyphSize * scale,
        .variant = ButtonVariant::Default,
        .surfaceOpacity = panelCardOpacity(),
        .tooltip = std::move(tooltip),
        .minWidth = kRow * scale,
        .minHeight = kRow * scale,
        .radius = Style::scaledRadiusLg(scale),
        .onClick = std::move(onClick),
    });
    m_footerButtons.push_back(button.get());
    footer->addChild(std::move(button));
  };
  footerButton("settings", i18n::tr("control-center.open-settings"), []() {
    PanelManager::instance().closePanel(false);
    PanelManager::instance().openSettings();
  });
  footer->addChild(ui::box({.flexGrow = 1.0F}));
  if (m_session != nullptr) {
    footerButton("lock", i18n::tr("session.actions.lock"), [this]() {
      PanelManager::instance().closePanel(false);
      if (m_session != nullptr) {
        (void)m_session->lock();
      }
    });
  }
  footerButton("power", i18n::tr("launcher.providers.panel.builtin.session"), []() {
    PanelManager::instance().closePanel(false);
    PanelManager::instance().togglePanel("session");
  });
  root->addChild(std::move(footer));

  setRoot(std::move(root));
  if (m_animations != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }
  syncSliders();
}

void QuickSettingsPanel::onOpen(std::string_view /*context*/) {
  m_volumeTouched = {};
  m_brightnessTouched = {};
}

void QuickSettingsPanel::onClose() {
  m_root = nullptr;
  m_volume = nullptr;
  m_volumeGlyph = nullptr;
  m_brightnessSlider = nullptr;
  m_tiles.clear();
  m_footerButtons.clear();
  clearReleasedRoot();
}

void QuickSettingsPanel::onPanelCardOpacityChanged(float opacity) {
  for (auto& tile : m_tiles) {
    if (tile.button != nullptr) {
      tile.button->setSurfaceOpacity(opacity);
    }
  }
  for (Button* button : m_footerButtons) {
    button->setSurfaceOpacity(opacity);
  }
}

void QuickSettingsPanel::syncSliders() {
  if (m_volume != nullptr && m_audio != nullptr && !recently(m_volumeTouched)) {
    const AudioNode* sink = m_audio->defaultSink();
    if (sink != nullptr) {
      m_volume->setValue(std::clamp(static_cast<double>(sink->volume) * 100.0, 0.0, 100.0));
      if (m_volumeGlyph != nullptr) {
        m_volumeGlyph->setGlyph(sink->muted ? "volume-off" : "volume");
      }
    }
  }
  if (m_brightnessSlider != nullptr && hasBrightness() && !recently(m_brightnessTouched)) {
    const auto& displays = m_brightness->displays();
    const auto it = std::ranges::find_if(displays, [](const BrightnessDisplay& d) { return d.controllable; });
    if (it != displays.end()) {
      m_brightnessSlider->setValue(std::clamp(static_cast<double>(it->brightness) * 100.0, 0.0, 100.0));
    }
  }
}

void QuickSettingsPanel::syncTiles() {
  for (auto& tile : m_tiles) {
    if (tile.button == nullptr) {
      continue;
    }
    const std::string label = tile.shortcut->displayLabel();
    const std::string icon = tile.shortcut->displayIcon();
    const bool active = tile.shortcut->active();
    if (label != tile.label) {
      tile.label = label;
      tile.button->setText(label);
    }
    if (icon != tile.icon) {
      tile.icon = icon;
      tile.button->setGlyph(icon);
    }
    if (active != tile.active) {
      tile.active = active;
      tile.button->setVariant(active ? ButtonVariant::Primary : ButtonVariant::Default);
    }
  }
}

void QuickSettingsPanel::doUpdate(Renderer& /*renderer*/) {
  syncSliders();
  syncTiles();
}

void QuickSettingsPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_root == nullptr) {
    return;
  }
  m_root->setSize(width, height);
  m_root->layout(renderer);
  for (auto& tile : m_tiles) {
    if (tile.button != nullptr) {
      tile.button->updateInputArea();
    }
  }
  for (Button* button : m_footerButtons) {
    button->updateInputArea();
  }
}
