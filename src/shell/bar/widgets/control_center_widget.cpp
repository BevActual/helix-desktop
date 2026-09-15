#include "shell/bar/widgets/control_center_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/package_kit_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

namespace {
  constexpr float kDotBaseSize = 6.0F;
}

ControlCenterWidget::ControlCenterWidget(wl_output* /*output*/, Options options)
    : m_barGlyphId(std::move(options.glyph)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)) {}

void ControlCenterWidget::create() {
  auto area = ui::inputArea({});

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  const float dotSize = kDotBaseSize * m_contentScale;
  m_dot = area->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = dotSize * 0.5F,
          .width = dotSize,
          .height = dotSize,
          .visible = false,
      })
  );

  setRoot(std::move(area));
  refreshUpdateDot();
}

void ControlCenterWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr) {
    return;
  }

  if (m_image != nullptr) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
    node->setSize(m_image->width(), m_image->height());
  } else if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
    node->setSize(m_glyph->width(), m_glyph->height());
  }

  if (m_dot != nullptr) {
    const float dotSize = kDotBaseSize * m_contentScale;
    m_dot->setPosition(std::max(0.0F, node->width() - dotSize), 0.0F);
  }
}

void ControlCenterWidget::doUpdate(Renderer& /*renderer*/) { refreshUpdateDot(); }

void ControlCenterWidget::refreshUpdateDot() {
  const bool hasUpdates = noctalia::packages::pendingUpdateCount() > 0;
  if (hasUpdates == m_hasUpdates) {
    return;
  }
  m_hasUpdates = hasUpdates;
  if (m_dot != nullptr) {
    m_dot->setVisible(hasUpdates);
  }
}
