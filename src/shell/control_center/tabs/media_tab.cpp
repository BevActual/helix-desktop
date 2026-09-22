#include "shell/control_center/tabs/media_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/select.h"
#include "ui/visuals/audio_visualizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace control_center;
using namespace mpris;

namespace {

  const Logger kLog{"media_tab"};

  constexpr float kMediaUnit = 36.0F;
  constexpr float kArtworkMin = kMediaUnit * 4;
  constexpr float kArtworkMax = kMediaUnit * 12;
  constexpr float kVisualizerStripHeight = kMediaUnit + Style::spaceSm;
  constexpr float kMediaControlsHeight = kMediaUnit + Style::spaceXs;
  constexpr float kMediaPlayPauseHeight = kMediaUnit + Style::spaceSm;
  constexpr auto kNoActivePlayerGrace = std::chrono::milliseconds(2000);
  constexpr auto kTransientPositionRegressionWindow = std::chrono::milliseconds(1500);
  constexpr std::int64_t kTransientPositionRegressionFloorUs = 5'000'000;
  constexpr std::int64_t kTransientPositionRegressionCeilingUs = 1'500'000;
  constexpr std::int64_t kTransientPositionRegressionDeltaUs = 5'000'000;
  constexpr std::int64_t kSeekArrivedToleranceUs = 1'500'000;
  constexpr std::int64_t kSeekNearZeroUs = 2'000'000;
  constexpr auto kProgressSettleHold = std::chrono::milliseconds(2500);
  constexpr auto kPendingSeekTimeout = std::chrono::milliseconds(5000);
  constexpr int kVisualizerBandCount = 32;

  std::string playPauseGlyph(const std::string& playbackStatus) {
    return playbackStatus == "Playing" ? "media-pause" : "media-play";
  }

  [[nodiscard]] int mediaTabArtDecodeSize(float scale) {
    return static_cast<int>(std::round(kArtworkMax * scale));
  }

  std::string repeatGlyph(const std::string& loopStatus) { return loopStatus == "Track" ? "repeat-once" : "repeat"; }

  ButtonVariant toggleVariant(bool active) { return active ? ButtonVariant::Primary : ButtonVariant::Ghost; }

  [[nodiscard]] std::string_view repeatTooltipKey(const std::string& loopStatus) {
    if (loopStatus == "Playlist") {
      return "control-center.media.repeat-all";
    }
    if (loopStatus == "Track") {
      return "control-center.media.repeat-one";
    }
    return "control-center.media.repeat";
  }

  [[nodiscard]] std::string formatMediaClock(std::int64_t microseconds) {
    return formatClockTime(std::max<std::int64_t>(0, microseconds) / 1'000'000);
  }

  [[nodiscard]] std::string formatVolumePercent(double percent) {
    return std::format("{}%", static_cast<int>(std::lround(percent)));
  }

  [[nodiscard]] std::string hideTokenFor(const MprisPlayerInfo& player) {
    if (!player.desktopEntry.empty()) {
      return player.desktopEntry;
    }
    if (!player.identity.empty()) {
      return player.identity;
    }
    return player.busName;
  }

  bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

} // namespace

MediaTab::MediaTab(
    MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, const ControlCenterServices& services
)
    : m_mpris(mpris), m_httpClient(httpClient), m_spectrum(spectrum), m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "services", .includedGroups = {"media"}}
      )) {}

MediaTab::~MediaTab() { m_aliveGuard.reset(); }

void MediaTab::selectPlayer(std::size_t index) {
  if (m_mpris == nullptr) {
    return;
  }
  if (index == 0) {
    m_mpris->clearPinnedPlayerPreference();
  } else if (index - 1 < m_playerBusNames.size()) {
    m_mpris->setPinnedPlayerPreference(m_playerBusNames[index - 1]);
  }
  PanelManager::instance().refresh();
}

void MediaTab::syncPlayerSelect(const std::vector<MprisPlayerInfo>& players) {
  std::vector<std::string> playerBusNames;
  playerBusNames.reserve(players.size());
  std::vector<std::string> options;
  options.reserve(players.size() + 1);
  options.push_back(i18n::tr("control-center.media.automatic"));
  for (const auto& player : players) {
    playerBusNames.push_back(player.busName);
    options.push_back(player.identity.empty() ? player.busName : player.identity);
  }

  const bool busesChanged = m_playerBusNames != playerBusNames;
  m_playerBusNames = std::move(playerBusNames);
  if (m_playerSelect != nullptr && (busesChanged || m_playerSelect->selectedIndex() >= options.size())) {
    m_playerSelect->setOptions(std::move(options));
  }

  if (m_playerSelect != nullptr && !m_playerBusNames.empty()) {
    const auto pinnedBusName = m_mpris != nullptr ? m_mpris->pinnedPlayerPreference() : std::nullopt;
    std::size_t selected = 0;
    if (pinnedBusName.has_value()) {
      for (std::size_t i = 0; i < m_playerBusNames.size(); ++i) {
        if (m_playerBusNames[i] == *pinnedBusName) {
          selected = i + 1;
          break;
        }
      }
    }
    if (m_playerSelect->selectedIndex() != selected) {
      m_playerSelect->setSelectedIndexSilently(selected);
    }
  }
}

bool MediaTab::syncPlaybackChrome(bool hasPlayer, bool showVisualizer) {
  bool dirty = false;
  m_stageExpanded = hasPlayer;
  dirty |= setShown(m_playerSelectHost, !m_playerBusNames.empty());
  dirty |= setShown(m_progressRow, hasPlayer);
  dirty |= setShown(m_controlsHost, hasPlayer);
  dirty |= setShown(m_volumeHost, hasPlayer);
  dirty |= setShown(m_visualizerHost, showVisualizer);
  if (m_nowCard != nullptr) {
    if (std::abs(m_nowCard->flexGrow()) > 0.01F) {
      m_nowCard->setFlexGrow(0.0F);
      dirty = true;
    }
  }
  dirty |= setShown(m_artworkWell, hasPlayer);
  if (m_artworkWell != nullptr) {
    const float grow = hasPlayer ? 1.0F : 0.0F;
    if (std::abs(m_artworkWell->flexGrow() - grow) > 0.01F) {
      m_artworkWell->setFlexGrow(grow);
      dirty = true;
    }
  }
  if (m_artworkHost != nullptr) {
    const float grow = hasPlayer ? 1.0F : 0.0F;
    if (std::abs(m_artworkHost->flexGrow() - grow) > 0.01F) {
      m_artworkHost->setFlexGrow(grow);
      dirty = true;
    }
  }
  if (m_artworkHit != nullptr) {
    m_artworkHit->setHitTestVisible(hasPlayer);
    m_artworkHit->setEnabled(hasPlayer);
  }
  if (m_playPauseButton != nullptr) {
    m_playPauseButton->setEnabled(hasPlayer);
  }
  if (m_repeatButton != nullptr) {
    m_repeatButton->setEnabled(hasPlayer);
  }
  if (m_shuffleButton != nullptr) {
    m_shuffleButton->setEnabled(hasPlayer);
  }
  if (m_hidePlayerButton != nullptr) {
    m_hidePlayerButton->setEnabled(hasPlayer);
  }
  return dirty;
}

void MediaTab::pinArtwork() {
  if (m_artworkSlot == nullptr) {
    return;
  }
  if (m_artwork != nullptr) {
    m_artwork->setPosition(0.0F, 0.0F);
    m_artwork->setSize(m_artworkSlot->width(), m_artworkSlot->height());
  }
  if (m_artworkHit != nullptr) {
    m_artworkHit->setPosition(0.0F, 0.0F);
    m_artworkHit->setSize(m_artworkSlot->width(), m_artworkSlot->height());
  }
}

void MediaTab::layoutArtworkSlot(float scale) {
  if (m_artworkSlot == nullptr) {
    return;
  }
  const float minSide = kArtworkMin * scale;
  const float maxSide = kArtworkMax * scale;
  float side = minSide;
  if (m_stageExpanded && m_artworkHost != nullptr) {
    const float innerWidth =
        std::max(1.0F, m_artworkHost->width() - (m_artworkHost->paddingLeft() + m_artworkHost->paddingRight()));
    const float innerHeight =
        std::max(1.0F, m_artworkHost->height() - (m_artworkHost->paddingTop() + m_artworkHost->paddingBottom()));
    side = std::clamp(std::min(innerWidth, innerHeight), minSide, maxSide);
  }
  m_artworkSlot->setSize(side, side);
  m_artworkSlot->setMinWidth(side);
  m_artworkSlot->setMinHeight(side);
  m_artworkSlot->setMaxWidth(side);
  m_artworkSlot->setMaxHeight(side);
  m_artworkSlot->setRadius(Style::scaledRadiusXl(scale));
  if (m_artworkFallback != nullptr) {
    m_artworkFallback->setGlyphSize(side * 0.45F);
  }
  if (m_artwork != nullptr) {
    m_artwork->setRadius(Style::scaledRadiusXl(scale));
  }
}

void MediaTab::openPlayer() {
  if (m_mpris == nullptr) {
    return;
  }
  (void)m_mpris->raiseActive();
}

void MediaTab::hideActivePlayer() {
  if (m_config == nullptr || m_mpris == nullptr) {
    return;
  }
  const auto active = m_mpris->activePlayer();
  if (!active.has_value()) {
    return;
  }
  const std::string token = hideTokenFor(*active);
  if (token.empty()) {
    return;
  }
  auto blacklist = m_config->config().shell.mpris.blacklist;
  if (std::ranges::find(blacklist, token) != blacklist.end()) {
    return;
  }
  blacklist.push_back(token);
  (void)m_config->setOverride({"shell", "mpris", "blacklist"}, std::move(blacklist));
}

void MediaTab::commitVolume(double percent) {
  if (m_syncingVolume || m_mpris == nullptr) {
    return;
  }
  const int rounded = static_cast<int>(std::lround(std::clamp(percent, 0.0, 100.0)));
  if (rounded == m_lastSentVolumePercent) {
    return;
  }
  m_lastSentVolumePercent = rounded;
  (void)m_mpris->setVolumeActive(static_cast<double>(rounded) / 100.0);
}

std::unique_ptr<Flex> MediaTab::create() {
  const float scale = contentScale();
  const float artSize = kArtworkMin * scale;
  const float captionSize = Style::fontSizeCaption * scale;
  const float controlHeight = Style::controlHeight * scale;

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto nowCard = ui::column({
      .out = &m_nowCard,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applyCompactCardStyle(card, scale, opacity, false); },
  });

  auto playerHost = ui::column({
      .out = &m_playerSelectHost,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
  });
  auto playerRow = makeControlRow(i18n::tr("control-center.media.player"), scale, nullptr, false);
  playerRow->addChild(
      ui::button({
          .out = &m_hidePlayerButton,
          .glyph = "eye-off",
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.media.hide-player"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick = [this]() { hideActivePlayer(); },
      })
  );
  playerRow->addChild(
      ui::select({
          .out = &m_playerSelect,
          .options = std::vector<std::string>{i18n::tr("control-center.media.automatic")},
          .selectedIndex = std::size_t{0},
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = controlHeight,
          .surfaceOpacity = panelCardOpacity(),
          .onSelectionChanged =
              [this](std::size_t index, std::string_view) { selectPlayer(index); },
      })
  );
  playerHost->addChild(std::move(playerRow));
  nowCard->addChild(std::move(playerHost));

  nowCard->addChild(
      ui::column(
          {.out = &m_metaColumn, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .fillWidth = true},
          ui::label({
              .out = &m_trackTitle,
              .text = i18n::tr("control-center.media.nothing-playing"),
              .fontSize = Style::fontSizeTitle * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
              .ellipsize = TextEllipsize::End,
              .autoScroll = true,
              .autoScrollOnlyWhenHovered = true,
          }),
          ui::label({
              .out = &m_trackArtist,
              .text = i18n::tr("control-center.media.start-playback"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          }),
          ui::label({
              .out = &m_trackAlbum,
              .text = "",
              .fontSize = captionSize,
              .color = colorSpecFromRole(ColorRole::Secondary),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .visible = false,
              .participatesInLayout = false,
          })
      )
  );

  auto progressRow = ui::row({
      .out = &m_progressRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
  });
  progressRow->addChild(
      ui::label({
          .out = &m_elapsedLabel,
          .text = formatClockTime(0),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .minWidth = Style::controlHeight * scale,
      })
  );
  progressRow->addChild(
      ui::slider({
          .out = &m_progressSlider,
          .minValue = 0.0F,
          .maxValue = 100.0F,
          .step = 1.0F,
          .trackHeight = 7.0F * scale,
          .thumbSize = 16.0F * scale,
          .controlHeight = (Style::controlHeight + Style::spaceXs) * scale,
          .flexGrow = 1.0F,
          .onValueChanged =
              [this](double value) {
                if (m_syncingProgress || m_mpris == nullptr) {
                  return;
                }
                const auto active = m_mpris->activePlayer();
                const auto targetUs = static_cast<std::int64_t>(std::llround(value * 1000000.0));
                const auto now = std::chrono::steady_clock::now();
                m_positionUs = targetUs;
                m_positionSampleAt = now;
                m_pendingSeekBusName = active.has_value()
                    ? active->busName
                    : (!m_positionBusName.empty() ? m_positionBusName : std::string{});
                m_pendingSeekUs = targetUs;
                m_pendingSeekUntil = now + kPendingSeekTimeout;
                m_progressSettleUntil = now + kProgressSettleHold;
                if (m_elapsedLabel != nullptr) {
                  m_elapsedLabel->setText(formatMediaClock(targetUs));
                }
                if (m_remainingLabel != nullptr && m_progressSlider != nullptr) {
                  const auto lengthUs = static_cast<std::int64_t>(std::llround(m_progressSlider->maxValue() * 1000000.0));
                  m_remainingLabel->setText("-" + formatMediaClock(lengthUs - targetUs));
                }
              },
          .onDragEnd =
              [this]() {
                if (m_syncingProgress || m_mpris == nullptr || m_progressSlider == nullptr) {
                  return;
                }
                commitPendingSeek(m_progressSlider->value());
              },
      })
  );
  progressRow->addChild(
      ui::label({
          .out = &m_remainingLabel,
          .text = "-" + formatClockTime(0),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .minWidth = Style::controlHeight * scale,
      })
  );
  nowCard->addChild(std::move(progressRow));

  auto visualizerHost = ui::column({
      .out = &m_visualizerHost,
      .align = FlexAlign::Stretch,
      .minHeight = kVisualizerStripHeight * scale,
      .maxHeight = kVisualizerStripHeight * scale,
      .fillWidth = true,
      .clipChildren = true,
      .visible = false,
      .participatesInLayout = false,
  });
  auto visualizerBody = ui::row({
      .out = &m_visualizerBody,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });
  auto visualizerSpectrum = std::make_unique<AudioVisualizer>();
  visualizerSpectrum->setGradient(colorForRole(ColorRole::Secondary), colorForRole(ColorRole::Tertiary));
  visualizerSpectrum->setOrientation(AudioSpectrumOrientation::Horizontal);
  visualizerSpectrum->setMirrored(false);
  visualizerSpectrum->setCentered(false);
  visualizerSpectrum->setValues(std::vector<float>(kVisualizerBandCount, 0.0F));
  visualizerSpectrum->tick(0.0F);
  visualizerSpectrum->setFlexGrow(1.0F);
  m_visualizerSpectrum = visualizerSpectrum.get();
  visualizerBody->addChild(std::move(visualizerSpectrum));
  visualizerHost->addChild(std::move(visualizerBody));
  nowCard->addChild(std::move(visualizerHost));

  auto controls = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
  });
  controls->addChild(
      ui::button({
          .out = &m_repeatButton,
          .glyph = "repeat",
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.media.repeat"),
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              const auto current = m_mpris->loopStatusActive().value_or("None");
              const std::string next = current == "None" ? "Playlist" : (current == "Playlist" ? "Track" : "None");
              (void)m_mpris->setLoopStatusActive(next);
              PanelManager::instance().refresh();
            });
          },
      })
  );
  controls->addChild(
      ui::button({
          .out = &m_prevButton,
          .glyph = "media-prev",
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.media.previous"),
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->previousActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );
  controls->addChild(
      ui::button({
          .out = &m_playPauseButton,
          .glyph = "media-play",
          .enabled = false,
          .variant = ButtonVariant::Primary,
          .tooltip = i18n::tr("control-center.media.play"),
          .minWidth = kMediaPlayPauseHeight * scale,
          .minHeight = kMediaPlayPauseHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->playPauseActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );
  controls->addChild(
      ui::button({
          .out = &m_nextButton,
          .glyph = "media-next",
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.media.next"),
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->nextActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );
  controls->addChild(
      ui::button({
          .out = &m_shuffleButton,
          .glyph = "shuffle",
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.media.shuffle"),
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              const bool enabled = m_mpris->shuffleActive().value_or(false);
              (void)m_mpris->setShuffleActive(!enabled);
              PanelManager::instance().refresh();
            });
          },
      })
  );

  auto controlsHost = ui::row({
      .out = &m_controlsHost,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = 0.0F,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
  });
  controlsHost->addChild(std::move(controls));
  nowCard->addChild(std::move(controlsHost));

  auto volumeHost = ui::column({
      .out = &m_volumeHost,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
  });
  addSliderSection(
      *volumeHost, scale, i18n::tr("control-center.media.volume"), &m_volumeSlider, &m_volumeValue, 0.0, 100.0, 1.0,
      100.0, formatVolumePercent,
      [this]() {
        if (m_volumeSlider != nullptr) {
          commitVolume(m_volumeSlider->value());
        }
      }
  );
  if (m_volumeSlider != nullptr) {
    m_volumeSlider->setOnValueChanged([this](double next) {
      if (m_volumeValue != nullptr) {
        m_volumeValue->setText(formatVolumePercent(next));
      }
      if (!m_syncingVolume) {
        commitVolume(next);
      }
    });
  }
  nowCard->addChild(std::move(volumeHost));
  live->addChild(std::move(nowCard));

  auto leftover = ui::column({
      .out = &m_artworkWell,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  auto artworkHost = ui::column({
      .out = &m_artworkHost,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });
  auto artworkSlot = ui::column({
      .out = &m_artworkSlot,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .fill = colorSpecFromRole(ColorRole::Surface),
      .radius = Style::scaledRadiusXl(scale),
      .clipChildren = true,
      .width = artSize,
      .height = artSize,
  });
  artworkSlot->addChild(
      ui::glyph({
          .out = &m_artworkFallback,
          .glyph = "disc-filled",
          .glyphSize = artSize * 0.45F,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  artworkSlot->addChild(
      ui::image({
          .out = &m_artwork,
          .fit = ImageFit::Cover,
          .radius = Style::scaledRadiusXl(scale),
          .participatesInLayout = false,
          .configure = [](Image& image) { image.setZIndex(1); },
      })
  );
  artworkSlot->addChild(
      ui::inputArea({
          .out = &m_artworkHit,
          .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .tooltip = i18n::tr("control-center.media.open-player"),
          .zIndex = 2,
          .participatesInLayout = false,
          .onClick = [this](const InputArea::PointerData&) { openPlayer(); },
      })
  );
  artworkHost->addChild(std::move(artworkSlot));
  leftover->addChild(std::move(artworkHost));
  live->addChild(std::move(leftover));

  return m_settings->wrapLive(std::move(live), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> MediaTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void MediaTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool MediaTab::configureMode() const { return m_settings->configureMode(); }

void MediaTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* MediaTab::settingsPane() { return m_settings.get(); }

void MediaTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }

  const float scale = contentScale();
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  layoutArtworkSlot(scale);
  pinArtwork();

  const float metaWidth = m_metaColumn != nullptr
      ? std::max(1.0F, m_metaColumn->width() - (m_metaColumn->paddingLeft() + m_metaColumn->paddingRight()))
      : 1.0F;
  if (m_trackTitle != nullptr) {
    m_trackTitle->setMaxWidth(metaWidth);
  }
  if (m_trackArtist != nullptr) {
    m_trackArtist->setMaxWidth(metaWidth);
  }
  if (m_trackAlbum != nullptr) {
    m_trackAlbum->setMaxWidth(metaWidth);
  }

  const float sideButtonSize = kMediaControlsHeight * scale;
  const float playPauseButtonSize = kMediaPlayPauseHeight * scale;
  const float sideGlyphSize = Style::fontSizeTitle * scale;
  const float playPauseGlyphSize = (Style::fontSizeTitle + Style::spaceXs) * scale;
  for (auto* button : {m_repeatButton, m_prevButton, m_nextButton, m_shuffleButton}) {
    if (button != nullptr) {
      button->setMinWidth(sideButtonSize);
      button->setMinHeight(sideButtonSize);
      button->setGlyphSize(sideGlyphSize);
      button->setPadding(Style::spaceSm * scale, Style::spaceSm * scale);
      button->setRadius(Style::scaledRadiusLg(scale));
    }
  }
  if (m_playPauseButton != nullptr) {
    m_playPauseButton->setMinWidth(playPauseButtonSize);
    m_playPauseButton->setMinHeight(playPauseButtonSize);
    m_playPauseButton->setGlyphSize(playPauseGlyphSize);
    m_playPauseButton->setPadding(Style::spaceSm * scale, Style::spaceSm * scale);
    m_playPauseButton->setRadius(Style::scaledRadiusLg(scale));
  }

  m_rootLayout->layout(renderer);
  layoutArtworkSlot(scale);
  pinArtwork();

  if (m_visualizerHost != nullptr && m_visualizerHost->visible() && m_visualizerBody != nullptr
      && m_visualizerSpectrum != nullptr) {
    const float bodyWidth = std::max(
        0.0F, m_visualizerBody->width() - (m_visualizerBody->paddingLeft() + m_visualizerBody->paddingRight())
    );
    const float bodyHeightAvail = std::max(
        0.0F, m_visualizerBody->height() - (m_visualizerBody->paddingTop() + m_visualizerBody->paddingBottom())
    );
    m_visualizerSpectrum->setSize(std::max(1.0F, bodyWidth), std::max(1.0F, bodyHeightAvail));
    m_visualizerBody->layout(renderer);
  }
}

void MediaTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    m_progressTimer.stop();
    return;
  }
  if (m_visualizerSpectrum != nullptr && m_spectrum != nullptr && m_spectrumListenerId != 0) {
    if (!m_spectrum->idle() || !m_visualizerSpectrum->converged()) {
      m_visualizerSpectrum->setValues(m_spectrum->values(m_spectrumListenerId));
    }
  }

  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
  const auto now = std::chrono::steady_clock::now();
  const bool hasPendingSeek = m_pendingSeekUs >= 0 && now < m_pendingSeekUntil;
  const bool withinProgressSettle =
      m_progressSettleUntil != std::chrono::steady_clock::time_point{} && now < m_progressSettleUntil;
  const bool playing = active.has_value() && active->playbackStatus == "Playing";
  if (playing || hasPendingSeek || withinProgressSettle) {
    if (!m_progressTimer.active()) {
      m_progressTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
        if (!m_active) {
          return;
        }
        PanelManager::instance().requestUpdateOnly();
        PanelManager::instance().requestRedraw();
      });
    }
  } else {
    m_progressTimer.stop();
  }

  refresh(renderer);
}

void MediaTab::onFrameTick(float deltaMs) {
  if (!m_active) {
    return;
  }

  if (m_visualizerSpectrum != nullptr) {
    if (m_spectrum != nullptr && m_spectrumListenerId != 0) {
      if (!m_spectrum->idle() || !m_visualizerSpectrum->converged()) {
        m_visualizerSpectrum->setValues(m_spectrum->values(m_spectrumListenerId));
      }
    }
    m_visualizerSpectrum->tick(deltaMs);
  }
}

void MediaTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_spectrum != nullptr) {
    if (active && m_spectrumListenerId == 0) {
      m_spectrumListenerId = m_spectrum->addChangeListener(kVisualizerBandCount, [this]() {
        if (!m_active || m_spectrum->idle()) {
          return;
        }
        PanelManager::instance().requestFrameTick();
      });
    } else if (!active && m_spectrumListenerId != 0) {
      m_spectrum->removeChangeListener(m_spectrumListenerId);
      m_spectrumListenerId = 0;
    }
  }
  if (!active) {
    m_progressTimer.stop();
    m_positionSampleAt = {};
    m_positionTrackSignature.clear();
    m_progressSettleUntil = {};
    m_nextRealtimeUpdateAt = {};
    m_lastRealtimeMprisPollAt = {};
  } else if (m_mpris != nullptr) {
    m_positionSampleAt = {};
  }
}

void MediaTab::onClose() {
  m_settings->onClose();
  m_progressTimer.stop();
  if (m_spectrum != nullptr) {
    if (m_spectrumListenerId != 0) {
      m_spectrum->removeChangeListener(m_spectrumListenerId);
      m_spectrumListenerId = 0;
    }
  }
  m_active = false;
  m_rootLayout = nullptr;
  m_nowCard = nullptr;
  m_artworkWell = nullptr;
  m_playerSelectHost = nullptr;
  m_playerSelect = nullptr;
  m_hidePlayerButton = nullptr;
  m_artworkHost = nullptr;
  m_artworkSlot = nullptr;
  m_artworkFallback = nullptr;
  m_artwork = nullptr;
  m_artworkHit = nullptr;
  m_metaColumn = nullptr;
  m_trackTitle = nullptr;
  m_trackArtist = nullptr;
  m_trackAlbum = nullptr;
  m_progressRow = nullptr;
  m_elapsedLabel = nullptr;
  m_progressSlider = nullptr;
  m_remainingLabel = nullptr;
  m_visualizerHost = nullptr;
  m_visualizerBody = nullptr;
  m_visualizerSpectrum = nullptr;
  m_controlsHost = nullptr;
  m_prevButton = nullptr;
  m_playPauseButton = nullptr;
  m_nextButton = nullptr;
  m_repeatButton = nullptr;
  m_shuffleButton = nullptr;
  m_volumeHost = nullptr;
  m_volumeSlider = nullptr;
  m_volumeValue = nullptr;
  m_lastArtPath.clear();
  m_lastBusName.clear();
  m_lastPlaybackStatus.clear();
  m_lastLoopStatus.clear();
  m_playerBusNames.clear();
  m_lastActiveSnapshot.reset();
  m_pendingSeekBusName.clear();
  m_pendingSeekUs = -1;
  m_progressSettleUntil = {};
  m_positionTrackSignature.clear();
  m_nextRealtimeUpdateAt = {};
  m_lastRealtimeMprisPollAt = {};
}

void MediaTab::clearArt(Renderer& renderer) {
  if (m_artwork != nullptr) {
    m_artwork->clear(renderer);
    m_artwork->setVisible(false);
  }
  (void)setShown(m_artworkFallback, true);
}

void MediaTab::commitPendingSeek(double valueSeconds) {
  if (m_mpris == nullptr) {
    return;
  }

  const auto targetUs = static_cast<std::int64_t>(std::llround(valueSeconds * 1000000.0));
  const auto now = std::chrono::steady_clock::now();
  m_positionUs = targetUs;
  m_positionSampleAt = now;
  const auto active = m_mpris->activePlayer();
  const std::string seekBusName =
      active.has_value() ? active->busName : (!m_positionBusName.empty() ? m_positionBusName : std::string{});
  m_pendingSeekBusName = seekBusName;
  m_pendingSeekUs = targetUs;
  m_pendingSeekUntil = now + kPendingSeekTimeout;
  m_progressSettleUntil = now + kProgressSettleHold;

  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard, seekBusName, targetUs]() {
    if (aliveGuard.expired() || m_mpris == nullptr) {
      return;
    }
    if (!seekBusName.empty()) {
      (void)m_mpris->setPosition(seekBusName, targetUs);
    } else {
      (void)m_mpris->setPositionActive(targetUs);
    }
    PanelManager::instance().refresh();
  });
}

void MediaTab::refresh(Renderer& renderer) {
  std::vector<MprisPlayerInfo> players;
  std::optional<MprisPlayerInfo> active;
  const auto now = std::chrono::steady_clock::now();
  if (m_mpris != nullptr) {
    players = m_mpris->listPlayers();
    active = m_mpris->activePlayer();
  }

  if (!active.has_value() && m_lastActiveSnapshot.has_value() && now - m_lastActiveSeenAt <= kNoActivePlayerGrace) {
    active = m_lastActiveSnapshot;
  }

  syncPlayerSelect(players);
  const bool spectrumLive = m_spectrum != nullptr && !m_spectrum->idle();
  const bool playing = active.has_value() && active->playbackStatus == "Playing";
  const bool showVisualizer = playing || spectrumLive
      || (m_visualizerSpectrum != nullptr && !m_visualizerSpectrum->converged());
  bool layoutDirty = syncPlaybackChrome(active.has_value(), showVisualizer);

  if (m_trackTitle == nullptr
      || m_trackArtist == nullptr
      || m_progressSlider == nullptr
      || m_playPauseButton == nullptr
      || m_repeatButton == nullptr
      || m_shuffleButton == nullptr) {
    if (layoutDirty) {
      PanelManager::instance().requestLayout();
    }
    return;
  }

  if (active.has_value()) {
    const auto& player = *active;
    m_lastActiveSnapshot = player;
    m_lastActiveSeenAt = now;
    const std::string trackSignature = std::format(
        "{}\n{}\n{}\n{}\n{}", player.trackId, player.title, joinArtists(player.artists), player.album, player.sourceUrl
    );
    std::int64_t livePositionUs = player.positionUs;
    if (player.lengthUs > 0) {
      livePositionUs = std::clamp<std::int64_t>(livePositionUs, 0, player.lengthUs);
    } else {
      livePositionUs = std::max<std::int64_t>(0, livePositionUs);
    }

    const bool pendingMatchesPlayer = m_pendingSeekBusName.empty() || m_pendingSeekBusName == player.busName;
    const bool seekArrived = pendingMatchesPlayer
        && m_pendingSeekUs >= 0
        && std::llabs(livePositionUs - m_pendingSeekUs) <= kSeekArrivedToleranceUs
        && (m_pendingSeekUs <= kSeekNearZeroUs
            || livePositionUs > kSeekNearZeroUs
            || livePositionUs >= m_pendingSeekUs - kSeekArrivedToleranceUs);
    const bool seekPending = pendingMatchesPlayer && m_pendingSeekUs >= 0 && !seekArrived && now < m_pendingSeekUntil;
    const bool withinProgressSettle =
        m_progressSettleUntil != std::chrono::steady_clock::time_point{} && now < m_progressSettleUntil;
    const bool sameDisplayedTrack = m_positionBusName == player.busName && m_positionTrackSignature == trackSignature;
    const bool withinTransientRegressionWindow = m_positionSampleAt != std::chrono::steady_clock::time_point{}
        && now - m_positionSampleAt <= kTransientPositionRegressionWindow;
    const bool preserveDisplayedPosition = !seekPending
        && sameDisplayedTrack
        && m_lastPlaybackStatus == "Playing"
        && player.playbackStatus == "Playing"
        && m_positionUs >= kTransientPositionRegressionFloorUs
        && livePositionUs <= kTransientPositionRegressionCeilingUs
        && livePositionUs + kTransientPositionRegressionDeltaUs < m_positionUs
        && withinTransientRegressionWindow;
    if (preserveDisplayedPosition) {
      livePositionUs = m_positionUs;
    }

    std::int64_t displayPositionUs = livePositionUs;
    if (seekPending) {
      displayPositionUs = m_pendingSeekUs;
    } else if (withinProgressSettle && livePositionUs + kTransientPositionRegressionDeltaUs < m_positionUs) {
      displayPositionUs = m_positionUs;
    } else if (preserveDisplayedPosition) {
      displayPositionUs = m_positionUs;
    }

    const bool samePlayerAsDisplayed = m_positionBusName == player.busName || m_pendingSeekBusName == player.busName;

    m_positionBusName = player.busName;
    m_positionTrackId = player.trackId;
    m_positionTrackSignature = trackSignature;
    m_positionUs = displayPositionUs;
    m_positionSampleAt = now;

    if (seekArrived) {
      m_pendingSeekBusName.clear();
      m_pendingSeekUs = -1;
    }

    m_trackTitle->setText(player.title.empty() ? player.identity : player.title);
    m_trackArtist->setText(joinArtists(player.artists).empty() ? player.identity : joinArtists(player.artists));
    if (m_trackAlbum != nullptr) {
      m_trackAlbum->setText(player.album);
      layoutDirty |= setShown(m_trackAlbum, !player.album.empty());
    }

    const std::string resolvedArtUrl = effectiveArtUrl(player);
    const std::string artPath = resolveArtworkSource(
        m_httpClient, m_pendingArtDownloads, resolvedArtUrl,
        [this] {
          m_lastArtPath.clear();
          PanelManager::instance().refresh();
        },
        m_aliveGuard
    );

    if (m_artwork != nullptr
        && (!resolvedArtUrl.empty() && (resolvedArtUrl != m_lastArtPath || !m_artwork->hasImage()))) {
      bool loaded = false;
      if (artPath.empty()) {
        kLog.debug("artwork unresolved url=\"{}\"", resolvedArtUrl);
        clearArt(renderer);
      } else if (!m_artwork->setSourceFile(renderer, artPath, mediaTabArtDecodeSize(contentScale()), true, true)) {
        kLog.warn(R"(artwork load failed url="{}" path="{}")", resolvedArtUrl, artPath);
        clearArt(renderer);
      } else {
        kLog.debug(R"(artwork loaded url="{}" path="{}")", resolvedArtUrl, artPath);
        loaded = true;
        m_artwork->setVisible(true);
        (void)setShown(m_artworkFallback, false);
      }

      m_lastArtPath = loaded ? resolvedArtUrl : std::string{};
      if (loaded) {
        layoutDirty = true;
      }
    } else if (m_artwork != nullptr && resolvedArtUrl.empty()) {
      clearArt(renderer);
      m_lastArtPath.clear();
    } else if (m_artwork != nullptr && m_artwork->hasImage()) {
      m_artwork->setVisible(true);
      (void)setShown(m_artworkFallback, false);
    }

    std::int64_t trackLengthUs = player.lengthUs;
    if (trackLengthUs > 0) {
      m_lastTrackLengthUs = trackLengthUs;
    } else if (m_lastTrackLengthUs > 0 && samePlayerAsDisplayed) {
      trackLengthUs = m_lastTrackLengthUs;
    }
    const bool progressInteracting = m_progressSlider->dragging() || seekPending || withinProgressSettle;
    const bool progressEnabled = player.canSeek && (trackLengthUs > 0 || progressInteracting);

    m_syncingProgress = true;
    m_progressSlider->setEnabled(progressEnabled);
    if (trackLengthUs > 0) {
      m_progressSlider->setRange(0.0, static_cast<double>(trackLengthUs) / 1000000.0);
    }
    if (!m_progressSlider->dragging()) {
      const double sliderMax = m_progressSlider->maxValue();
      const double nextValue =
          sliderMax > 0.0 ? std::clamp(static_cast<double>(displayPositionUs) / 1000000.0, 0.0, sliderMax) : 0.0;
      m_progressSlider->setValue(nextValue);
    }
    m_syncingProgress = false;

    if (m_elapsedLabel != nullptr) {
      m_elapsedLabel->setText(formatMediaClock(displayPositionUs));
    }
    layoutDirty |= setShown(m_remainingLabel, trackLengthUs > 0);
    if (m_remainingLabel != nullptr && trackLengthUs > 0) {
      m_remainingLabel->setText("-" + formatMediaClock(trackLengthUs - displayPositionUs));
    }

    m_playPauseButton->setGlyph(playPauseGlyph(player.playbackStatus));
    m_playPauseButton->setVariant(ButtonVariant::Primary);
    m_playPauseButton->setTooltip(
        i18n::tr(player.playbackStatus == "Playing" ? "control-center.media.pause" : "control-center.media.play")
    );
    if (m_prevButton != nullptr) {
      m_prevButton->setEnabled(player.canGoPrevious);
    }
    if (m_nextButton != nullptr) {
      m_nextButton->setEnabled(player.canGoNext);
    }
    m_repeatButton->setGlyph(repeatGlyph(player.loopStatus));
    m_repeatButton->setVariant(toggleVariant(player.loopStatus != "None"));
    m_repeatButton->setTooltip(i18n::tr(repeatTooltipKey(player.loopStatus)));
    m_shuffleButton->setVariant(toggleVariant(player.shuffle));

    if (m_volumeSlider != nullptr && !m_volumeSlider->dragging()) {
      const int percent = static_cast<int>(std::lround(std::clamp(player.volume, 0.0, 1.0) * 100.0));
      m_syncingVolume = true;
      m_volumeSlider->setEnabled(player.canControl);
      m_volumeSlider->setValue(static_cast<double>(percent));
      if (m_volumeValue != nullptr) {
        m_volumeValue->setText(formatVolumePercent(percent));
      }
      m_syncingVolume = false;
      m_lastSentVolumePercent = percent;
    }

    m_lastBusName = player.busName;
    m_lastPlaybackStatus = player.playbackStatus;
    m_lastLoopStatus = player.loopStatus;
    m_lastShuffle = player.shuffle;
    if (layoutDirty) {
      PanelManager::instance().requestLayout();
    }
    return;
  }

  m_pendingSeekBusName.clear();
  m_pendingSeekUs = -1;
  m_progressSettleUntil = {};
  m_lastActiveSnapshot.reset();
  m_positionBusName.clear();
  m_positionTrackId.clear();
  m_positionTrackSignature.clear();
  m_positionUs = 0;
  m_lastTrackLengthUs = 0;
  m_positionSampleAt = {};
  m_trackTitle->setText(i18n::tr("control-center.media.nothing-playing"));
  m_trackArtist->setText(i18n::tr("control-center.media.start-playback"));
  if (m_trackAlbum != nullptr) {
    m_trackAlbum->setText("");
    layoutDirty |= setShown(m_trackAlbum, false);
  }
  clearArt(renderer);
  m_lastArtPath.clear();
  m_syncingProgress = true;
  m_progressSlider->setEnabled(false);
  m_progressSlider->setRange(0.0F, 100.0F);
  m_progressSlider->setValue(0.0F);
  m_syncingProgress = false;
  if (m_volumeSlider != nullptr) {
    m_syncingVolume = true;
    m_volumeSlider->setEnabled(false);
    m_volumeSlider->setValue(100.0);
    if (m_volumeValue != nullptr) {
      m_volumeValue->setText(formatVolumePercent(100.0));
    }
    m_syncingVolume = false;
    m_lastSentVolumePercent = -1;
  }
  if (m_elapsedLabel != nullptr) {
    m_elapsedLabel->setText(formatClockTime(0));
  }
  if (m_remainingLabel != nullptr) {
    m_remainingLabel->setText("-" + formatClockTime(0));
  }
  m_playPauseButton->setGlyph("media-play");
  m_playPauseButton->setTooltip(i18n::tr("control-center.media.play"));
  if (m_prevButton != nullptr) {
    m_prevButton->setEnabled(false);
  }
  if (m_nextButton != nullptr) {
    m_nextButton->setEnabled(false);
  }
  m_repeatButton->setGlyph("repeat");
  m_repeatButton->setVariant(ButtonVariant::Ghost);
  m_repeatButton->setTooltip(i18n::tr("control-center.media.repeat"));
  m_shuffleButton->setVariant(ButtonVariant::Ghost);
  m_lastBusName.clear();
  m_lastPlaybackStatus.clear();
  m_lastLoopStatus.clear();
  m_lastShuffle = false;
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}
