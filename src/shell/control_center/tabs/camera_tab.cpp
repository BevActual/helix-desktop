#include "shell/control_center/tabs/camera_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  constexpr float kSplitMinWidth = 440.0F;
  constexpr float kPreviewPaneMinWidth = 180.0F;
  constexpr float kInspectorMinWidth = 200.0F;

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  [[nodiscard]] std::string folded(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }

  // Picture / Exposure / White Balance / Focus follow Camera Hub and Logitech
  // Capture. Zoom/pan/tilt stay with the preview (Apple's Video menu framing).
  enum class ControlSection : std::uint8_t { Picture, Exposure, WhiteBalance, Focus, Framing, Other };

  const char* sectionTitleKey(ControlSection section) {
    switch (section) {
    case ControlSection::Picture:
      return "control-center.camera.sections.picture";
    case ControlSection::Exposure:
      return "control-center.camera.sections.exposure";
    case ControlSection::WhiteBalance:
      return "control-center.camera.sections.white-balance";
    case ControlSection::Focus:
      return "control-center.camera.sections.focus";
    case ControlSection::Framing:
    case ControlSection::Other:
      break;
    }
    return "control-center.camera.sections.other";
  }

  ControlSection sectionFor(std::uint32_t id) {
    namespace ids = camera_control_ids;
    if (id == ids::exposureAuto || id == ids::exposureAbsolute || id == ids::gain
        || id == ids::backlightCompensation || id == ids::powerLineFrequency) {
      return ControlSection::Exposure;
    }
    if (id == ids::autoWhiteBalance || id == ids::whiteBalanceTemperature) {
      return ControlSection::WhiteBalance;
    }
    if (id == ids::zoomAbsolute || id == ids::panAbsolute || id == ids::tiltAbsolute) {
      return ControlSection::Framing;
    }
    if (id == ids::focusAuto || id == ids::focusAbsolute) {
      return ControlSection::Focus;
    }
    if (id == ids::brightness || id == ids::contrast || id == ids::saturation || id == ids::sharpness
        || id == ids::hue) {
      return ControlSection::Picture;
    }
    return ControlSection::Other;
  }

  constexpr std::array<ControlSection, 5> kInspectorOrder{
      ControlSection::Picture, ControlSection::Exposure, ControlSection::WhiteBalance, ControlSection::Focus,
      ControlSection::Other,
  };

  bool selectedMenuItemLooksManual(const CameraControl& control) {
    for (const auto& item : control.menuItems) {
      if (item.index != control.value) {
        continue;
      }
      return folded(item.name).find("manual") != std::string::npos;
    }
    return false;
  }

  const CameraControl* findControl(const std::vector<CameraControl>& controls, std::uint32_t id) {
    const auto it = std::ranges::find(controls, id, &CameraControl::id);
    return it != controls.end() ? &*it : nullptr;
  }

  bool isAutoGate(const CameraControl& control) {
    namespace ids = camera_control_ids;
    return control.id == ids::exposureAuto || control.id == ids::autoWhiteBalance || control.id == ids::focusAuto;
  }

  [[nodiscard]] std::string integerLabel(double value) {
    return std::to_string(static_cast<int>(std::lround(value)));
  }

  [[nodiscard]] std::string kelvinLabel(double value) {
    return std::format("{} K", static_cast<int>(std::lround(value)));
  }

  // V4L2_CID_EXPOSURE_ABSOLUTE is in 100 µs units.
  [[nodiscard]] std::string shutterLabel(double value) {
    const double seconds = value * 1.0e-4;
    if (seconds <= 0.0) {
      return integerLabel(value);
    }
    const int denom = static_cast<int>(std::lround(1.0 / seconds));
    if (denom >= 2) {
      return std::format("1/{}", denom);
    }
    return std::format("{:.3f}s", seconds);
  }

  [[nodiscard]] std::string controlTitle(const CameraControl& control) {
    namespace ids = camera_control_ids;
    if (control.id == ids::brightness) {
      return i18n::tr("control-center.camera.controls.brightness");
    }
    if (control.id == ids::contrast) {
      return i18n::tr("control-center.camera.controls.contrast");
    }
    if (control.id == ids::saturation) {
      return i18n::tr("control-center.camera.controls.saturation");
    }
    if (control.id == ids::sharpness) {
      return i18n::tr("control-center.camera.controls.sharpness");
    }
    if (control.id == ids::hue) {
      return i18n::tr("control-center.camera.controls.hue");
    }
    if (control.id == ids::gamma) {
      return i18n::tr("control-center.camera.controls.gamma");
    }
    if (control.id == ids::gain) {
      return i18n::tr("control-center.camera.controls.gain");
    }
    if (control.id == ids::backlightCompensation) {
      return i18n::tr("control-center.camera.controls.backlight");
    }
    if (control.id == ids::powerLineFrequency) {
      return i18n::tr("control-center.camera.controls.flicker");
    }
    if (control.id == ids::whiteBalanceTemperature) {
      return i18n::tr("control-center.camera.controls.temperature");
    }
    if (control.id == ids::exposureAbsolute) {
      return i18n::tr("control-center.camera.controls.shutter");
    }
    if (control.id == ids::focusAbsolute) {
      return i18n::tr("control-center.camera.controls.focus");
    }
    if (control.id == ids::zoomAbsolute) {
      return i18n::tr("control-center.camera.controls.zoom");
    }
    if (control.id == ids::panAbsolute) {
      return i18n::tr("control-center.camera.controls.pan");
    }
    if (control.id == ids::tiltAbsolute) {
      return i18n::tr("control-center.camera.controls.tilt");
    }
    return control.name;
  }

  [[nodiscard]] std::string menuItemLabel(const CameraControl& control, const CameraControlMenuItem& item) {
    if (control.id != camera_control_ids::exposureAuto) {
      return item.name;
    }
    const std::string lower = folded(item.name);
    if (lower.find("manual") != std::string::npos) {
      return i18n::tr("control-center.camera.controls.manual");
    }
    if (lower.find("shutter") != std::string::npos) {
      return i18n::tr("control-center.camera.controls.shutter-priority");
    }
    if (lower.find("aperture") != std::string::npos) {
      return i18n::tr("control-center.camera.controls.aperture-priority");
    }
    if (lower.find("auto") != std::string::npos) {
      return i18n::tr("control-center.camera.controls.auto");
    }
    return item.name;
  }

  std::function<std::string(double)> formatFor(const CameraControl& control) {
    if (control.id == camera_control_ids::whiteBalanceTemperature) {
      return kelvinLabel;
    }
    if (control.id == camera_control_ids::exposureAbsolute) {
      return shutterLabel;
    }
    return integerLabel;
  }

  void addLiveSlider(
      Flex& parent, float scale, const std::string& title, bool enabled, double minValue, double maxValue, double step,
      double value, std::function<std::string(double)> format, std::function<void(double)> onChange
  ) {
    Label* valueLabel = nullptr;
    auto header = makeControlRow(title, scale, nullptr, false);
    header->addChild(
        ui::label({
            .out = &valueLabel,
            .text = format(value),
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    parent.addChild(std::move(header));
    parent.addChild(
        ui::slider({
            .minValue = minValue,
            .maxValue = maxValue,
            .step = step,
            .value = value,
            .enabled = enabled,
            .trackHeight = Style::sliderTrackHeight * scale,
            .thumbSize = Style::sliderThumbSize * scale,
            .controlHeight = Style::controlHeight * scale,
            .flexGrow = 1.0F,
            .onValueChanged =
                [valueLabel, format, callback = std::move(onChange)](double next) {
                  if (valueLabel != nullptr) {
                    valueLabel->setText(format(next));
                  }
                  if (callback) {
                    callback(next);
                  }
                },
        })
    );
  }

  std::vector<const CameraControl*> membersOf(
      const std::vector<CameraControl>& controls, ControlSection section, bool includeAuto
  ) {
    std::vector<const CameraControl*> members;
    for (const auto& control : controls) {
      if (sectionFor(control.id) != section) {
        continue;
      }
      if (!includeAuto && isAutoGate(control)) {
        continue;
      }
      members.push_back(&control);
    }
    return members;
  }

} // namespace

CameraTab::CameraTab(const ControlCenterServices& services)
    : m_config(services.config), m_camera(services.camera),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "camera", .showConfigureButton = false}
      )) {
  if (m_camera != nullptr) {
    m_camera->setFrameCallback([this](int width, int height, const std::uint8_t* rgba) {
      onFrame(width, height, rgba);
    });
  }
}

std::unique_ptr<Flex> CameraTab::create() {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  m_devices = m_camera != nullptr ? m_camera->listDevices() : std::vector<CameraDevice>{};

  if (m_devices.empty()) {
    auto emptyCard = ui::column({
        .out = &m_previewCard,
        .fillWidth = true,
        .configure =
            [scale, opacity = panelCardOpacity()](Flex& section) { applyCompactCardStyle(section, scale, opacity, false); },
    });
    emptyCard->addChild(
        ui::label({
            .out = &m_emptyLabel,
            .text = i18n::tr("control-center.camera.empty"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 2,
        })
    );
    emptyCard->addChild(
        ui::label({
            .text = i18n::tr("control-center.camera.empty-hint"),
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    live->addChild(std::move(emptyCard));
    syncHeaderActions();
    return m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
  }

  const bool haveSelection =
      std::ranges::any_of(m_devices, [this](const CameraDevice& device) { return device.id == m_selectedDeviceId; });
  if (!haveSelection) {
    const auto saved = m_config != nullptr ? m_config->stateString("camera_tab", "device") : std::nullopt;
    const bool savedValid = saved.has_value()
        && std::ranges::any_of(m_devices, [&](const CameraDevice& device) { return device.id == *saved; });
    m_selectedDeviceId = savedValid ? *saved : m_devices.front().id;
  }

  auto stage = ui::row({
      .out = &m_stage,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });

  auto previewColumn = ui::column({
      .out = &m_previewColumn,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .fillWidth = true,
      .flexGrow = 1.15F,
  });

  auto previewCard = ui::column({
      .out = &m_previewCard,
      .fillWidth = true,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applyCompactCardStyle(section, scale, opacity, false); },
  });

  if (m_devices.size() > 1) {
    std::vector<std::string> labels;
    labels.reserve(m_devices.size());
    std::size_t selectedIndex = 0;
    for (std::size_t i = 0; i < m_devices.size(); ++i) {
      labels.push_back(m_devices[i].name);
      if (m_devices[i].id == m_selectedDeviceId) {
        selectedIndex = i;
      }
    }
    m_deviceSelect = addSelectRow(
        *previewCard, scale, i18n::tr("control-center.camera.device"), std::move(labels), panelCardOpacity(),
        [this](std::size_t index) { selectDevice(index); }
    );
    if (m_deviceSelect != nullptr) {
      m_deviceSelect->setSelectedIndexSilently(selectedIndex);
    }
  } else {
    previewCard->addChild(makeCardHeaderRow(m_devices.front().name, scale));
  }

  auto previewBox = ui::column({
      .out = &m_previewBox,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .fill = colorSpecFromRole(ColorRole::Surface),
      .radius = Style::scaledRadiusMd(scale),
      .minHeight = 140.0F * scale,
      .fillWidth = true,
      .clipChildren = true,
  });
  previewBox->addChild(
      ui::image({
          .out = &m_previewImage,
          .fit = ImageFit::Contain,
          .radius = Style::scaledRadiusMd(scale),
          .participatesInLayout = false,
      })
  );
  previewCard->addChild(std::move(previewBox));
  previewCard->addChild(
      ui::label({
          .out = &m_previewUnavailableLabel,
          .text = i18n::tr("control-center.camera.preview-unavailable"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  previewCard->addChild(
      ui::column({
          .out = &m_framingHost,
          .align = FlexAlign::Stretch,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  previewColumn->addChild(std::move(previewCard));
  stage->addChild(std::move(previewColumn));

  auto controlsScroll = ui::scrollView({
      .out = &m_controlsScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_controlsHost = controlsScroll->content();
  m_controlsHost->setDirection(FlexDirection::Vertical);
  m_controlsHost->setAlign(FlexAlign::Stretch);
  m_controlsHost->setGap(Style::spaceMd * scale);
  stage->addChild(std::move(controlsScroll));
  live->addChild(std::move(stage));

  m_previewBound = false;
  if (m_active) {
    startSelectedDevice();
  }
  rebuildControls();
  syncHeaderActions();

  return m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
}

std::unique_ptr<Flex> CameraTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_resetButton,
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.camera.reset-tooltip"),
          .onClick =
              [this]() {
                if (m_camera != nullptr) {
                  m_camera->resetControlsToDefaults();
                }
                rebuildControls();
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  syncHeaderActions();
  return m_settings->embedHeader(std::move(live), scale);
}

void CameraTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (active) {
    startSelectedDevice();
    rebuildControls();
  } else if (m_camera != nullptr) {
    m_camera->stop();
  }
}

void CameraTab::selectDevice(std::size_t index) {
  if (index >= m_devices.size()) {
    return;
  }
  m_selectedDeviceId = m_devices[index].id;
  if (m_config != nullptr) {
    (void)m_config->setStateString("camera_tab", "device", m_selectedDeviceId);
  }
  m_previewBound = false;
  if (m_active) {
    startSelectedDevice();
  }
  rebuildControls();
}

void CameraTab::startSelectedDevice() {
  if (m_camera == nullptr || m_selectedDeviceId.empty()) {
    return;
  }
  if (m_camera->active() && m_camera->activeDeviceId() == m_selectedDeviceId && m_camera->previewAvailable()) {
    syncPreviewAvailability();
    return;
  }
  m_camera->start(m_selectedDeviceId);
  syncPreviewAvailability();
}

void CameraTab::syncPreviewAvailability() {
  const bool preview = m_camera != nullptr && m_camera->previewAvailable();
  bool layoutDirty = setShown(m_previewBox, preview) || setShown(m_previewImage, preview);
  layoutDirty = setShown(m_previewUnavailableLabel, !preview && m_camera != nullptr && m_camera->active())
      || layoutDirty;
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void CameraTab::syncHeaderActions() {
  if (setShown(m_resetButton, !m_selectedDeviceId.empty())) {
    PanelManager::instance().requestLayout();
  }
}

void CameraTab::layoutStage(float contentWidth, float bodyHeight) {
  (void)bodyHeight;
  if (m_stage == nullptr || m_previewColumn == nullptr || m_controlsScroll == nullptr) {
    return;
  }
  const float scale = contentScale();
  const bool preview = m_previewBox != nullptr && m_previewBox->visible();
  const bool split = preview && contentWidth >= kSplitMinWidth * scale;
  m_stage->setDirection(split ? FlexDirection::Horizontal : FlexDirection::Vertical);
  m_previewColumn->setFlexGrow(split ? 1.15F : 0.0F);
  m_previewColumn->setMinWidth(split ? kPreviewPaneMinWidth * scale : 0.0F);
  m_previewColumn->setFillHeight(split);
  m_controlsScroll->setFlexGrow(1.0F);
  m_controlsScroll->setMinWidth(split ? kInspectorMinWidth * scale : 0.0F);
  m_controlsScroll->setFillHeight(true);
}

void CameraTab::layoutPreviewBox(float contentWidth, float bodyHeight) {
  if (m_previewBox == nullptr || !m_previewBox->visible()) {
    return;
  }
  const float scale = contentScale();
  const bool split = m_stage != nullptr && m_stage->direction() == FlexDirection::Horizontal;
  const float cardPad = Style::spaceMd * 2.0F * scale;
  float innerWidth = contentWidth - cardPad;
  if (m_previewCard != nullptr && m_previewCard->width() > cardPad + 1.0F) {
    innerWidth = m_previewCard->width() - cardPad;
  }
  innerWidth = std::max(1.0F, innerWidth);

  float aspect = 16.0F / 9.0F;
  if (m_camera != nullptr && m_camera->previewWidth() > 0 && m_camera->previewHeight() > 0) {
    aspect = static_cast<float>(m_camera->previewWidth()) / static_cast<float>(m_camera->previewHeight());
  } else if (m_previewImage != nullptr && m_previewImage->hasImage() && m_previewImage->aspectRatio() > 0.0F) {
    aspect = m_previewImage->aspectRatio();
  }

  const float minHeight = 120.0F * scale;
  float chrome = cardPad + Style::controlHeightSm * scale + Style::spaceMd * scale;
  if (m_framingHost != nullptr && m_framingHost->visible()) {
    chrome += m_framingHost->height() + Style::spaceSm * scale;
  }
  float maxHeight = 240.0F * scale;
  if (split) {
    maxHeight = std::max(minHeight, bodyHeight - chrome);
  } else {
    const float leftoverMin = 160.0F * scale;
    maxHeight = std::min(maxHeight, std::max(minHeight, bodyHeight - chrome - leftoverMin));
  }
  const float height = std::clamp(innerWidth / aspect, minHeight, maxHeight);
  m_previewBox->setFlexGrow(0.0F);
  m_previewBox->setMinHeight(height);
  m_previewBox->setMaxHeight(height);
}

void CameraTab::pinPreviewImage() {
  if (m_previewBox == nullptr || m_previewImage == nullptr || !m_previewBox->visible()) {
    return;
  }
  m_previewImage->setPosition(0.0F, 0.0F);
  m_previewImage->setSize(m_previewBox->width(), m_previewBox->height());
}

void CameraTab::appendControl(Flex& parent, const CameraControl& control, bool withDivider) {
  const float scale = contentScale();
  auto host = ui::column({.align = FlexAlign::Stretch});
  if (withDivider) {
    addWellDivider(*host, scale);
  }

  const std::uint32_t id = control.id;
  const std::string title = controlTitle(control);
  switch (control.type) {
  case CameraControlType::Boolean: {
    Toggle* toggle = addToggleRow(*host, scale, title, [this, id](bool enabled) {
      if (m_camera != nullptr) {
        m_camera->setControl(id, enabled ? 1 : 0);
      }
      refreshDependentVisibility();
    });
    if (toggle != nullptr) {
      toggle->setCheckedImmediate(control.value != 0);
      toggle->setEnabled(!control.readOnly);
    }
    break;
  }
  case CameraControlType::Menu: {
    std::vector<std::string> labels;
    std::size_t selectedIndex = 0;
    labels.reserve(control.menuItems.size());
    for (std::size_t i = 0; i < control.menuItems.size(); ++i) {
      labels.push_back(menuItemLabel(control, control.menuItems[i]));
      if (control.menuItems[i].index == control.value) {
        selectedIndex = i;
      }
    }
    const auto items = control.menuItems;
    Select* select = addSelectRow(
        *host, scale, title, std::move(labels), panelCardOpacity(),
        [this, id, items](std::size_t index) {
          if (m_camera != nullptr && index < items.size()) {
            m_camera->setControl(id, items[index].index);
          }
          refreshDependentVisibility();
        }
    );
    if (select != nullptr) {
      select->setSelectedIndexSilently(selectedIndex);
      select->setEnabled(!control.readOnly);
    }
    break;
  }
  case CameraControlType::Integer:
  default: {
    std::int32_t last = control.value;
    addLiveSlider(
        *host, scale, title, !control.readOnly, static_cast<double>(control.minimum),
        static_cast<double>(control.maximum), static_cast<double>(control.step), static_cast<double>(control.value),
        formatFor(control),
        [this, id, last](double next) mutable {
          if (m_camera == nullptr) {
            return;
          }
          const auto rounded = static_cast<std::int32_t>(std::lround(next));
          if (rounded == last) {
            return;
          }
          last = rounded;
          m_camera->setControl(id, rounded);
        }
    );
    break;
  }
  }

  m_controlRows[control.id] = host.get();
  parent.addChild(std::move(host));
}

void CameraTab::rebuildControls() {
  m_controlRows.clear();
  if (m_framingHost != nullptr) {
    while (!m_framingHost->children().empty()) {
      m_framingHost->removeChild(m_framingHost->children().back().get());
    }
  }
  if (m_controlsHost != nullptr) {
    while (!m_controlsHost->children().empty()) {
      m_controlsHost->removeChild(m_controlsHost->children().back().get());
    }
  }
  if (m_camera == nullptr || !m_camera->active()) {
    (void)setShown(m_framingHost, false);
    return;
  }

  const float scale = contentScale();
  const std::vector<CameraControl> controls = m_camera->controls();

  const auto framing = membersOf(controls, ControlSection::Framing, true);
  if (m_framingHost != nullptr) {
    bool first = true;
    for (const CameraControl* control : framing) {
      appendControl(*m_framingHost, *control, !first);
      first = false;
    }
    (void)setShown(m_framingHost, !framing.empty());
  }

  if (m_controlsHost == nullptr) {
    refreshDependentVisibility();
    return;
  }

  for (const ControlSection section : kInspectorOrder) {
    const CameraControl* autoCtrl = nullptr;
    if (section == ControlSection::Exposure) {
      autoCtrl = findControl(controls, camera_control_ids::exposureAuto);
    } else if (section == ControlSection::WhiteBalance) {
      autoCtrl = findControl(controls, camera_control_ids::autoWhiteBalance);
    } else if (section == ControlSection::Focus) {
      autoCtrl = findControl(controls, camera_control_ids::focusAuto);
    }
    auto members = membersOf(controls, section, false);
    if (members.empty() && autoCtrl == nullptr) {
      continue;
    }

    auto well = ui::column({
        .align = FlexAlign::Stretch,
        .fillWidth = true,
        .configure = [scale](Flex& card) { applyContentCardStyle(card, scale); },
    });

    auto header = makeCardHeaderRow(i18n::tr(sectionTitleKey(section)), scale);
    if (autoCtrl != nullptr) {
      const std::uint32_t id = autoCtrl->id;
      if (autoCtrl->type == CameraControlType::Boolean) {
        Toggle* toggle = nullptr;
        header->addChild(
            ui::toggle({
                .out = &toggle,
                .checkedImmediate = autoCtrl->value != 0,
                .enabled = !autoCtrl->readOnly,
                .toggleSize = ToggleSize::Medium,
                .scale = scale,
                .onChange =
                    [this, id](bool enabled) {
                      if (m_camera != nullptr) {
                        m_camera->setControl(id, enabled ? 1 : 0);
                      }
                      refreshDependentVisibility();
                    },
            })
        );
      } else if (autoCtrl->type == CameraControlType::Menu && !autoCtrl->menuItems.empty()) {
        std::vector<std::string> labels;
        std::size_t selectedIndex = 0;
        labels.reserve(autoCtrl->menuItems.size());
        for (std::size_t i = 0; i < autoCtrl->menuItems.size(); ++i) {
          labels.push_back(menuItemLabel(*autoCtrl, autoCtrl->menuItems[i]));
          if (autoCtrl->menuItems[i].index == autoCtrl->value) {
            selectedIndex = i;
          }
        }
        const auto items = autoCtrl->menuItems;
        header->addChild(
            ui::select({
                .options = std::move(labels),
                .selectedIndex = selectedIndex,
                .fontSize = Style::fontSizeBody * scale,
                .controlHeight = Style::controlHeight * scale,
                .enabled = !autoCtrl->readOnly,
                .surfaceOpacity = 1.0F,
                .onSelectionChanged =
                    [this, id, items](std::size_t index, std::string_view) {
                      if (m_camera != nullptr && index < items.size()) {
                        m_camera->setControl(id, items[index].index);
                      }
                      refreshDependentVisibility();
                    },
            })
        );
      }
    }
    well->addChild(std::move(header));

    bool first = true;
    for (const CameraControl* control : members) {
      appendControl(*well, *control, !first);
      first = false;
    }
    m_controlsHost->addChild(std::move(well));
  }

  refreshDependentVisibility();
}

void CameraTab::refreshDependentVisibility() {
  if (m_camera == nullptr) {
    return;
  }
  namespace ids = camera_control_ids;
  const std::vector<CameraControl> controls = m_camera->controls();
  bool layoutDirty = false;

  if (const CameraControl* autoExposure = findControl(controls, ids::exposureAuto); autoExposure != nullptr) {
    if (const auto it = m_controlRows.find(ids::exposureAbsolute); it != m_controlRows.end()) {
      layoutDirty = setShown(it->second, selectedMenuItemLooksManual(*autoExposure)) || layoutDirty;
    }
  }
  if (const CameraControl* autoWhiteBalance = findControl(controls, ids::autoWhiteBalance);
      autoWhiteBalance != nullptr) {
    if (const auto it = m_controlRows.find(ids::whiteBalanceTemperature); it != m_controlRows.end()) {
      layoutDirty = setShown(it->second, autoWhiteBalance->value == 0) || layoutDirty;
    }
  }
  if (const CameraControl* autoFocus = findControl(controls, ids::focusAuto); autoFocus != nullptr) {
    if (const auto it = m_controlRows.find(ids::focusAbsolute); it != m_controlRows.end()) {
      layoutDirty = setShown(it->second, autoFocus->value == 0) || layoutDirty;
    }
  }
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void CameraTab::onFrame(int width, int height, const std::uint8_t* rgba) {
  if (width <= 0 || height <= 0) {
    return;
  }
  const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
  m_pendingFrame.assign(rgba, rgba + size);
  m_pendingWidth = width;
  m_pendingHeight = height;
  m_frameDirty = true;
  PanelManager::instance().refresh();
}

void CameraTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  if (m_camera != nullptr) {
    m_camera->stop();
  }
  m_active = false;
  m_previewBound = false;
  m_rootLayout = nullptr;
  m_stage = nullptr;
  m_previewColumn = nullptr;
  m_previewCard = nullptr;
  m_deviceSelect = nullptr;
  m_emptyLabel = nullptr;
  m_previewBox = nullptr;
  m_previewImage = nullptr;
  m_previewUnavailableLabel = nullptr;
  m_framingHost = nullptr;
  m_controlsScroll = nullptr;
  m_controlsHost = nullptr;
  m_resetButton = nullptr;
}

void CameraTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool CameraTab::configureMode() const { return m_settings->configureMode(); }

void CameraTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* CameraTab::settingsPane() { return m_settings.get(); }

void CameraTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  layoutStage(contentWidth, bodyHeight);
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  layoutPreviewBox(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  pinPreviewImage();
}

void CameraTab::doUpdate(Renderer& renderer) {
  if (m_settings != nullptr && m_settings->configureMode()) {
    return;
  }
  if (!m_frameDirty || m_previewImage == nullptr) {
    return;
  }
  m_frameDirty = false;

  auto& textures = renderer.textureManager();
  if (!m_previewBound || m_previewTexWidth != m_pendingWidth || m_previewTexHeight != m_pendingHeight
      || !m_previewTexture.valid()) {
    if (m_previewTexture.valid()) {
      textures.unload(m_previewTexture);
    }
    m_previewTexture =
        textures.createEmpty(m_pendingWidth, m_pendingHeight, TextureDataFormat::Rgba, TextureFilter::Linear);
    m_previewTexWidth = m_pendingWidth;
    m_previewTexHeight = m_pendingHeight;
    m_previewImage->setExternalTexture(renderer, m_previewTexture);
    m_previewBound = true;
    pinPreviewImage();
    PanelManager::instance().requestLayout();
  }
  textures.updateSubImage(
      m_previewTexture, m_pendingFrame.data(), 0, 0, m_pendingWidth, m_pendingHeight, TextureDataFormat::Rgba
  );
  PanelManager::instance().requestRedraw();
}
