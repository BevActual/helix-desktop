#include "shell/control_center/tabs/capture_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  void setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return;
    }
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
  }

} // namespace

CaptureTab::View CaptureTab::viewFromIndex(std::size_t index) noexcept {
  return index == 1 ? View::Record : View::Screenshot;
}

std::size_t CaptureTab::indexFromView(View view) noexcept {
  return view == View::Record ? 1 : 0;
}

CaptureTab::CaptureTab(const ControlCenterServices& services)
    : m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "screenshot"})),
      m_screenshot(std::make_unique<ScreenshotTab>(services)),
      m_record(std::make_unique<ScreenRecordTab>(services)) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> view = m_config->stateString("capture_tab", "view")) {
      if (*view == "record") {
        m_view = View::Record;
      }
    }
  }
  syncSettingsSection();
}

std::unique_ptr<Flex> CaptureTab::create() {
  const float scale = contentScale();
  m_screenshot->setContentScale(scale);
  m_record->setContentScale(scale);
  m_screenshot->setPanelCardOpacity(panelCardOpacity());
  m_record->setPanelCardOpacity(panelCardOpacity());

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });
  live->addChild(
      ui::segmented({
          .out = &m_viewSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("control-center.capture.screenshot")},
                  {.label = i18n::tr("control-center.capture.record")},
              },
          .selectedIndex = indexFromView(m_view),
          .fontSize = Style::fontSizeCaption,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { applyView(index); },
      })
  );

  auto screenshotHost = ui::column({
      .out = &m_screenshotHost,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });
  screenshotHost->addChild(m_screenshot->createLiveBody());
  live->addChild(std::move(screenshotHost));

  auto recordHost = ui::column({
      .out = &m_recordHost,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });
  recordHost->addChild(m_record->createLiveBody());
  live->addChild(std::move(recordHost));

  syncHosts();
  return m_settings->wrapLive(std::move(live), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> CaptureTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void CaptureTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool CaptureTab::configureMode() const { return m_settings->configureMode(); }

void CaptureTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* CaptureTab::settingsPane() { return m_settings.get(); }

void CaptureTab::setActive(bool active) {
  if (m_screenshot != nullptr) {
    m_screenshot->setActive(active && m_view == View::Screenshot);
  }
  if (m_record != nullptr) {
    m_record->setActive(active && m_view == View::Record);
  }
}

void CaptureTab::onClose() {
  if (m_screenshot != nullptr) {
    m_screenshot->onClose();
  }
  if (m_record != nullptr) {
    m_record->onClose();
  }
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_viewSwitch = nullptr;
  m_screenshotHost = nullptr;
  m_recordHost = nullptr;
  m_viewSyncing = false;
}

void CaptureTab::applyOpenView(View view, bool persist) {
  if (m_view == view) {
    syncHosts();
    syncSettingsSection();
    return;
  }
  m_view = view;
  if (persist) {
    persistView();
  }
  if (m_viewSwitch != nullptr) {
    m_viewSyncing = true;
    m_viewSwitch->setSelectedIndex(indexFromView(m_view));
    m_viewSyncing = false;
  }
  if (m_screenshot != nullptr) {
    m_screenshot->cancelCountdown();
  }
  if (m_record != nullptr) {
    m_record->cancelCountdown();
  }
  syncHosts();
  syncSettingsSection();
  setActive(true);
  PanelManager::instance().requestLayout();
}

void CaptureTab::applyView(std::size_t index) {
  if (m_viewSyncing) {
    return;
  }
  applyOpenView(viewFromIndex(index), true);
}

void CaptureTab::persistView() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString(
      "capture_tab", "view", m_view == View::Record ? "record" : "screenshot"
  );
}

void CaptureTab::syncHosts() {
  const bool record = m_view == View::Record;
  setShown(m_screenshotHost, !record);
  setShown(m_recordHost, record);
}

void CaptureTab::syncSettingsSection() {
  if (m_settings == nullptr) {
    return;
  }
  m_settings->setSection(m_view == View::Record ? "screen-record" : "screenshot");
}

void CaptureTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void CaptureTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (m_view == View::Screenshot && m_screenshot != nullptr) {
    m_screenshot->updateContent(renderer);
  } else if (m_view == View::Record && m_record != nullptr) {
    m_record->updateContent(renderer);
  }
}

void CaptureTab::onPanelCardOpacityChanged(float opacity) {
  if (m_viewSwitch != nullptr) {
    m_viewSwitch->setSurfaceOpacity(opacity);
  }
  if (m_screenshot != nullptr) {
    m_screenshot->setPanelCardOpacity(opacity);
  }
  if (m_record != nullptr) {
    m_record->setPanelCardOpacity(opacity);
  }
}
