#include "shell/control_center/tabs/screenshot_tab.h"

#include "config/config_service.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] std::filesystem::path screenshotOutputDirectory(const Config& config) {
    if (config.shell.screenshot.directory.empty()) {
      return FileUtils::defaultPicturesDirectory();
    }
    return FileUtils::expandUserPath(config.shell.screenshot.directory);
  }

} // namespace

ScreenshotTab::ScreenshotTab(const ControlCenterServices& services)
    : m_ipc(services.ipc), m_config(services.config), m_thumbnails(services.thumbnails) {}

std::unique_ptr<Flex> ScreenshotTab::createLiveBody() {
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

  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*card, i18n::tr("control-center.screenshot.capture"), scale);
  addBody(*card, i18n::tr("control-center.screenshot.hint"), scale);

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

  m_captureButtons.clear();
  auto makeCaptureButton =
      [this, scale](
          const char* glyph, const char* textKey, const char* command, ButtonVariant variant, bool available = true,
          const char* missingKey = nullptr
      ) {
        Button* button = nullptr;
        auto node = ui::button({
            .out = &button,
            .text = i18n::tr(textKey),
            .glyph = glyph,
            .enabled = available,
            .variant = variant,
            .tooltip = !available && missingKey != nullptr ? std::optional<std::string>{i18n::tr(missingKey)}
                                                          : std::nullopt,
            .minHeight = Style::controlHeight * scale,
            .paddingH = Style::spaceMd * scale,
            .flexGrow = 1.0F,
            .onClick = [this, command]() { requestCapture(command); },
        });
        if (button != nullptr) {
          m_captureButtons.push_back({.button = button, .available = available});
        }
        return node;
      };

  const bool ocrAvailable = process::commandExists("tesseract");
  const bool qrAvailable = process::commandExists("zbarimg");

  card->addChild(
      makeCaptureButton("crop", "control-center.screenshot.region", "screenshot-region", ButtonVariant::Primary)
  );
  card->addChild(ui::row(
      {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
      makeCaptureButton(
          "screenshot", "control-center.screenshot.fullscreen", "screenshot-fullscreen", ButtonVariant::Ghost
      ),
      makeCaptureButton(
          "device-desktop", "control-center.screenshot.pick-display", "screenshot-fullscreen pick",
          ButtonVariant::Ghost
      )
  ));
  card->addChild(ui::row(
      {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
      makeCaptureButton(
          "color-picker", "control-center.screenshot.pick-color", "screenshot-pick-color", ButtonVariant::Ghost
      ),
      makeCaptureButton("brush", "control-center.screenshot.annotate", "screenshot-annotate", ButtonVariant::Ghost)
  ));
  card->addChild(ui::row(
      {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
      makeCaptureButton(
          "text-recognition", "control-center.screenshot.ocr", "screenshot-ocr", ButtonVariant::Ghost, ocrAvailable,
          "control-center.screenshot.ocr-missing"
      ),
      makeCaptureButton(
          "qrcode", "control-center.screenshot.scan-qr", "screenshot-scan-qr", ButtonVariant::Ghost, qrAvailable,
          "control-center.screenshot.qr-missing"
      )
  ));

  card->addChild(
      ui::button({
          .text = i18n::tr("control-center.screenshot.open-folder"),
          .glyph = "folder",
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick = [this]() { openScreenshotFolder(); },
      })
  );
  card->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  content->addChild(std::move(card));

  auto recentCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*recentCard, i18n::tr("control-center.screenshot.recent"), scale);
  recentCard->addChild(
      ui::label({
          .out = &m_recentEmpty,
          .text = i18n::tr("control-center.screenshot.recent-empty"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  recentCard->addChild(
      ui::column({
          .out = &m_recentList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      })
  );
  content->addChild(std::move(recentCard));
  live->addChild(std::move(scroll));
  bindRecentGallery();
  rebuildRecentFiles();
  return live;
}

void ScreenshotTab::setPanelCardOpacity(float opacity) noexcept {
  m_panelCardOpacity = std::clamp(opacity, 0.0F, 1.0F);
}

void ScreenshotTab::setActive(bool active) {
  if (active) {
    rebuildRecentFiles();
  }
}

void ScreenshotTab::onClose() {
  cancelCountdown();
  m_rootLayout = nullptr;
  m_delay = nullptr;
  m_status = nullptr;
  m_recentList = nullptr;
  m_recentEmpty = nullptr;
  m_captureButtons.clear();
  m_recents.onClose();
}

void ScreenshotTab::updateContent(Renderer& renderer) {
  m_recents.update(renderer);
}

int ScreenshotTab::delaySeconds() const noexcept {
  if (m_delayIndex == 1) {
    return 3;
  }
  if (m_delayIndex == 2) {
    return 5;
  }
  return 0;
}

void ScreenshotTab::requestCapture(const char* command) {
  if (command == nullptr) {
    return;
  }
  const int delay = delaySeconds();
  if (delay <= 0) {
    runCapture(command);
    return;
  }
  m_pendingCommand = command;
  m_countdownLeft = delay;
  syncCountdownUi();
  m_delayTimer.stop();
  m_delayTimer.startRepeating(std::chrono::seconds{1}, [this]() { tickCountdown(); });
  PanelManager::instance().requestRedraw();
}

void ScreenshotTab::tickCountdown() {
  if (m_pendingCommand == nullptr) {
    cancelCountdown();
    return;
  }
  if (m_countdownLeft <= 1) {
    const char* command = m_pendingCommand;
    cancelCountdown();
    runCapture(command);
    return;
  }
  --m_countdownLeft;
  syncCountdownUi();
  PanelManager::instance().requestRedraw();
}

void ScreenshotTab::cancelCountdown() {
  m_delayTimer.stop();
  m_pendingCommand = nullptr;
  m_countdownLeft = 0;
  syncCountdownUi();
}

void ScreenshotTab::syncCountdownUi() {
  const bool counting = m_pendingCommand != nullptr && m_countdownLeft > 0;
  if (m_delay != nullptr) {
    m_delay->setEnabled(!counting);
  }
  for (const auto& action : m_captureButtons) {
    if (action.button != nullptr) {
      action.button->setEnabled(!counting && action.available);
    }
  }
  if (m_status == nullptr) {
    return;
  }
  if (!counting) {
    m_status->setText({});
    return;
  }
  m_status->setText(i18n::tr("control-center.screenshot.countdown", "seconds", std::to_string(m_countdownLeft)));
}

void ScreenshotTab::runCapture(const char* command) {
  PanelManager::instance().closePanel();
  if (m_ipc != nullptr && command != nullptr) {
    (void)m_ipc->execute(command);
  }
}

void ScreenshotTab::bindRecentGallery() {
  m_recents.bind({
      .list = m_recentList,
      .empty = m_recentEmpty,
      .thumbnails = m_thumbnails,
      .scale = contentScale(),
      .kind = RecentCaptureKind::Images,
      .showAnnotate = true,
      .onOpen = [this](const std::string& path) { openRecentFile(path); },
      .onAnnotate = [this](const std::string& path) { annotateRecentFile(path); },
      .onStatus =
          [this](std::string_view message) {
            if (m_status != nullptr) {
              m_status->setText(std::string(message));
              PanelManager::instance().requestRedraw();
            }
          },
  });
}

void ScreenshotTab::rebuildRecentFiles() {
  if (m_config == nullptr) {
    return;
  }
  m_recents.rebuild(screenshotOutputDirectory(m_config->config()));
}

void ScreenshotTab::openRecentFile(const std::string& path) {
  if (!process::commandExists("xdg-open")) {
    if (m_status != nullptr) {
      m_status->setText(i18n::tr("control-center.screenshot.open-folder-failed"));
      PanelManager::instance().requestRedraw();
    }
    return;
  }
  (void)process::runAsync(std::vector<std::string>{"xdg-open", path});
}

void ScreenshotTab::annotateRecentFile(const std::string& path) {
  PanelManager::instance().closePanel();
  if (m_ipc != nullptr) {
    (void)m_ipc->execute("annotate " + path);
  }
}

void ScreenshotTab::openScreenshotFolder() {
  if (m_config == nullptr) {
    return;
  }
  const std::filesystem::path dir = screenshotOutputDirectory(m_config->config());
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    (void)std::filesystem::create_directories(dir, ec);
  }
  if (!process::commandExists("xdg-open")) {
    if (m_status != nullptr) {
      m_status->setText(i18n::tr("control-center.screenshot.open-folder-failed"));
      PanelManager::instance().requestRedraw();
    }
    return;
  }
  (void)process::runAsync(std::vector<std::string>{"xdg-open", dir.string()});
}
