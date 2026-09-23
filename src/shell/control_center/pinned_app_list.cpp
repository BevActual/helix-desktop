#include "shell/control_center/pinned_app_list.h"

#include "shell/control_center/tab.h"
#include "shell/dock/pinned_apps.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/list_editor.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <memory>

namespace control_center {

  namespace {

    constexpr float kIconSize = 24.0F;

    [[nodiscard]] IconResolver& iconResolver() {
      static IconResolver resolver;
      return resolver;
    }

  } // namespace

  void PinnedAppList::attach(ListEditor& editor, float scale) {
    editor.setPresentation(ListEditorPresentation::Grouped);
    editor.setGap(Style::spaceXs * scale);
    editor.setRowConfigure([scale](Flex& row) {
      applyGroupedListRowStyle(row, scale, GroupedListRowEmphasis::None);
    });
    editor.setItemLeading([this, scale](std::size_t index) -> std::unique_ptr<Node> {
      if (index == 0) {
        m_icons.clear();
      }
      IconSlot slot;
      const float size = kIconSize * scale;
      auto host = ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::Center, .width = size, .height = size},
          ui::image({
              .out = &slot.image,
              .fit = ImageFit::Contain,
              .width = size,
              .height = size,
              .visible = false,
              .participatesInLayout = false,
          }),
          ui::glyph({
              .out = &slot.fallback,
              .glyph = "app-window",
              .glyphSize = size * 0.6F,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      m_icons.push_back(slot);
      return host;
    });
  }

  void PinnedAppList::load(Renderer& renderer, const std::vector<std::string>& pinned, float scale) {
    if (m_icons.empty() || pinned.size() != m_icons.size()) {
      return;
    }
    std::vector<DesktopEntry> entries;
    const int targetPx = static_cast<int>(std::round(kIconSize * scale));
    for (std::size_t i = 0; i < m_icons.size(); ++i) {
      IconSlot& slot = m_icons[i];
      if (slot.loaded || slot.image == nullptr) {
        continue;
      }
      if (entries.empty()) {
        entries = shell::dock::pinned_apps::resolveEntries(pinned);
      }
      slot.loaded = true;
      if (i >= entries.size() || entries[i].icon.empty()) {
        continue;
      }
      const std::string& path = iconResolver().resolve(entries[i].icon, targetPx);
      const bool ok = !path.empty() && slot.image->setSourceFile(renderer, path, targetPx, true);
      slot.image->setVisible(ok);
      slot.image->setParticipatesInLayout(ok);
      if (slot.fallback != nullptr) {
        slot.fallback->setVisible(!ok);
        slot.fallback->setParticipatesInLayout(!ok);
      }
    }
  }

} // namespace control_center
