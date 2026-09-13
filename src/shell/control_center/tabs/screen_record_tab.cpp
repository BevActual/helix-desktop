#include "shell/control_center/tabs/screen_record_tab.h"

#include "capture/screen_recorder_service.h"
#include "config/config_service.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
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
    : m_recorder(services.screenRecorder), m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "screen-record"})) {
  if (m_recorder != nullptr) {
    m_recorder->setChangeCallback([this]() { syncUi(); });
  }
}

std::unique_ptr<Flex> ScreenRecordTab::create() {
  const float scale = contentScale();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

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
    sources.push_back({.label = sourceLabel(m_sources[i])});
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
                  {.label = i18n::tr("control-center.screenshot.delay-none")},
                  {.label = i18n::tr("control-center.screenshot.delay-3s")},
                  {.label = i18n::tr("control-center.screenshot.delay-5s")},
              },
          .selectedIndex = m_delayIndex,
          .fontSize = Style::fontSizeCaption,
          .scale = scale,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t idx) { m_delayIndex = idx; },
      })
  );

  auto audioRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = Style::controlHeightSm * scale,
  });
  audioRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.screen-record.audio"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  audioRow->addChild(
      ui::toggle({
          .out = &m_audio,
          .checked = m_config != nullptr && m_config->config().shell.screenRecord.audio,
          .scale = scale,
          .onChange =
              [this](bool value) {
                if (m_config != nullptr) {
                  (void)m_config->setOverride({"shell", "screen_record", "audio"}, value);
                }
              },
      })
  );
  card->addChild(std::move(audioRow));

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

  live->addChild(std::move(card));
  auto wrapped = m_settings->wrapLive(std::move(live), contentScale(), panelCardOpacity());
  syncUi();
  return wrapped;
}

std::unique_ptr<Flex> ScreenRecordTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void ScreenRecordTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool ScreenRecordTab::configureMode() const { return m_settings->configureMode(); }

void ScreenRecordTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

void ScreenRecordTab::onClose() {
  m_elapsedTimer.stop();
  cancelCountdown();
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_source = nullptr;
  m_delay = nullptr;
  m_audio = nullptr;
  m_recordButton = nullptr;
  m_status = nullptr;
  m_elapsed = nullptr;
  m_backend = nullptr;
}

void ScreenRecordTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

ScreenRecorderService::Source ScreenRecordTab::selectedSource() const noexcept { return m_selectedSource; }

int ScreenRecordTab::delaySeconds() const noexcept {
  if (m_delayIndex == 1) {
    return 3;
  }
  if (m_delayIndex == 2) {
    return 5;
  }
  return 0;
}

void ScreenRecordTab::requestStart() {
  if (m_recorder == nullptr || m_recorder->isRecording()) {
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
    if (m_recorder != nullptr && !m_recorder->isRecording()) {
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

void ScreenRecordTab::syncUi() {
  const bool recording = m_recorder != nullptr && m_recorder->isRecording();
  const bool stopping = m_recorder != nullptr && m_recorder->isStopping();
  const bool available = m_recorder != nullptr && m_recorder->available();
  const bool counting = m_pendingStart && m_countdownLeft > 0;

  if (recording) {
    if (m_pendingStart) {
      cancelCountdown();
    }
    startElapsedTimer();
  } else {
    m_elapsedTimer.stop();
  }

  if (m_source != nullptr) {
    m_source->setEnabled(!recording && !counting);
  }
  if (m_delay != nullptr) {
    m_delay->setEnabled(!recording && !counting);
  }
  if (m_audio != nullptr) {
    m_audio->setEnabled(!recording && !counting);
    if (m_config != nullptr && !recording) {
      m_audio->setCheckedImmediate(m_config->config().shell.screenRecord.audio);
    }
  }
  if (m_recordButton != nullptr) {
    m_recordButton->setEnabled((available || recording) && !counting);
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
  if (m_elapsed != nullptr) {
    m_elapsed->setText(m_recorder != nullptr ? m_recorder->elapsedLabel() : std::string{"00:00"});
  }
  if (m_backend != nullptr) {
    m_backend->setText(m_recorder != nullptr ? m_recorder->backendDescription() : std::string{});
  }
  if (m_status != nullptr) {
    if (counting) {
      m_status->setText(
          i18n::tr("control-center.screen-record.countdown", "seconds", std::to_string(m_countdownLeft))
      );
    } else if (recording && m_recorder != nullptr && !m_recorder->outputPath().empty()) {
      m_status->setText(m_recorder->outputPath().string());
    } else {
      m_status->setText({});
    }
  }
  PanelManager::instance().requestRedraw();
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
