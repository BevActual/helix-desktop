#include "capture/screenshot_region_overlay.h"

#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "core/color.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace capture {
  namespace {

    constexpr Logger kLog("screenshot-region");
    constexpr float kDimensionFontSize = 14.0F;
    constexpr float kDimensionCursorOffsetX = 12.0F;
    constexpr float kDimensionCursorOffsetY = 14.0F;
    constexpr float kDimensionPaddingX = 6.0F;
    constexpr float kDimensionPaddingY = 4.0F;
    constexpr float kSelectionBorderWidth = 2.0F;
    constexpr float kDimOpacity = 0.65F;
    constexpr float kColorSwatchSize = 16.0F;
    constexpr int kMagnifierRadius = 5;
    constexpr int kMagnifierScale = 8;
    constexpr int kMagnifierSrc = kMagnifierRadius * 2 + 1;
    constexpr int kMagnifierDst = kMagnifierSrc * kMagnifierScale;

    struct SampledPixel {
      std::string hex;
      Color color;
      std::uint8_t r = 0;
      std::uint8_t g = 0;
      std::uint8_t b = 0;
    };

    [[nodiscard]] capture::DragMode hitTestSelection(double x, double y, double x0, double y0, double x1, double y1) {
      constexpr double kHandleMargin = 15.0; // Hitbox size in pixels

      const bool withinX = x >= x0 - kHandleMargin && x <= x1 + kHandleMargin;
      const bool withinY = y >= y0 - kHandleMargin && y <= y1 + kHandleMargin;

      if (!withinX || !withinY)
        return capture::DragMode::None;

      const double distLeft = std::abs(x - x0);
      const double distRight = std::abs(x - x1);
      const double distTop = std::abs(y - y0);
      const double distBottom = std::abs(y - y1);

      bool nearLeft = distLeft <= kHandleMargin;
      bool nearRight = distRight <= kHandleMargin;
      bool nearTop = distTop <= kHandleMargin;
      bool nearBottom = distBottom <= kHandleMargin;

      // Narrow selections can overlap opposing hit zones; bind the pointer to the nearer edge.
      if (nearLeft && nearRight) {
        if (distLeft < distRight)
          nearRight = false;
        else
          nearLeft = false;
      }
      if (nearTop && nearBottom) {
        if (distTop < distBottom)
          nearBottom = false;
        else
          nearTop = false;
      }

      if (nearTop && nearLeft)
        return capture::DragMode::TopLeftCorner;
      if (nearTop && nearRight)
        return capture::DragMode::TopRightCorner;
      if (nearBottom && nearLeft)
        return capture::DragMode::BottomLeftCorner;
      if (nearBottom && nearRight)
        return capture::DragMode::BottomRightCorner;

      if (nearTop && x >= x0 && x <= x1)
        return capture::DragMode::TopEdge;
      if (nearBottom && x >= x0 && x <= x1)
        return capture::DragMode::BottomEdge;
      if (nearLeft && y >= y0 && y <= y1)
        return capture::DragMode::LeftEdge;
      if (nearRight && y >= y0 && y <= y1)
        return capture::DragMode::RightEdge;

      if (x > x0 && x < x1 && y > y0 && y < y1)
        return capture::DragMode::Move;

      return capture::DragMode::None;
    }

    [[nodiscard]] std::uint32_t cursorShapeForDragMode(capture::DragMode mode) {
      switch (mode) {
      case capture::DragMode::TopEdge:
      case capture::DragMode::BottomEdge:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
      case capture::DragMode::LeftEdge:
      case capture::DragMode::RightEdge:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
      case capture::DragMode::TopLeftCorner:
      case capture::DragMode::BottomRightCorner:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
      case capture::DragMode::TopRightCorner:
      case capture::DragMode::BottomLeftCorner:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
      case capture::DragMode::Move:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL;
      default:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
      }
    }

    [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
      for (const auto& entry : wayland.outputs()) {
        if (entry.output == output) {
          return &entry;
        }
      }
      return nullptr;
    }

    [[nodiscard]] LayerShellKeyboard overlayKeyboardMode() { return LayerShellKeyboard::Exclusive; }

    [[nodiscard]] const ScreencopyImage*
    frozenImageForOutput(const std::vector<FrozenScreenshot>& screenshots, wl_output* output) {
      for (const auto& entry : screenshots) {
        if (entry.output == output) {
          return &entry.image.image;
        }
      }
      return nullptr;
    }

    [[nodiscard]] std::optional<std::pair<int, int>>
    pixelCoordAtLocal(const ScreencopyImage& image, int logicalWidth, int logicalHeight, double localX, double localY) {
      if (image.width <= 0 || image.height <= 0 || logicalWidth <= 0 || logicalHeight <= 0 || image.rgba.size() < 4) {
        return std::nullopt;
      }
      const int pixelX = std::clamp(
          static_cast<int>(std::floor(localX * static_cast<double>(image.width) / static_cast<double>(logicalWidth))), 0,
          image.width - 1
      );
      int pixelY = std::clamp(
          static_cast<int>(std::floor(localY * static_cast<double>(image.height) / static_cast<double>(logicalHeight))),
          0, image.height - 1
      );
      if (image.yInvert) {
        pixelY = image.height - 1 - pixelY;
      }
      return std::pair<int, int>{pixelX, pixelY};
    }

    [[nodiscard]] std::optional<SampledPixel> samplePixel(const ScreencopyImage& image, int pixelX, int pixelY) {
      if (pixelX < 0 || pixelY < 0 || pixelX >= image.width || pixelY >= image.height) {
        return std::nullopt;
      }
      const std::size_t index =
          (static_cast<std::size_t>(pixelY) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(pixelX))
          * 4U;
      if (index + 2 >= image.rgba.size()) {
        return std::nullopt;
      }
      const std::uint8_t r = image.rgba[index];
      const std::uint8_t g = image.rgba[index + 1];
      const std::uint8_t b = image.rgba[index + 2];
      char buf[8];
      std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
      return SampledPixel{
          .hex = std::string(buf),
          .color = rgba(colorByte(r), colorByte(g), colorByte(b)),
          .r = r,
          .g = g,
          .b = b,
      };
    }

    [[nodiscard]] std::optional<SampledPixel>
    pixelAtLocal(const ScreencopyImage& image, int logicalWidth, int logicalHeight, double localX, double localY) {
      const auto coord = pixelCoordAtLocal(image, logicalWidth, logicalHeight, localX, localY);
      if (!coord.has_value()) {
        return std::nullopt;
      }
      return samplePixel(image, coord->first, coord->second);
    }

    void fillMagnifier(
        const ScreencopyImage& image, int logicalWidth, int logicalHeight, double localX, double localY,
        std::vector<std::uint8_t>& out
    ) {
      out.assign(static_cast<std::size_t>(kMagnifierDst) * static_cast<std::size_t>(kMagnifierDst) * 4U, 0);
      const auto coord = pixelCoordAtLocal(image, logicalWidth, logicalHeight, localX, localY);
      if (!coord.has_value()) {
        return;
      }
      std::uint8_t centerR = 0;
      std::uint8_t centerG = 0;
      std::uint8_t centerB = 0;
      for (int dy = -kMagnifierRadius; dy <= kMagnifierRadius; ++dy) {
        for (int dx = -kMagnifierRadius; dx <= kMagnifierRadius; ++dx) {
          const int px = std::clamp(coord->first + dx, 0, image.width - 1);
          const int py = std::clamp(coord->second + dy, 0, image.height - 1);
          const auto sample = samplePixel(image, px, py);
          const std::uint8_t r = sample.has_value() ? sample->r : 0;
          const std::uint8_t g = sample.has_value() ? sample->g : 0;
          const std::uint8_t b = sample.has_value() ? sample->b : 0;
          if (dx == 0 && dy == 0) {
            centerR = r;
            centerG = g;
            centerB = b;
          }
          const int dstX0 = (dx + kMagnifierRadius) * kMagnifierScale;
          const int dstY0 = (dy + kMagnifierRadius) * kMagnifierScale;
          for (int sy = 0; sy < kMagnifierScale; ++sy) {
            for (int sx = 0; sx < kMagnifierScale; ++sx) {
              const std::size_t index =
                  (static_cast<std::size_t>(dstY0 + sy) * static_cast<std::size_t>(kMagnifierDst)
                   + static_cast<std::size_t>(dstX0 + sx))
                  * 4U;
              out[index] = r;
              out[index + 1] = g;
              out[index + 2] = b;
              out[index + 3] = 255;
            }
          }
        }
      }
      const bool light = static_cast<int>(centerR) + static_cast<int>(centerG) + static_cast<int>(centerB) > 382;
      const std::uint8_t outline = light ? 0 : 255;
      const int origin = kMagnifierRadius * kMagnifierScale;
      const int last = origin + kMagnifierScale - 1;
      const auto setPx = [&out, outline](int x, int y) {
        const std::size_t index =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(kMagnifierDst) + static_cast<std::size_t>(x)) * 4U;
        out[index] = outline;
        out[index + 1] = outline;
        out[index + 2] = outline;
        out[index + 3] = 255;
      };
      for (int i = 0; i < kMagnifierScale; ++i) {
        setPx(origin + i, origin);
        setPx(origin + i, last);
        setPx(origin, origin + i);
        setPx(last, origin + i);
      }
    }

    [[nodiscard]] std::optional<std::string>
    hexAtLocal(const ScreencopyImage& image, int logicalWidth, int logicalHeight, double localX, double localY) {
      const auto sample = pixelAtLocal(image, logicalWidth, logicalHeight, localX, localY);
      if (!sample.has_value()) {
        return std::nullopt;
      }
      return sample->hex;
    }

    [[nodiscard]] wl_output* outputAtGlobalPoint(const WaylandConnection& wayland, double globalX, double globalY) {
      for (const auto& out : wayland.outputs()) {
        if (out.output == nullptr || out.logicalWidth <= 0 || out.logicalHeight <= 0) {
          continue;
        }
        if (globalX >= static_cast<double>(out.logicalX)
            && globalX < static_cast<double>(out.logicalX + out.logicalWidth)
            && globalY >= static_cast<double>(out.logicalY)
            && globalY < static_cast<double>(out.logicalY + out.logicalHeight)) {
          return out.output;
        }
      }
      return nullptr;
    }

    [[nodiscard]] std::string outputPickerLabel(const WaylandOutput& output) {
      if (!output.connectorName.empty()) {
        return output.connectorName;
      }
      if (!output.description.empty()) {
        return output.description;
      }
      return "Display";
    }

    std::unique_ptr<Flex>
    buildFullscreenPickerBar(const WaylandConnection& wayland, std::function<void(wl_output*)> onPick) {
      auto bar = ui::row(
          {
              .align = FlexAlign::Center,
              .justify = FlexJustify::Center,
              .gap = Style::spaceSm,
              .paddingV = Style::spaceSm,
              .paddingH = Style::spaceMd,
              .configure = [](Flex& control) { control.setCardStyle(1.0F, 0.94F, true); },
          },
          ui::label({
              .text = i18n::tr("bar.screenshot.choose-display"),
              .fontSize = Style::fontSizeCaption,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          })
      );

      for (const auto& out : wayland.outputs()) {
        if (out.output == nullptr || out.logicalWidth <= 0 || out.logicalHeight <= 0) {
          continue;
        }
        bar->addChild(
            ui::button({
                .text = outputPickerLabel(out),
                .variant = ButtonVariant::Outline,
                .onClick = [onPick, output = out.output]() { onPick(output); },
            })
        );
      }

      return bar;
    }

    std::unique_ptr<Flex> buildConfirmHintBar(Label*& hintOut) {
      return ui::row(
          {
              .align = FlexAlign::Center,
              .justify = FlexJustify::Center,
              .paddingV = Style::spaceSm,
              .paddingH = Style::spaceMd,
              .visible = false,
              .configure = [](Flex& control) { control.setCardStyle(1.0F, 0.94F, true); },
          },
          ui::label({
              .out = &hintOut,
              .fontSize = Style::fontSizeCaption,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          })
      );
    }

  } // namespace

  struct ScreenshotRegionOverlay::Instance {
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    InputArea* input = nullptr;
    Image* backdrop = nullptr;
    Box* dimTop = nullptr;
    Box* dimBottom = nullptr;
    Box* dimLeft = nullptr;
    Box* dimRight = nullptr;
    Box* selection = nullptr;
    Box* dimensionsBadge = nullptr;
    Label* dimensionsLabel = nullptr;
    Flex* colorPreview = nullptr;
    Image* colorMagnifier = nullptr;
    Box* colorSwatch = nullptr;
    Label* colorHexLabel = nullptr;
    Flex* confirmHint = nullptr;
    Label* confirmHintLabel = nullptr;
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    bool pointerInside = false;
  };

  ScreenshotRegionOverlay::ScreenshotRegionOverlay() = default;

  ScreenshotRegionOverlay::~ScreenshotRegionOverlay() = default;

  void ScreenshotRegionOverlay::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
    m_wayland = &wayland;
    m_renderContext = renderContext;
  }

  void ScreenshotRegionOverlay::setCompleteCallback(CompleteCallback callback) { m_onComplete = std::move(callback); }

  void ScreenshotRegionOverlay::setColorPickCallback(ColorPickCallback callback) { m_onColorPick = std::move(callback); }

  void ScreenshotRegionOverlay::setFailureCallback(FailureCallback callback) { m_onFailure = std::move(callback); }

  void ScreenshotRegionOverlay::setConfirmKeybindLabels(
      std::string copyLabel, std::string saveLabel, std::string cancelLabel
  ) {
    m_copyKeybindLabel = std::move(copyLabel);
    m_saveKeybindLabel = std::move(saveLabel);
    m_cancelKeybindLabel = std::move(cancelLabel);
  }

  void ScreenshotRegionOverlay::setFrozenScreenshots(std::vector<FrozenScreenshot> screenshots) {
    m_frozenScreenshots = std::move(screenshots);
  }

  std::vector<FrozenScreenshot> ScreenshotRegionOverlay::takeFrozenScreenshots() {
    std::vector<FrozenScreenshot> screenshots = std::move(m_frozenScreenshots);
    m_frozenScreenshots.clear();
    return screenshots;
  }

  void ScreenshotRegionOverlay::begin(
      bool freezeScreen, bool fullscreenPick, bool confirmRegion, std::optional<LogicalRect> initialRegion,
      bool colorPick
  ) {
    if (m_wayland == nullptr || m_renderContext == nullptr) {
      return;
    }
    destroySurfaces();
    m_abandonedRegion.reset();
    m_freezeScreen = freezeScreen;
    m_colorPick = colorPick;
    m_fullscreenPick = fullscreenPick && !colorPick;
    m_confirmRegion = confirmRegion && !fullscreenPick && !colorPick;
    m_confirming = false;
    m_active = true;
    m_dragging = false;
    if (!fullscreenPick && !colorPick && initialRegion.has_value() && initialRegion->width >= 2
        && initialRegion->height >= 2) {
      m_startGlobalX = static_cast<double>(initialRegion->x);
      m_startGlobalY = static_cast<double>(initialRegion->y);
      m_currentGlobalX = static_cast<double>(initialRegion->x + initialRegion->width);
      m_currentGlobalY = static_cast<double>(initialRegion->y + initialRegion->height);
      m_confirming = true;
    }
    ensureSurfaces();
    if (m_confirming) {
      updateSelectionVisuals();
    }
    for (auto& inst : m_instances) {
      if (inst->surface != nullptr) {
        inst->surface->requestLayout();
        inst->surface->requestRedraw();
      }
    }
  }

  void ScreenshotRegionOverlay::cancel() {
    m_active = false;
    m_dragging = false;
    m_confirming = false;
    m_freezeScreen = false;
    m_fullscreenPick = false;
    m_colorPick = false;
    m_confirmRegion = false;
    m_frozenScreenshots.clear();
    destroySurfaces();
  }

  void ScreenshotRegionOverlay::cancelSelection() {
    if (!m_active) {
      return;
    }
    DeferredCall::callLater([this]() {
      if (!m_active) {
        return;
      }
      if (m_colorPick) {
        cancel();
        if (m_onColorPick) {
          m_onColorPick(std::nullopt);
        }
        return;
      }
      m_abandonedRegion = selectionRectIfValid();
      cancel();
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
    });
  }

  std::optional<LogicalRect> ScreenshotRegionOverlay::takeAbandonedRegion() {
    return std::exchange(m_abandonedRegion, std::nullopt);
  }

    std::optional<LogicalRect> ScreenshotRegionOverlay::selectionRectIfValid() const {
    if (m_fullscreenPick || m_colorPick) {
      return std::nullopt;
    }
    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int width = globalX1 - globalX0;
    const int height = globalY1 - globalY0;
    if (width < 2 || height < 2) {
      return std::nullopt;
    }
    return LogicalRect{.x = globalX0, .y = globalY0, .width = width, .height = height};
  }

  void ScreenshotRegionOverlay::onOutputChange() {
    if (!m_active) {
      return;
    }
    if (!m_instances.empty() && !surfacesMatchOutputs()) {
      destroySurfaces();
      ensureSurfaces();
    }
  }

  bool ScreenshotRegionOverlay::surfacesMatchOutputs() const {
    if (m_wayland == nullptr) {
      return m_instances.empty();
    }
    const auto& outputs = m_wayland->outputs();
    if (m_instances.size() != outputs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (m_instances[i] == nullptr || m_instances[i]->output != outputs[i].output) {
        return false;
      }
    }
    return true;
  }

  void ScreenshotRegionOverlay::ensureSurfaces() {
    if (m_wayland == nullptr || m_renderContext == nullptr || !m_active) {
      return;
    }
    if (!m_instances.empty() && surfacesMatchOutputs()) {
      return;
    }
    destroySurfaces();

    for (const auto& output : m_wayland->outputs()) {
      if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
        continue;
      }

      auto inst = std::make_unique<Instance>();
      inst->output = output.output;

      auto config = LayerSurfaceConfig{
          .nameSpace = "noctalia-screenshot-region",
          .layer = LayerShellLayer::Overlay,
          .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
          .width = 0,
          .height = 0,
          .exclusiveZone = -1,
          .keyboard = overlayKeyboardMode(),
          .defaultWidth = static_cast<std::uint32_t>(output.logicalWidth),
          .defaultHeight = static_cast<std::uint32_t>(output.logicalHeight),
      };

      inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
      auto* instPtr = inst.get();
      inst->surface->setRenderContext(m_renderContext);
      inst->surface->setAnimationManager(&inst->animations);
      inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
        instPtr->surface->requestLayout();
      });
      inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
        prepareFrame(*instPtr, needsUpdate, needsLayout);
      });

      if (!inst->surface->initialize(output.output)) {
        kLog.warn("failed to initialize screenshot region overlay on {}", output.connectorName);
        continue;
      }

      m_instances.push_back(std::move(inst));
    }
  }

  void ScreenshotRegionOverlay::destroySurfaces() {
    for (auto& inst : m_instances) {
      if (inst != nullptr) {
        if (inst->backdrop != nullptr && inst->surface != nullptr && m_renderContext != nullptr) {
          Renderer& renderer = inst->surface->renderTarget().renderer();
          inst->backdrop->clear(renderer);
        }
        inst->inputDispatcher.setSceneRoot(nullptr);
        inst->animations.cancelAll();
      }
    }
    m_instances.clear();
    m_dragging = false;
  }

  void ScreenshotRegionOverlay::prepareFrame(Instance& inst, bool /*needsUpdate*/, bool /*needsLayout*/) {
    if (!m_active || m_renderContext == nullptr || inst.surface == nullptr) {
      return;
    }

    const auto width = inst.surface->width();
    const auto height = inst.surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    // The overlay's EGL surface could not be made current (e.g. EGL_BAD_ALLOC when the
    // driver is out of video memory). Painting would be a no-op, leaving an invisible
    // fullscreen surface that eats input, so tear down and report instead of spinning.
    if (!m_renderContext->makeCurrent(inst.surface->renderTarget())) {
      abortWithError(i18n::tr("bar.screenshot.overlay-alloc-failed"));
      return;
    }
    Renderer& renderer = inst.surface->renderTarget().renderer();

    const bool needsSceneBuild = inst.sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->height())) != height;
    if (!needsSceneBuild) {
      updateSelectionVisuals();
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);

    inst.sceneRoot = ui::node({
        .width = w,
        .height = h,
    });

    auto input = ui::inputArea({
        .acceptedButtons = InputArea::buttonMask(BTN_LEFT),
        .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR,
        .focusable = true,
    });

    if (m_colorPick) {
      input->setOnClick([this, surfaceOutput = inst.output](const InputArea::PointerData& data) {
        if (data.pressed || data.button != BTN_LEFT) {
          return;
        }
        const auto* surfaceOut = findOutput(*m_wayland, surfaceOutput);
        if (surfaceOut == nullptr) {
          return;
        }
        const double globalX = static_cast<double>(surfaceOut->logicalX) + static_cast<double>(data.localX);
        const double globalY = static_cast<double>(surfaceOut->logicalY) + static_cast<double>(data.localY);
        wl_output* picked = outputAtGlobalPoint(*m_wayland, globalX, globalY);
        if (picked == nullptr) {
          picked = surfaceOutput;
        }
        const auto* pickedOut = findOutput(*m_wayland, picked);
        const auto* frozen = frozenImageForOutput(m_frozenScreenshots, picked);
        if (pickedOut == nullptr || frozen == nullptr) {
          return;
        }
        const double localX = globalX - static_cast<double>(pickedOut->logicalX);
        const double localY = globalY - static_cast<double>(pickedOut->logicalY);
        const auto hex = hexAtLocal(*frozen, pickedOut->logicalWidth, pickedOut->logicalHeight, localX, localY);
        DeferredCall::callLater([this, hex]() { completeColorPick(hex); });
      });
      input->setOnMotion([this, instPtr = &inst](const InputArea::PointerData& data) {
        updateColorPickPreview(*instPtr, static_cast<double>(data.localX), static_cast<double>(data.localY));
      });
      input->setOnLeave([this]() { hideColorPickPreviews(); });
    } else if (m_fullscreenPick) {
      input->setOnClick([this, surfaceOutput = inst.output](const InputArea::PointerData& data) {
        if (data.pressed || data.button != BTN_LEFT) {
          return;
        }
        const auto* surfaceOut = findOutput(*m_wayland, surfaceOutput);
        if (surfaceOut == nullptr) {
          return;
        }
        const double globalX = static_cast<double>(surfaceOut->logicalX) + static_cast<double>(data.localX);
        const double globalY = static_cast<double>(surfaceOut->logicalY) + static_cast<double>(data.localY);
        wl_output* picked = outputAtGlobalPoint(*m_wayland, globalX, globalY);
        if (picked == nullptr) {
          picked = surfaceOutput;
        }
        DeferredCall::callLater([this, picked]() { completeFullscreenPick(picked); });
      });
    } else {
      input->setOnPress([this, output = inst.output](const InputArea::PointerData& data) {
        if (data.button != BTN_LEFT)
          return;

        if (!data.pressed) {
          if (!m_dragging)
            return;
          m_dragging = false;
          // completeSelection() tears down surfaces; defer past InputDispatcher::pointerButton.
          DeferredCall::callLater([this]() { completeSelection(); });
          return;
        }

        const auto* out = findOutput(*m_wayland, output);
        if (out == nullptr)
          return;

        const double globalX = static_cast<double>(out->logicalX) + static_cast<double>(data.localX);
        const double globalY = static_cast<double>(out->logicalY) + static_cast<double>(data.localY);

        m_dragging = true;

        if (m_confirming) {
          const double x0 = std::min(m_startGlobalX, m_currentGlobalX);
          const double y0 = std::min(m_startGlobalY, m_currentGlobalY);
          const double x1 = std::max(m_startGlobalX, m_currentGlobalX);
          const double y1 = std::max(m_startGlobalY, m_currentGlobalY);

          m_dragMode = hitTestSelection(globalX, globalY, x0, y0, x1, y1);

          if (m_dragMode == DragMode::None) {
            // Clicked outside the selection, start a new one
            m_confirming = false;
            m_dragMode = DragMode::NewSelection;
          }
        } else {
          m_dragMode = DragMode::NewSelection;
        }

        if (m_dragMode == DragMode::NewSelection) {
          m_startGlobalX = globalX;
          m_startGlobalY = globalY;
          m_currentGlobalX = globalX;
          m_currentGlobalY = globalY;

          m_cursorGlobalX = globalX;
          m_cursorGlobalY = globalY;
        } else if (m_dragMode == DragMode::Move) {
          m_moveOffsetX = globalX;
          m_moveOffsetY = globalY;
        } else {
          // Calculate anchors (the opposite side of what we are dragging)
          const double x0 = std::min(m_startGlobalX, m_currentGlobalX);
          const double y0 = std::min(m_startGlobalY, m_currentGlobalY);
          const double x1 = std::max(m_startGlobalX, m_currentGlobalX);
          const double y1 = std::max(m_startGlobalY, m_currentGlobalY);

          if (m_dragMode == DragMode::LeftEdge
              || m_dragMode == DragMode::TopLeftCorner
              || m_dragMode == DragMode::BottomLeftCorner) {
            m_startGlobalX = x1;
            m_currentGlobalX = globalX;
          } else if (
              m_dragMode == DragMode::RightEdge
              || m_dragMode == DragMode::TopRightCorner
              || m_dragMode == DragMode::BottomRightCorner
          ) {
            m_startGlobalX = x0;
            m_currentGlobalX = globalX;
          }

          if (m_dragMode == DragMode::TopEdge
              || m_dragMode == DragMode::TopLeftCorner
              || m_dragMode == DragMode::TopRightCorner) {
            m_startGlobalY = y1;
            m_currentGlobalY = globalY;
          } else if (
              m_dragMode == DragMode::BottomEdge
              || m_dragMode == DragMode::BottomLeftCorner
              || m_dragMode == DragMode::BottomRightCorner
          ) {
            m_startGlobalY = y0;
            m_currentGlobalY = globalY;
          }

          m_cursorGlobalX = globalX;
          m_cursorGlobalY = globalY;
        }

        updateSelectionVisuals();
        for (auto& instance : m_instances) {
          if (instance->surface != nullptr)
            instance->surface->requestRedraw();
        }
      });

      input->setOnMotion([this, output = inst.output, inputPtr = input.get()](const InputArea::PointerData& data) {
        const auto* out = findOutput(*m_wayland, output);
        if (out == nullptr)
          return;

        const double globalX = static_cast<double>(out->logicalX) + static_cast<double>(data.localX);
        const double globalY = static_cast<double>(out->logicalY) + static_cast<double>(data.localY);

        if (!m_dragging) {
          if (m_confirming) {
            const double x0 = std::min(m_startGlobalX, m_currentGlobalX);
            const double y0 = std::min(m_startGlobalY, m_currentGlobalY);
            const double x1 = std::max(m_startGlobalX, m_currentGlobalX);
            const double y1 = std::max(m_startGlobalY, m_currentGlobalY);

            DragMode hoverMode = hitTestSelection(globalX, globalY, x0, y0, x1, y1);
            inputPtr->setCursorShape(cursorShapeForDragMode(hoverMode));
          } else {
            inputPtr->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
          }
          return;
        }

        if (m_dragMode == DragMode::Move) {
          const double deltaX = globalX - m_moveOffsetX;
          const double deltaY = globalY - m_moveOffsetY;

          m_startGlobalX += deltaX;
          m_currentGlobalX += deltaX;
          m_startGlobalY += deltaY;
          m_currentGlobalY += deltaY;

          m_moveOffsetX = globalX;
          m_moveOffsetY = globalY;
        } else {
          if (m_dragMode != DragMode::TopEdge && m_dragMode != DragMode::BottomEdge) {
            m_currentGlobalX = globalX;
          }
          if (m_dragMode != DragMode::LeftEdge && m_dragMode != DragMode::RightEdge) {
            m_currentGlobalY = globalY;
          }

          m_cursorGlobalX = globalX;
          m_cursorGlobalY = globalY;
        }

        updateSelectionVisuals();
        for (auto& instance : m_instances) {
          if (instance->surface != nullptr)
            instance->surface->requestRedraw();
        }
      });
    }

    input->setOnKeyDown([this](const InputArea::KeyData& key) {
      if (!key.pressed) {
        return;
      }
      if (m_confirming) {
        if (KeybindMatcher::matches(KeybindAction::Copy, key.sym, key.modifiers)) {
          DeferredCall::callLater([this]() { confirmPendingSelection(ConfirmAction::ForceClipboard); });
          return;
        }
        if (KeybindMatcher::matches(KeybindAction::Save, key.sym, key.modifiers)) {
          DeferredCall::callLater([this]() { confirmPendingSelection(ConfirmAction::ForceSave); });
          return;
        }
        if (KeySymbol::isEnterOrSpace(key.sym)) {
          DeferredCall::callLater([this]() { confirmPendingSelection(ConfirmAction::None); });
          return;
        }
      }
      if (KeybindMatcher::matches(KeybindAction::Cancel, key.sym, key.modifiers)) {
        cancelSelection();
      }
    });

    const auto* frozen = m_freezeScreen ? frozenImageForOutput(m_frozenScreenshots, inst.output) : nullptr;
    if (frozen != nullptr) {
      auto backdrop = ui::image({
          .fit = ImageFit::Stretch,
          .width = w,
          .height = h,
          .configure = [](Image& image) { image.setPosition(0.0F, 0.0F); },
      });
      if (!backdrop->setSourceRaw(
              renderer, frozen->rgba.data(), frozen->rgba.size(), frozen->width, frozen->height, frozen->width * 4,
              PixmapFormat::RGBA, false
          )) {
        kLog.warn("failed to upload frozen screenshot backdrop");
      }
      inst.backdrop = static_cast<Image*>(input->addChild(std::move(backdrop)));
    }

    if (!m_colorPick) {
      auto makeDimStrip = [&]() {
        auto strip = ui::box({
            // Fixed black scrim so it darkens under every theme.
            .fill = fixedColorSpec(rgba(0.0F, 0.0F, 0.0F, 1.0F)),
            .width = 0.0F,
            .height = 0.0F,
            .opacity = kDimOpacity,
            .configure = [](Box& box) { box.setPosition(0.0F, 0.0F); },
        });
        return static_cast<Box*>(input->addChild(std::move(strip)));
      };
      inst.dimTop = makeDimStrip();
      inst.dimBottom = makeDimStrip();
      inst.dimLeft = makeDimStrip();
      inst.dimRight = makeDimStrip();
    }

    Color border = colorForRole(ColorRole::Primary);
    border.a = 1.0F;

    auto selection = ui::box({
        .visible = false,
        .configure = [border](Box& box) { box.setBorder(fixedColorSpec(border), kSelectionBorderWidth); },
    });

    Color badgeFill = colorForRole(ColorRole::Surface);
    badgeFill.a = 0.94F;
    auto dimensionsBadge = ui::box({
        .fill = fixedColorSpec(badgeFill),
        .radius = Style::radiusSm,
        .visible = false,
        .configure = [border](Box& box) { box.setBorder(fixedColorSpec(border), 1.0F); },
    });

    auto dimensionsLabel = ui::label({
        .fontSize = kDimensionFontSize,
        .fontWeight = FontWeight::Bold,
        .color = fixedColorSpec(border),
    });

    if (!m_fullscreenPick && !m_colorPick) {
      inst.dimensionsLabel = static_cast<Label*>(dimensionsBadge->addChild(std::move(dimensionsLabel)));
      inst.selection = static_cast<Box*>(input->addChild(std::move(selection)));
      inst.dimensionsBadge = static_cast<Box*>(input->addChild(std::move(dimensionsBadge)));
    }
    inst.input = input.get();
    inst.sceneRoot->addChild(std::move(input));

    if (m_fullscreenPick) {
      auto pickerBar = buildFullscreenPickerBar(*m_wayland, [this](wl_output* output) {
        DeferredCall::callLater([this, output]() { completeFullscreenPick(output); });
      });
      Flex* pickerBarPtr = pickerBar.get();
      inst.sceneRoot->addChild(std::move(pickerBar));
      pickerBarPtr->layout(renderer);
      pickerBarPtr->setPosition((w - pickerBarPtr->width()) * 0.5F, Style::spaceMd);
    } else if (m_colorPick) {
      auto hintBar = buildConfirmHintBar(inst.confirmHintLabel);
      if (inst.confirmHintLabel != nullptr) {
        inst.confirmHintLabel->setText(
            i18n::tr("bar.screenshot.pick-color-hint", "cancel", m_cancelKeybindLabel)
        );
      }
      Flex* hintPtr = hintBar.get();
      inst.confirmHint = hintBar.get();
      inst.sceneRoot->addChild(std::move(hintBar));
      hintPtr->layout(renderer);
      hintPtr->setPosition((w - hintPtr->width()) * 0.5F, Style::spaceMd);

      auto preview = ui::column({
          .out = &inst.colorPreview,
          .align = FlexAlign::Center,
          .gap = Style::spaceXs,
          .paddingV = kDimensionPaddingY,
          .paddingH = kDimensionPaddingX,
          .fill = fixedColorSpec(badgeFill),
          .radius = Style::radiusSm,
          .border = colorSpecFromRole(ColorRole::Outline),
          .borderWidth = 1.0F,
          .visible = false,
          .configure = [](Flex& row) { row.setHitTestVisible(false); },
      });
      preview->addChild(
          ui::image({
              .out = &inst.colorMagnifier,
              .fit = ImageFit::Stretch,
              .width = static_cast<float>(kMagnifierDst),
              .height = static_cast<float>(kMagnifierDst),
              .configure = [](Image& image) { image.setHitTestVisible(false); },
          })
      );
      auto hexRow = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceXs,
      });
      hexRow->addChild(
          ui::box({
              .out = &inst.colorSwatch,
              .fill = fixedColorSpec(rgba(0.0F, 0.0F, 0.0F)),
              .border = colorSpecFromRole(ColorRole::Outline),
              .borderWidth = 1.0F,
              .radius = Style::radiusSm,
              .width = kColorSwatchSize,
              .height = kColorSwatchSize,
          })
      );
      hexRow->addChild(
          ui::label({
              .out = &inst.colorHexLabel,
              .fontSize = kDimensionFontSize,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          })
      );
      preview->addChild(std::move(hexRow));
      inst.sceneRoot->addChild(std::move(preview));
    } else if (m_confirmRegion) {
      auto hintBar = buildConfirmHintBar(inst.confirmHintLabel);
      inst.confirmHint = hintBar.get();
      inst.sceneRoot->addChild(std::move(hintBar));
    }

    inst.surface->setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      if (m_wayland != nullptr) {
        m_wayland->setCursorShape(serial, shape);
      }
    });
    if (inst.input != nullptr) {
      inst.inputDispatcher.setFocus(inst.input);
    }

    updateSelectionVisuals();
  }

  bool ScreenshotRegionOverlay::onPointerEvent(const PointerEvent& event) {
    if (!m_active) {
      return false;
    }

    Instance* target = nullptr;
    if (event.surface != nullptr) {
      for (auto& inst : m_instances) {
        if (inst != nullptr && inst->surface != nullptr && inst->surface->wlSurface() == event.surface) {
          target = inst.get();
          break;
        }
      }
    }

    if (target == nullptr) {
      for (auto& inst : m_instances) {
        if (inst != nullptr && inst->pointerInside) {
          target = inst.get();
          break;
        }
      }
    }

    if (target == nullptr) {
      return false;
    }

    const bool onTarget =
        event.surface != nullptr && target->surface != nullptr && event.surface == target->surface->wlSurface();

    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (onTarget) {
        target->pointerInside = true;
        target->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      }
      return onTarget;
    case PointerEvent::Type::Leave:
      if (onTarget || target->pointerInside) {
        target->pointerInside = false;
        target->inputDispatcher.pointerLeave();
      }
      return onTarget || target->pointerInside;
    case PointerEvent::Type::Motion:
      if (onTarget) {
        target->pointerInside = true;
      }
      if (onTarget || target->pointerInside) {
        target->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
        return true;
      }
      return false;
    case PointerEvent::Type::Button: {
      if (onTarget) {
        target->pointerInside = true;
      }
      if (!onTarget && !target->pointerInside) {
        return false;
      }
      const bool pressed = event.pressed;
      return target->inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed, event.serial, event.time,
          event.touch
      );
    }
    case PointerEvent::Type::Axis:
      if (onTarget || target->pointerInside) {
        return target->inputDispatcher.pointerAxis(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
            event.axisDiscrete, event.axisValue120, event.axisLines
        );
      }
      return false;
    }

    return false;
  }

  bool ScreenshotRegionOverlay::onKeyboardEvent(const KeyboardEvent& event) {
    if (!m_active || !event.pressed || m_wayland == nullptr) {
      return false;
    }

    wl_surface* const kbSurface = m_wayland->lastKeyboardSurface();
    bool onOverlay = false;
    for (const auto& inst : m_instances) {
      if (inst != nullptr && inst->surface != nullptr && inst->surface->wlSurface() == kbSurface) {
        onOverlay = true;
        break;
      }
    }
    if (!onOverlay) {
      return false;
    }

    if (!KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      if (m_confirming) {
        if (KeybindMatcher::matches(KeybindAction::Copy, event.sym, event.modifiers)) {
          confirmPendingSelection(ConfirmAction::ForceClipboard);
          return true;
        }
        if (KeybindMatcher::matches(KeybindAction::Save, event.sym, event.modifiers)) {
          confirmPendingSelection(ConfirmAction::ForceSave);
          return true;
        }
        if (KeySymbol::isEnterOrSpace(event.sym)) {
          confirmPendingSelection(ConfirmAction::None);
          return true;
        }
      }
      return false;
    }
    cancelSelection();
    return true;
  }

  void ScreenshotRegionOverlay::hideColorPickPreviews() {
    for (auto& inst : m_instances) {
      if (inst == nullptr || inst->colorPreview == nullptr) {
        continue;
      }
      inst->colorPreview->setVisible(false);
      if (inst->surface != nullptr) {
        inst->surface->requestRedraw();
      }
    }
  }

  void ScreenshotRegionOverlay::updateColorPickPreview(Instance& instance, double localX, double localY) {
    if (!m_colorPick || m_wayland == nullptr || instance.colorPreview == nullptr || instance.surface == nullptr) {
      return;
    }
    const auto* surfaceOut = findOutput(*m_wayland, instance.output);
    if (surfaceOut == nullptr) {
      hideColorPickPreviews();
      return;
    }
    const double globalX = static_cast<double>(surfaceOut->logicalX) + localX;
    const double globalY = static_cast<double>(surfaceOut->logicalY) + localY;
    wl_output* picked = outputAtGlobalPoint(*m_wayland, globalX, globalY);
    if (picked == nullptr) {
      picked = instance.output;
    }
    const auto* pickedOut = findOutput(*m_wayland, picked);
    const auto* frozen = frozenImageForOutput(m_frozenScreenshots, picked);
    if (pickedOut == nullptr || frozen == nullptr) {
      instance.colorPreview->setVisible(false);
      instance.surface->requestRedraw();
      return;
    }
    const double sampleX = globalX - static_cast<double>(pickedOut->logicalX);
    const double sampleY = globalY - static_cast<double>(pickedOut->logicalY);
    const auto sample = pixelAtLocal(*frozen, pickedOut->logicalWidth, pickedOut->logicalHeight, sampleX, sampleY);
    if (!sample.has_value()) {
      instance.colorPreview->setVisible(false);
      instance.surface->requestRedraw();
      return;
    }
    if (instance.colorSwatch != nullptr) {
      instance.colorSwatch->setFill(sample->color);
    }
    if (instance.colorHexLabel != nullptr) {
      instance.colorHexLabel->setText(sample->hex);
    }
    Renderer& renderer = instance.surface->renderTarget().renderer();
    if (instance.colorMagnifier != nullptr) {
      fillMagnifier(
          *frozen, pickedOut->logicalWidth, pickedOut->logicalHeight, sampleX, sampleY, m_magnifierRgba
      );
      (void)instance.colorMagnifier->setSourceRaw(
          renderer, m_magnifierRgba.data(), m_magnifierRgba.size(), kMagnifierDst, kMagnifierDst, kMagnifierDst * 4,
          PixmapFormat::RGBA, false
      );
    }
    instance.colorPreview->layout(renderer);
    const auto surfaceW = static_cast<float>(instance.surface->width());
    const auto surfaceH = static_cast<float>(instance.surface->height());
    const float badgeW = instance.colorPreview->width();
    const float badgeH = instance.colorPreview->height();
    float badgeX = static_cast<float>(localX) + kDimensionCursorOffsetX;
    float badgeY = static_cast<float>(localY) + kDimensionCursorOffsetY;
    badgeX = std::clamp(badgeX, 0.0F, std::max(0.0F, surfaceW - badgeW));
    badgeY = std::clamp(badgeY, 0.0F, std::max(0.0F, surfaceH - badgeH));
    instance.colorPreview->setPosition(badgeX, badgeY);
    instance.colorPreview->setVisible(true);
    for (auto& other : m_instances) {
      if (other != nullptr && other.get() != &instance && other->colorPreview != nullptr) {
        other->colorPreview->setVisible(false);
        if (other->surface != nullptr) {
          other->surface->requestRedraw();
        }
      }
    }
    instance.surface->requestRedraw();
  }

  void ScreenshotRegionOverlay::abortWithError(const std::string& message) {
    if (!m_active) {
      return;
    }
    // Stop further frames from re-triggering the abort while teardown is pending.
    m_active = false;
    kLog.warn("aborting screenshot region overlay: {}", message);
    FailureCallback onFailure = m_onFailure;
    // Defer past the surface's prepareFrame callback before destroying its surfaces.
    DeferredCall::callLater([this, onFailure, message]() {
      cancel();
      if (onFailure) {
        onFailure(message);
      }
    });
  }

  void ScreenshotRegionOverlay::updateSelectionVisuals() {
    if (m_colorPick) {
      return;
    }
    // Lay out the four dim strips so they cover the surface except for the hole
    // rect (surface-local). An empty hole dims the whole surface.
    const auto layoutDimFrame = [](Instance& inst, float surfaceW, float surfaceH, float hx0, float hy0, float hx1,
                                   float hy1) {
      hx0 = std::clamp(hx0, 0.0F, surfaceW);
      hx1 = std::clamp(hx1, 0.0F, surfaceW);
      hy0 = std::clamp(hy0, 0.0F, surfaceH);
      hy1 = std::clamp(hy1, 0.0F, surfaceH);
      if (hx1 < hx0 || hy1 < hy0) {
        hx0 = hy0 = hx1 = hy1 = 0.0F;
      }
      if (inst.dimTop != nullptr) {
        inst.dimTop->setPosition(0.0F, 0.0F);
        inst.dimTop->setSize(surfaceW, hy0);
      }
      if (inst.dimBottom != nullptr) {
        inst.dimBottom->setPosition(0.0F, hy1);
        inst.dimBottom->setSize(surfaceW, surfaceH - hy1);
      }
      if (inst.dimLeft != nullptr) {
        inst.dimLeft->setPosition(0.0F, hy0);
        inst.dimLeft->setSize(hx0, hy1 - hy0);
      }
      if (inst.dimRight != nullptr) {
        inst.dimRight->setPosition(hx1, hy0);
        inst.dimRight->setSize(surfaceW - hx1, hy1 - hy0);
      }
    };

    if (!m_dragging && !m_confirming) {
      for (auto& inst : m_instances) {
        if (inst->surface != nullptr) {
          layoutDimFrame(
              *inst, static_cast<float>(inst->surface->width()), static_cast<float>(inst->surface->height()), 0.0F,
              0.0F, 0.0F, 0.0F
          );
        }
        if (inst->selection != nullptr) {
          inst->selection->setVisible(false);
        }
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
        if (inst->confirmHint != nullptr) {
          inst->confirmHint->setVisible(false);
        }
      }
      return;
    }

    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int selectionWidth = globalX1 - globalX0;
    const int selectionHeight = globalY1 - globalY0;

    char dimensionText[32];
    std::snprintf(dimensionText, sizeof(dimensionText), "%dx%d", selectionWidth, selectionHeight);

    for (auto& inst : m_instances) {
      if (inst->selection == nullptr || inst->surface == nullptr) {
        continue;
      }
      const auto surfaceW = static_cast<float>(inst->surface->width());
      const auto surfaceH = static_cast<float>(inst->surface->height());
      const auto* out = findOutput(*m_wayland, inst->output);
      if (out == nullptr) {
        layoutDimFrame(*inst, surfaceW, surfaceH, 0.0F, 0.0F, 0.0F, 0.0F);
        inst->selection->setVisible(false);
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
        continue;
      }

      const int outLeft = out->logicalX;
      const int outTop = out->logicalY;
      const int outRight = out->logicalX + out->logicalWidth;
      const int outBottom = out->logicalY + out->logicalHeight;

      const int ix0 = std::max(globalX0, outLeft);
      const int iy0 = std::max(globalY0, outTop);
      const int ix1 = std::min(globalX1, outRight);
      const int iy1 = std::min(globalY1, outBottom);
      if (ix1 <= ix0 || iy1 <= iy0) {
        layoutDimFrame(*inst, surfaceW, surfaceH, 0.0F, 0.0F, 0.0F, 0.0F);
        inst->selection->setVisible(false);
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
        continue;
      }

      const auto holeX0 = static_cast<float>(ix0 - outLeft);
      const auto holeY0 = static_cast<float>(iy0 - outTop);
      const auto holeX1 = static_cast<float>(ix1 - outLeft);
      const auto holeY1 = static_cast<float>(iy1 - outTop);
      layoutDimFrame(*inst, surfaceW, surfaceH, holeX0, holeY0, holeX1, holeY1);

      // The outline is inset, so expand it outward to keep the border out of the
      // captured (undimmed) region.
      inst->selection->setVisible(true);
      inst->selection->setPosition(holeX0 - kSelectionBorderWidth, holeY0 - kSelectionBorderWidth);
      inst->selection->setSize(
          (holeX1 - holeX0) + (kSelectionBorderWidth * 2.0F), (holeY1 - holeY0) + (kSelectionBorderWidth * 2.0F)
      );

      if (inst->dimensionsBadge != nullptr && inst->dimensionsLabel != nullptr && m_renderContext != nullptr) {
        const bool showBadge = m_dragging && m_dragMode != DragMode::Move;
        const double targetX = m_cursorGlobalX;
        const double targetY = m_cursorGlobalY;
        const bool badgeOnOutput = targetX >= outLeft && targetX < outRight && targetY >= outTop && targetY < outBottom;

        if (showBadge && badgeOnOutput) {
          inst->dimensionsLabel->setText(dimensionText);
          inst->dimensionsLabel->measure(inst->surface->renderTarget().renderer());

          const float badgeWidth = inst->dimensionsLabel->width() + (kDimensionPaddingX * 2.0F);
          const float badgeHeight = inst->dimensionsLabel->height() + (kDimensionPaddingY * 2.0F);
          inst->dimensionsBadge->setSize(badgeWidth, badgeHeight);

          const double selX0 = std::min(m_startGlobalX, m_currentGlobalX);
          const double selX1 = std::max(m_startGlobalX, m_currentGlobalX);
          const double selY0 = std::min(m_startGlobalY, m_currentGlobalY);
          const double selY1 = std::max(m_startGlobalY, m_currentGlobalY);

          const double badgeCursorX = std::clamp(targetX, selX0, selX1);
          const double badgeCursorY = std::clamp(targetY, selY0, selY1);

          float badgeX = static_cast<float>(badgeCursorX - outLeft) + kDimensionCursorOffsetX;
          float badgeY = static_cast<float>(badgeCursorY - outTop) + kDimensionCursorOffsetY;

          const float maxX = std::max(0.0F, surfaceW - badgeWidth);
          const float maxY = std::max(0.0F, surfaceH - badgeHeight);

          badgeX = std::clamp(badgeX, 0.0F, maxX);
          badgeY = std::clamp(badgeY, 0.0F, maxY);

          inst->dimensionsBadge->setPosition(badgeX, badgeY);
          inst->dimensionsLabel->setPosition(kDimensionPaddingX, kDimensionPaddingY);
          inst->dimensionsBadge->setVisible(true);
        } else {
          inst->dimensionsBadge->setVisible(false);
        }
      }
    }

    if (m_renderContext != nullptr) {
      for (auto& inst : m_instances) {
        if (inst->confirmHint == nullptr || inst->surface == nullptr) {
          continue;
        }
        inst->confirmHint->setVisible(m_confirming);
        if (!m_confirming) {
          continue;
        }
        if (inst->confirmHintLabel != nullptr) {
          inst->confirmHintLabel->setText(
              i18n::tr(
                  "bar.screenshot.confirm-region", "copy", m_copyKeybindLabel, "save", m_saveKeybindLabel, "cancel",
                  m_cancelKeybindLabel
              )
          );
        }
        const auto surfaceW = static_cast<float>(inst->surface->width());
        const auto surfaceH = static_cast<float>(inst->surface->height());
        Renderer& renderer = inst->surface->renderTarget().renderer();
        inst->confirmHint->layout(renderer);
        const float y = std::max(Style::spaceMd, surfaceH - inst->confirmHint->height() - Style::spaceMd);
        inst->confirmHint->setPosition((surfaceW - inst->confirmHint->width()) * 0.5F, y);
      }
    }
  }

  void ScreenshotRegionOverlay::completeSelection() {
    m_dragging = false;
    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int width = globalX1 - globalX0;
    const int height = globalY1 - globalY0;

    if (width < 2 || height < 2) {
      m_active = false;
      destroySurfaces();
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
      return;
    }

    if (m_confirmRegion) {
      m_confirming = true;
      m_startGlobalX = static_cast<double>(globalX0);
      m_startGlobalY = static_cast<double>(globalY0);
      m_currentGlobalX = static_cast<double>(globalX1);
      m_currentGlobalY = static_cast<double>(globalY1);
      updateSelectionVisuals();
      for (auto& instance : m_instances) {
        if (instance->surface != nullptr) {
          instance->surface->requestRedraw();
        }
      }
      return;
    }

    m_active = false;
    destroySurfaces();

    LogicalRect region{
        .x = globalX0,
        .y = globalY0,
        .width = width,
        .height = height,
    };
    if (m_onComplete) {
      m_onComplete(region, nullptr, ConfirmAction::None);
    }
  }

  void ScreenshotRegionOverlay::confirmPendingSelection(ConfirmAction action) {
    if (!m_active || !m_confirming) {
      return;
    }

    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int width = globalX1 - globalX0;
    const int height = globalY1 - globalY0;

    m_confirming = false;
    m_active = false;
    destroySurfaces();

    if (width < 2 || height < 2) {
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
      return;
    }

    LogicalRect region{
        .x = globalX0,
        .y = globalY0,
        .width = width,
        .height = height,
    };
    if (m_onComplete) {
      m_onComplete(region, nullptr, action);
    }
  }

  void ScreenshotRegionOverlay::completeFullscreenPick(wl_output* output) {
    if (!m_active || output == nullptr || m_wayland == nullptr) {
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
      return;
    }

    const auto* out = findOutput(*m_wayland, output);
    if (out == nullptr) {
      m_active = false;
      destroySurfaces();
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr, ConfirmAction::None);
      }
      return;
    }

    m_active = false;
    destroySurfaces();

    LogicalRect region{
        .x = 0,
        .y = 0,
        .width = out->logicalWidth,
        .height = out->logicalHeight,
    };
    if (m_onComplete) {
      m_onComplete(region, output, ConfirmAction::None);
    }
  }

  void ScreenshotRegionOverlay::completeColorPick(std::optional<std::string> hex) {
    if (!m_active) {
      if (m_onColorPick) {
        m_onColorPick(std::nullopt);
      }
      return;
    }
    m_active = false;
    destroySurfaces();
    if (m_onColorPick) {
      m_onColorPick(std::move(hex));
    }
  }

} // namespace capture
