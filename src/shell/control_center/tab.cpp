#include "shell/control_center/tab.h"

#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

namespace control_center {

  float sectionCardFillOpacity(float panelCardOpacity) noexcept {
    return panelCardOpacity >= 0.99F ? 1.0F : std::min(1.0F, panelCardOpacity + 0.28F);
  }

  void applySectionCardStyle(Flex& card, float scale, float fillOpacity) {
    // Tab bodies sit as solid wells on the glass panel chrome. Lift the fill so
    // Glass/Soft still read as content, while Solid stays fully opaque.
    card.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, sectionCardFillOpacity(fillOpacity)));
    if (Style::cardBordersEnabled()) {
      card.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
    } else {
      card.clearBorder();
    }
    card.setRadius(Style::scaledRadius2xl(scale));
    card.setDirection(FlexDirection::Vertical);
    card.setAlign(FlexAlign::Stretch);
    card.setGap(Style::spaceSm * scale);
    card.setPadding(Style::spaceMd * scale);
  }

  Label* addTitle(Flex& parent, const std::string& text, float scale) {
    Label* ptr = nullptr;
    auto label = ui::label({
        .out = &ptr,
        .text = text,
        .fontSize = Style::fontSizeTitle * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    });
    parent.addChild(std::move(label));
    return ptr;
  }

  void addBody(Flex& parent, const std::string& text, float scale) {
    parent.addChild(
        ui::label({
            .text = text,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  std::unique_ptr<Flex> makeCardHeaderRow(const std::string& title, float scale) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = Style::controlHeightSm * scale},
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
  }

} // namespace control_center

std::unique_ptr<Flex> Tab::createHeaderActions() { return nullptr; }
