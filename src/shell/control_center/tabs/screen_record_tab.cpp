#include "shell/control_center/tabs/screen_record_tab.h"

#include "capture/screen_recorder_service.h"
#include "config/config_service.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] std::filesystem::path recordingsDirectory(const Config& config) {
    if (config.shell.screenRecord.directory.empty()) {
      return FileUtils::expandUserPath("~/Videos/Recordings");
    }
    return FileUtils::expandUserPath(config.shell.screenRecord.directory);
  }

  [[nodiscard]] std::string sourceTooltip(ScreenRecorderService::Source source) {
    switch (source) {
    case ScreenRecorderService::Source::Region:
      return i18n::tr("control-center.screen-record.source-region-tooltip");
    case ScreenRecorderService::Source::Portal:
      return i18n::tr("control-center.screen-record.source-portal-tooltip");
    case ScreenRecorderService::Source::Focused:
      break;
    }
    return i18n::tr("control-center.screen-record.source-focused-tooltip");
  }

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  [[nodiscard]] std::string sourceLabel(ScreenRecorderService::Source source) {
    switch (source) {
    case ScreenRecorderService::Source::Region:
      return i18n::tr("control-center.screen-record.source-region");
    case ScreenRecorderService::Source::Portal:
      return i18n::tr("control-center.screen-record.source-portal");
    case ScreenRecorderService::Source::Focused:
      break;
    }
    return i18n::tr("control-center.screen-record.source-focused");
  }

} // namespace

ScreenRecordTab::ScreenRecordTab(const ControlCenterServices& services)
    : m_recorder(services.screenRecorder), m_config(services.config), m_thumbnails(services.thumbnails) {
  if (m_recorder != nullptr) {
    m_recorder->setChangeCallback([this]() {
      syncUi();
      rebuildRecentFiles();
    });
  }
  if (services.settingsInvalidated != nullptr) {
    m_settingsInvalidatedConn = services.settingsInvalidated->connect([this]() {
      syncUi();
      rebuildRecentFiles();
    });
  }
}

std::unique_ptr<Flex> ScreenRecordTab::createLiveBody() {
  const float scale = contentScale();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });

  auto scroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  buildRecordCard(*content, scale);
  buildReplayCard(*content, scale);
  buildRecentCard(*content, scale);

  live->addChild(std::move(scroll));
  bindRecentGallery();
  syncUi();
  rebuildRecentFiles();
  return live;
}

void ScreenRecordTab::setPanelCardOpacity(float opacity) noexcept {
  m_panelCardOpacity = std::clamp(opacity, 0.0F, 1.0F);
}

void ScreenRecordTab::buildRecordCard(Flex& root, float scale) {
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*card, i18n::tr("control-center.screen-record.record"), scale);
  addBody(*card, i18n::tr("control-center.screen-record.hint"), scale);

  m_sources = m_recorder != nullptr ? m_recorder->availableSources()
                                   : std::vector<ScreenRecorderService::Source>{ScreenRecorderService::Source::Focused};
  std::size_t selected = 0;
  std::vector<ui::SegmentedOption> sources;
  sources.reserve(m_sources.size());
  for (std::size_t i = 0; i < m_sources.size(); ++i) {
    if (m_sources[i] == m_selectedSource) {
      selected = i;
    }
    sources.push_back({.label = sourceLabel(m_sources[i]), .tooltip = sourceTooltip(m_sources[i])});
  }
  m_selectedSource = m_sources[selected];

  card->addChild(
      ui::segmented({
          .out = &m_source,
          .options = std::move(sources),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeCaption,
          .scale = scale,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange =
              [this](std::size_t idx) {
                if (idx < m_sources.size()) {
                  m_selectedSource = m_sources[idx];
                }
              },
      })
  );

  card->addChild(
      ui::segmented({
          .out = &m_delay,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("control-center.screenshot.delay-none"),
                      .tooltip = i18n::tr("control-center.screenshot.delay-none-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.screenshot.delay-3s"),
                      .tooltip = i18n::tr("control-center.screenshot.delay-3s-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.screenshot.delay-5s"),
                      .tooltip = i18n::tr("control-center.screenshot.delay-5s-tooltip"),
                  },
              },
          .selectedIndex = m_delayIndex,
          .fontSize = Style::fontSizeCaption,
          .scale = scale,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t idx) { m_delayIndex = idx; },
      })
  );

  card->addChild(
      ui::button({
          .out = &m_recordButton,
          .text = i18n::tr("control-center.screen-record.start"),
          .glyph = "video",
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_recorder == nullptr) {
                  return;
                }
                if (m_recorder->isRecording()) {
                  cancelCountdown();
                  (void)m_recorder->stop();
                  return;
                }
                requestStart();
              },
      })
  );
  card->addChild(
      ui::button({
          .text = i18n::tr("control-center.screen-record.open-folder"),
          .glyph = "folder",
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick = [this]() { openRecordingsFolder(); },
      })
  );
  card->addChild(
      ui::label({
          .out = &m_elapsed,
          .fontSize = Style::fontSizeHeader * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  card->addChild(
      ui::label({
          .out = &m_backend,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  card->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  root.addChild(std::move(card));
}

void ScreenRecordTab::buildReplayCard(Flex& root, float scale) {
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*card, i18n::tr("control-center.screen-record.replay"), scale);
  card->addChild(
      ui::label({
          .out = &m_replayHint,
          .text = i18n::tr("control-center.screen-record.replay-unavailable"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  card->addChild(
      ui::button({
          .out = &m_replayStartButton,
          .text = i18n::tr("control-center.screen-record.replay-start"),
          .glyph = "history",
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_recorder != nullptr && m_recorder->isReplaying()) {
                  requestReplayStop();
                  return;
                }
                requestReplayStart();
              },
      })
  );
  card->addChild(
      ui::button({
          .out = &m_replaySaveButton,
          .text = i18n::tr("control-center.screen-record.replay-save"),
          .glyph = "device-floppy",
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick = [this]() { requestReplaySave(); },
      })
  );
  card->addChild(
      ui::label({
          .out = &m_replayStatus,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  root.addChild(std::move(card));
}

void ScreenRecordTab::buildRecentCard(Flex& root, float scale) {
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*card, i18n::tr("control-center.screen-record.recent"), scale);
  card->addChild(
      ui::label({
          .out = &m_recentEmpty,
          .text = i18n::tr("control-center.screen-record.recent-empty"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  card->addChild(
      ui::column({
          .out = &m_recentList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      })
  );
  root.addChild(std::move(card));
}

void ScreenRecordTab::setActive(bool active) {
  if (active) {
    rebuildRecentFiles();
    syncUi();
  }
}

void ScreenRecordTab::onClose() {
  m_elapsedTimer.stop();
  cancelCountdown();
  m_rootLayout = nullptr;
  m_source = nullptr;
  m_delay = nullptr;
  m_recordButton = nullptr;
  m_status = nullptr;
  m_elapsed = nullptr;
  m_backend = nullptr;
  m_replayHint = nullptr;
  m_replayStartButton = nullptr;
  m_replaySaveButton = nullptr;
  m_replayStatus = nullptr;
  m_recentList = nullptr;
  m_recentEmpty = nullptr;
  m_recents.onClose();
}

void ScreenRecordTab::updateContent(Renderer& renderer) {
  m_recents.update(renderer);
}

ScreenRecorderService::Source ScreenRecordTab::selectedSource() const noexcept { return m_selectedSource; }

ScreenRecorderService::Source ScreenRecordTab::replaySource() const noexcept {
  if (m_selectedSource == ScreenRecorderService::Source::Portal) {
    return ScreenRecorderService::Source::Portal;
  }
  return ScreenRecorderService::Source::Focused;
}

int ScreenRecordTab::delaySeconds() const noexcept {
  if (m_delayIndex == 1) {
    return 3;
  }
  if (m_delayIndex == 2) {
    return 5;
  }
  return 0;
}

void ScreenRecordTab::requestReplayStart() {
  if (m_recorder == nullptr || m_recorder->isReplaying() || m_recorder->isRecording()) {
    return;
  }
  (void)m_recorder->startReplay(replaySource());
}

void ScreenRecordTab::requestReplayStop() {
  if (m_recorder == nullptr || !m_recorder->isReplaying()) {
    return;
  }
  (void)m_recorder->stopReplay();
}

void ScreenRecordTab::requestReplaySave() {
  if (m_recorder == nullptr || !m_recorder->isReplaying() || m_recorder->isReplayStopping()) {
    return;
  }
  (void)m_recorder->saveReplay();
}

void ScreenRecordTab::requestStart() {
  if (m_recorder == nullptr || m_recorder->isRecording() || m_recorder->isReplaying()) {
    return;
  }
  const int delay = delaySeconds();
  if (delay <= 0) {
    (void)m_recorder->start(selectedSource());
    return;
  }
  m_pendingStart = true;
  m_countdownLeft = delay;
  m_delayTimer.stop();
  m_delayTimer.startRepeating(std::chrono::seconds{1}, [this]() { tickCountdown(); });
  syncUi();
}

void ScreenRecordTab::tickCountdown() {
  if (!m_pendingStart) {
    cancelCountdown();
    return;
  }
  if (m_countdownLeft <= 1) {
    cancelCountdown();
    if (m_recorder != nullptr && !m_recorder->isRecording() && !m_recorder->isReplaying()) {
      (void)m_recorder->start(selectedSource());
    }
    return;
  }
  --m_countdownLeft;
  syncUi();
}

void ScreenRecordTab::cancelCountdown() {
  m_delayTimer.stop();
  m_pendingStart = false;
  m_countdownLeft = 0;
}

void ScreenRecordTab::startElapsedTimer() {
  if (m_elapsedTimer.active()) {
    return;
  }
  m_elapsedTimer.startRepeating(std::chrono::seconds{1}, [this]() { syncUi(); });
}

void ScreenRecordTab::syncReplayUi() {
  const bool hasGsr = m_recorder != nullptr && m_recorder->hasGpuScreenRecorder();
  const bool recording = m_recorder != nullptr && m_recorder->isRecording();
  const bool replaying = m_recorder != nullptr && m_recorder->isReplaying();
  const bool stoppingReplay = m_recorder != nullptr && m_recorder->isReplayStopping();
  const bool counting = m_pendingStart && m_countdownLeft > 0;
  const int duration = m_recorder != nullptr ? m_recorder->replayDurationSeconds() : 30;
  const bool canStart = hasGsr && !replaying && !recording && !counting;

  if (m_replayHint != nullptr) {
    if (!hasGsr) {
      m_replayHint->setText(i18n::tr("control-center.screen-record.replay-unavailable"));
    } else {
      m_replayHint->setText(
          i18n::tr("control-center.screen-record.replay-hint", "seconds", std::to_string(duration))
      );
    }
  }
  if (m_replayStartButton != nullptr) {
    m_replayStartButton->setEnabled(canStart || replaying);
    if (replaying) {
      m_replayStartButton->setText(
          stoppingReplay ? i18n::tr("control-center.screen-record.stopping")
                         : i18n::tr("control-center.screen-record.replay-stop")
      );
      m_replayStartButton->setGlyph("player-stop");
      m_replayStartButton->setVariant(ButtonVariant::Destructive);
    } else {
      m_replayStartButton->setText(i18n::tr("control-center.screen-record.replay-start"));
      m_replayStartButton->setGlyph("history");
      m_replayStartButton->setVariant(ButtonVariant::Primary);
    }
  }
  if (m_replaySaveButton != nullptr) {
    m_replaySaveButton->setEnabled(replaying && !stoppingReplay);
  }
  if (m_replayStatus != nullptr) {
    if (!hasGsr) {
      m_replayStatus->setText({});
    } else if (replaying && m_recorder != nullptr) {
      m_replayStatus->setText(
          i18n::tr("control-center.screen-record.replay-active") + " · " + m_recorder->elapsedLabel()
      );
    } else if (recording) {
      m_replayStatus->setText(i18n::tr("control-center.screen-record.replay-busy"));
    } else {
      m_replayStatus->setText({});
    }
  }
}

void ScreenRecordTab::syncUi() {
  const bool recording = m_recorder != nullptr && m_recorder->isRecording();
  const bool stopping = m_recorder != nullptr && m_recorder->isStopping();
  const bool replaying = m_recorder != nullptr && m_recorder->isReplaying();
  const bool available = m_recorder != nullptr && m_recorder->available();
  const bool counting = m_pendingStart && m_countdownLeft > 0;

  if (recording || replaying) {
    if (recording && m_pendingStart) {
      cancelCountdown();
    }
    startElapsedTimer();
  } else {
    m_elapsedTimer.stop();
  }

  if (m_source != nullptr) {
    m_source->setEnabled(!recording && !counting && !replaying);
  }
  if (m_delay != nullptr) {
    m_delay->setEnabled(!recording && !counting && !replaying);
  }
  if (m_recordButton != nullptr) {
    m_recordButton->setEnabled((available || recording) && !counting && !replaying);
    if (recording) {
      m_recordButton->setText(
          stopping ? i18n::tr("control-center.screen-record.stopping")
                   : i18n::tr("control-center.screen-record.stop")
      );
      m_recordButton->setGlyph("player-stop");
      m_recordButton->setVariant(ButtonVariant::Destructive);
    } else {
      m_recordButton->setText(i18n::tr("control-center.screen-record.start"));
      m_recordButton->setGlyph("video");
      m_recordButton->setVariant(ButtonVariant::Primary);
    }
  }
  if (m_elapsed != nullptr && recording && m_recorder != nullptr) {
    m_elapsed->setText(m_recorder->elapsedLabel());
  }
  if (m_backend != nullptr) {
    m_backend->setText(m_recorder != nullptr ? m_recorder->backendDescription() : std::string{});
  }
  if (m_status != nullptr) {
    if (counting) {
      m_status->setText(
          i18n::tr("control-center.screen-record.countdown", "seconds", std::to_string(m_countdownLeft))
      );
    } else {
      m_status->setText({});
    }
  }
  const bool layoutDirty = setShown(m_elapsed, recording) || setShown(m_backend, !available) ||
                            setShown(m_status, counting);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
  syncReplayUi();
  PanelManager::instance().requestRedraw();
}

void ScreenRecordTab::bindRecentGallery() {
  m_recents.bind({
      .list = m_recentList,
      .empty = m_recentEmpty,
      .thumbnails = m_thumbnails,
      .scale = contentScale(),
      .kind = RecentCaptureKind::Videos,
      .showAnnotate = false,
      .onOpen = [this](const std::string& path) { openRecentFile(path); },
      .onStatus =
          [this](std::string_view message) {
            if (m_status != nullptr) {
              m_status->setText(std::string(message));
              PanelManager::instance().requestRedraw();
            }
          },
  });
}

void ScreenRecordTab::rebuildRecentFiles() {
  if (m_config == nullptr) {
    return;
  }
  m_recents.rebuild(recordingsDirectory(m_config->config()));
}

void ScreenRecordTab::openRecentFile(const std::string& path) {
  if (!process::commandExists("xdg-open")) {
    if (m_status != nullptr) {
      m_status->setText(i18n::tr("control-center.screen-record.open-folder-failed"));
      PanelManager::instance().requestRedraw();
    }
    return;
  }
  (void)process::runAsync(std::vector<std::string>{"xdg-open", path});
}

void ScreenRecordTab::openRecordingsFolder() {
  if (m_config == nullptr) {
    return;
  }
  const std::filesystem::path dir = recordingsDirectory(m_config->config());
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    (void)std::filesystem::create_directories(dir, ec);
  }
  if (!process::commandExists("xdg-open")) {
    if (m_status != nullptr) {
      m_status->setText(i18n::tr("control-center.screen-record.open-folder-failed"));
      PanelManager::instance().requestRedraw();
    }
    return;
  }
  (void)process::runAsync(std::vector<std::string>{"xdg-open", dir.string()});
}
