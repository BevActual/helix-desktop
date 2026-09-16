#include "shell/control_center/tabs/camera_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
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
#include <string>
#include <utility>

using namespace control_center;

namespace {

  void setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return;
    }
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
  }

  // Sections mirror how Elgato Camera Hub organizes webcam controls: an Auto
  // toggle/mode sits above the manual slider it gates, grouped by what the
  // control affects rather than in raw driver enumeration order.
  enum class ControlSection : std::uint8_t { Exposure, WhiteBalance, Details, FieldOfView, Focus, Other };

  const char* sectionTitleKey(ControlSection section) {
    switch (section) {
    case ControlSection::Exposure:
      return "control-center.camera.sections.exposure";
    case ControlSection::WhiteBalance:
      return "control-center.camera.sections.white-balance";
    case ControlSection::Details:
      return "control-center.camera.sections.details";
    case ControlSection::FieldOfView:
      return "control-center.camera.sections.field-of-view";
    case ControlSection::Focus:
      return "control-center.camera.sections.focus";
    case ControlSection::Other:
      return "control-center.camera.sections.other";
    }
    return "control-center.camera.sections.other";
  }

  ControlSection sectionFor(std::uint32_t id) {
    namespace ids = camera_control_ids;
    if (id == ids::exposureAuto || id == ids::exposureAbsolute) {
      return ControlSection::Exposure;
    }
    if (id == ids::autoWhiteBalance || id == ids::whiteBalanceTemperature) {
      return ControlSection::WhiteBalance;
    }
    if (id == ids::zoomAbsolute || id == ids::panAbsolute || id == ids::tiltAbsolute) {
      return ControlSection::FieldOfView;
    }
    if (id == ids::focusAuto || id == ids::focusAbsolute) {
      return ControlSection::Focus;
    }
    if (id == ids::brightness || id == ids::contrast || id == ids::saturation || id == ids::sharpness
        || id == ids::hue || id == ids::gain || id == ids::gamma || id == ids::backlightCompensation
        || id == ids::powerLineFrequency) {
      return ControlSection::Details;
    }
    return ControlSection::Other;
  }

  // Order controls within a section the way Camera Hub does: the auto
  // mode/toggle first, then the manual value it gates.
  constexpr std::array<ControlSection, 6> kSectionOrder{
      ControlSection::Exposure, ControlSection::WhiteBalance, ControlSection::Details,
      ControlSection::FieldOfView, ControlSection::Focus, ControlSection::Other,
  };

  // For V4L2_CID_EXPOSURE_AUTO, UVC drivers report menu items like "Manual
  // Mode" / "Aperture Priority Mode" rather than a plain boolean; treat the
  // currently selected item's name as the source of truth for "manual".
  bool selectedMenuItemLooksManual(const CameraControl& control) {
    for (const auto& item : control.menuItems) {
      if (item.index != control.value) {
        continue;
      }
      std::string lower = item.name;
      std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return lower.find("manual") != std::string::npos;
    }
    return false;
  }

  const CameraControl* findControl(const std::vector<CameraControl>& controls, std::uint32_t id) {
    const auto it = std::ranges::find(controls, id, &CameraControl::id);
    return it != controls.end() ? &*it : nullptr;
  }

} // namespace

CameraTab::CameraTab(const ControlCenterServices& services)
    : m_config(services.config), m_camera(services.camera),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "camera"})) {
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

  m_devices = m_camera != nullptr ? m_camera->listDevices() : std::vector<CameraDevice>{};

  if (m_devices.empty()) {
    auto emptyCard = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure =
            [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
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
    content->addChild(std::move(emptyCard));
    live->addChild(std::move(scroll));
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
    content->addChild(
        ui::select({
            .out = &m_deviceSelect,
            .options = labels,
            .selectedIndex = selectedIndex,
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .flexGrow = 1.0F,
            .onSelectionChanged = [this](std::size_t index, std::string_view) { selectDevice(index); },
        })
    );
  }

  auto previewBox = ui::column({
      .out = &m_previewBox,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .fillWidth = true,
      .height = 220.0F * scale,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& section) { applySectionCardStyle(section, scale, opacity); },
  });
  previewBox->addChild(
      ui::image({
          .out = &m_previewImage,
          .fit = ImageFit::Contain,
          .radius = Style::scaledRadiusMd(scale),
          .flexGrow = 1.0F,
      })
  );
  previewBox->addChild(
      ui::label({
          .out = &m_previewUnavailableLabel,
          .text = i18n::tr("control-center.camera.preview-unavailable"),
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
          .participatesInLayout = false,
      })
  );
  content->addChild(std::move(previewBox));

  content->addChild(
      ui::column({
          .out = &m_controlsHost,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
      })
  );

  content->addChild(
      ui::button({
          .out = &m_resetButton,
          .text = i18n::tr("control-center.camera.reset"),
          .glyph = "refresh",
          .fontSize = Style::fontSizeBody * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Default,
          .tooltip = i18n::tr("control-center.camera.reset-tooltip"),
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this]() {
                if (m_camera != nullptr) {
                  m_camera->resetControlsToDefaults();
                }
                rebuildControls();
              },
      })
  );

  m_previewBound = false;
  if (m_active) {
    startSelectedDevice();
  }
  rebuildControls();

  live->addChild(std::move(scroll));
  return m_settings->wrapLive(std::move(live), scale, panelCardOpacity());
}

std::unique_ptr<Flex> CameraTab::createHeaderActions() { return m_settings->embedHeader(nullptr, contentScale()); }

void CameraTab::setActive(bool active) {
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
  m_camera->start(m_selectedDeviceId);
  setShown(m_previewImage, m_camera->previewAvailable());
  setShown(m_previewUnavailableLabel, !m_camera->previewAvailable());
}

void CameraTab::rebuildControls() {
  if (m_controlsHost == nullptr) {
    return;
  }
  while (!m_controlsHost->children().empty()) {
    m_controlsHost->removeChild(m_controlsHost->children().back().get());
  }
  m_controlRows.clear();
  if (m_camera == nullptr || !m_camera->active()) {
    return;
  }

  const float scale = contentScale();
  const std::vector<CameraControl> controls = m_camera->controls();

  for (const ControlSection section : kSectionOrder) {
    std::vector<const CameraControl*> members;
    for (const auto& control : controls) {
      if (sectionFor(control.id) == section) {
        members.push_back(&control);
      }
    }
    if (members.empty()) {
      continue;
    }
    // Within a section, put the Auto mode/toggle ahead of the manual value it gates
    // (Exposure Auto before Shutter Speed, Auto White Balance before Temperature, etc).
    std::ranges::stable_sort(members, [](const CameraControl* a, const CameraControl* b) {
      const bool aIsAuto = a->type != CameraControlType::Integer;
      const bool bIsAuto = b->type != CameraControlType::Integer;
      return aIsAuto && !bIsAuto;
    });

    addTitle(*m_controlsHost, i18n::tr(sectionTitleKey(section)), scale);
    for (const CameraControl* control : members) {
      auto row = buildControlRow(*control);
      m_controlRows[control->id] = row.get();
      m_controlsHost->addChild(std::move(row));
    }
  }

  refreshDependentVisibility();
}

std::unique_ptr<Flex> CameraTab::buildControlRow(const CameraControl& control) {
  const float scale = contentScale();
  const float captionSize = Style::fontSizeCaption * scale;

  auto row = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});

  const std::uint32_t id = control.id;
  if (control.type == CameraControlType::Boolean) {
    auto toggleRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    toggleRow->addChild(
        ui::label({
            .text = control.name,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
    toggleRow->addChild(
        ui::toggle({
            .checkedImmediate = control.value != 0,
            .enabled = !control.readOnly,
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
    row->addChild(std::move(toggleRow));
    return row;
  }

  row->addChild(
      ui::label({
          .text = control.name,
          .fontSize = captionSize,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  );

  switch (control.type) {
  case CameraControlType::Boolean:
    break;
  case CameraControlType::Menu: {
    std::vector<std::string> labels;
    std::size_t selectedIndex = 0;
    labels.reserve(control.menuItems.size());
    for (std::size_t i = 0; i < control.menuItems.size(); ++i) {
      labels.push_back(control.menuItems[i].name);
      if (control.menuItems[i].index == control.value) {
        selectedIndex = i;
      }
    }
    const auto items = control.menuItems;
    row->addChild(
        ui::select({
            .options = labels,
            .selectedIndex = selectedIndex,
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .enabled = !control.readOnly,
            .flexGrow = 1.0F,
            .onSelectionChanged =
                [this, id, items](std::size_t index, std::string_view) {
                  if (m_camera != nullptr && index < items.size()) {
                    m_camera->setControl(id, items[index].index);
                  }
                  refreshDependentVisibility();
                },
        })
    );
    break;
  }
  case CameraControlType::Integer:
  default: {
    Label* valueLabel = nullptr;
    Slider* slider = nullptr;
    auto sliderRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    sliderRow->addChild(
        ui::slider({
            .out = &slider,
            .minValue = static_cast<double>(control.minimum),
            .maxValue = static_cast<double>(control.maximum),
            .step = static_cast<double>(control.step),
            .value = static_cast<double>(control.value),
            .enabled = !control.readOnly,
            .thumbSize = Style::sliderThumbSize * scale,
            .controlHeight = Style::controlHeight * scale,
            .flexGrow = 1.0F,
            .onValueChanged =
                [valueLabel](double value) mutable {
                  if (valueLabel != nullptr) {
                    valueLabel->setText(std::to_string(static_cast<int>(std::lround(value))));
                  }
                },
            .onDragEnd =
                [this, id, slider]() {
                  if (m_camera != nullptr && slider != nullptr) {
                    m_camera->setControl(id, static_cast<std::int32_t>(std::lround(slider->value())));
                  }
                },
        })
    );
    sliderRow->addChild(
        ui::label({
            .out = &valueLabel,
            .text = std::to_string(control.value),
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .minWidth = Style::controlHeightSm * scale,
            .textAlign = TextAlign::End,
        })
    );
    row->addChild(std::move(sliderRow));
    break;
  }
  }
  return row;
}

void CameraTab::refreshDependentVisibility() {
  if (m_camera == nullptr) {
    return;
  }
  namespace ids = camera_control_ids;
  const std::vector<CameraControl> controls = m_camera->controls();

  if (const CameraControl* autoExposure = findControl(controls, ids::exposureAuto); autoExposure != nullptr) {
    if (const auto it = m_controlRows.find(ids::exposureAbsolute); it != m_controlRows.end()) {
      setShown(it->second, selectedMenuItemLooksManual(*autoExposure));
    }
  }
  if (const CameraControl* autoWhiteBalance = findControl(controls, ids::autoWhiteBalance);
      autoWhiteBalance != nullptr) {
    if (const auto it = m_controlRows.find(ids::whiteBalanceTemperature); it != m_controlRows.end()) {
      setShown(it->second, autoWhiteBalance->value == 0);
    }
  }
  if (const CameraControl* autoFocus = findControl(controls, ids::focusAuto); autoFocus != nullptr) {
    if (const auto it = m_controlRows.find(ids::focusAbsolute); it != m_controlRows.end()) {
      setShown(it->second, autoFocus->value == 0);
    }
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
  m_deviceSelect = nullptr;
  m_emptyLabel = nullptr;
  m_previewBox = nullptr;
  m_previewImage = nullptr;
  m_previewUnavailableLabel = nullptr;
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
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
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
  }
  textures.updateSubImage(
      m_previewTexture, m_pendingFrame.data(), 0, 0, m_pendingWidth, m_pendingHeight, TextureDataFormat::Rgba
  );
}
