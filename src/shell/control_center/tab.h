#pragma once

#include "core/ui_phase.h"
#include "ui/controls/separator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Flex;
class Label;
class Renderer;
class Select;
class Slider;
class Toggle;

namespace control_center {

  // Helix maps Apple HIG Liquid Glass to a hierarchy, not a skin:
  // chrome = panel / sidebar / header (glass), leftover = tab content (solid wells),
  // modules = functional plates (Power session, optional Home pins), widgets = glanceable Home/desktop cards that deep-link.
  // Do not nest glass-on-glass. Compact cards and leftover catalogs stay leftover.
  void applySectionCardStyle(Flex& card, float scale, float fillOpacity);
  // Compact source well. Hosted Theme children skip the well so Theme keeps one card.
  void applyCompactCardStyle(Flex& card, float scale, float fillOpacity, bool hosted);
  [[nodiscard]] float sectionCardFillOpacity(float panelCardOpacity) noexcept;
  void applyGroupedListStyle(Flex& host, float scale);
  // Glanceable widgets and Apple-app content on the glass panel chrome.
  void applyContentCardStyle(Flex& card, float scale);
  // Control Center functional modules (frosted plates). Not a second glass layer.
  void applyModuleWellStyle(Flex& well, float scale);
  // How many leftover catalog tiles fit in `availableWidth`. Used when Control Center
  // is sized like a Settings window.
  [[nodiscard]] std::size_t leftoverCatalogColumns(
      float availableWidth, float minCellWidth, float gap, std::size_t minColumns = 2,
      std::size_t maxColumns = 5
  ) noexcept;
  enum class GroupedListRowEmphasis : std::uint8_t { None, Hover, Pressed, Selected };
  void applyGroupedListRowStyle(Flex& row, float scale, GroupedListRowEmphasis emphasis);
  Separator* addWellDivider(
      Flex& parent, float scale, SeparatorOrientation orientation = SeparatorOrientation::HorizontalRule
  );
  // Well header label (body size, bold). Do not repeat the tab title on the compact card.
  Label* addTitle(Flex& parent, const std::string& text, float scale = 1.0F);
  void addBody(Flex& parent, const std::string& text, float scale = 1.0F);
  Label* addCaption(Flex& parent, const std::string& text, float scale = 1.0F, int maxLines = 2);

  // Card header row: a card-title-styled label (flexGrow) ready for trailing
  // controls appended by the caller. Used for titled group cards.
  std::unique_ptr<Flex> makeCardHeaderRow(const std::string& title, float scale = 1.0F);

  // Settings live row: title left, room for a trailing control on the right.
  // wrap lets a wide control (segmented toolbar, picker) fall under the title;
  // toggle, select, and slider-value rows keep wrap off so the control stays trailing.
  std::unique_ptr<Flex> makeControlRow(
      const std::string& title, float scale = 1.0F, Label** titleOut = nullptr, bool wrap = true
  );
  Toggle* addToggleRow(
      Flex& parent, float scale, const std::string& title, std::function<void(bool)> onChange
  );
  Select* addSelectRow(
      Flex& parent, float scale, const std::string& title, std::vector<std::string> options, float surfaceOpacity,
      std::function<void(std::size_t)> onChange
  );
  void addSliderSection(
      Flex& parent, float scale, const std::string& title, Slider** sliderOut, Label** valueOut, double minValue,
      double maxValue, double step, double value, std::function<std::string(double)> format,
      std::function<void()> onCommit
  );
  // Size a hosted live body to its content height so leftover wells can scroll
  // instead of stretching sliders and clipping trailing rows.
  void layoutHostedLiveBody(Flex* root, Renderer& renderer, float width);

} // namespace control_center

class Tab {
public:
  virtual ~Tab() = default;

  // Creates and returns the tab's root Flex container.
  // The returned node is owned by the caller (added to m_tabBodies).
  // Implementations may cache a raw pointer to it for later use.
  virtual std::unique_ptr<Flex> create() = 0;

  // Optional trailing header actions shown in the shared control-center header
  // while this tab is active.
  virtual std::unique_ptr<Flex> createHeaderActions();

  // Called by ControlCenterPanel::layout() with the available content dimensions.
  void layout(Renderer& renderer, float contentWidth, float bodyHeight) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    doLayout(renderer, contentWidth, bodyHeight);
  }

  // Called by ControlCenterPanel::update() every frame.
  void update(Renderer& renderer) {
    UiPhaseScope updatePhase(UiPhase::Update);
    doUpdate(renderer);
  }

  // Called every Wayland frame callback with elapsed milliseconds. Default is a no-op.
  // Tabs override this to advance per-frame animations independently of data arrival.
  virtual void onFrameTick(float deltaMs) { (void)deltaMs; }

  // Called when the tab becomes visible or hidden.
  virtual void setActive(bool active) { (void)active; }

  // Live tabs that host a settings pane use this to show the editor instead of
  // the live view (for example `settings-toggle notifications`).
  virtual void setConfigureMode(bool show) { (void)show; }
  [[nodiscard]] virtual bool configureMode() const { return false; }

  // Sidebar (or header) settings search. Empty keeps the page filter; non-empty
  // searches the whole registry.
    virtual void setSettingsSearchQuery(std::string_view query) { (void)query; }

    [[nodiscard]] virtual class SettingsPane* settingsPane() { return nullptr; }

    // Called when the panel closes. Null out all raw pointers to freed nodes.
  virtual void onClose() {}

  // Allows a tab to close transient UI (menus/popovers) when external empty
  // chrome areas are clicked.
  virtual bool dismissTransientUi() { return false; }

  [[nodiscard]] virtual bool handleGlobalKey(
      std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
  ) {
    (void)sym;
    (void)modifiers;
    (void)pressed;
    (void)preedit;
    return false;
  }

  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  void setPanelCardOpacity(float opacity) noexcept {
    const float clamped = std::clamp(opacity, 0.0F, 1.0F);
    if (m_panelCardOpacity == clamped) {
      return;
    }
    m_panelCardOpacity = clamped;
    onPanelCardOpacityChanged(clamped);
  }

protected:
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  [[nodiscard]] float panelCardOpacity() const noexcept { return m_panelCardOpacity; }
  [[nodiscard]] float scaled(float value) const noexcept { return value * m_contentScale; }
  virtual void onPanelCardOpacityChanged(float opacity) { (void)opacity; }
  virtual void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
    (void)renderer;
    (void)contentWidth;
    (void)bodyHeight;
  }
  virtual void doUpdate(Renderer& renderer) { (void)renderer; }

private:
  float m_contentScale = 1.0F;
  float m_panelCardOpacity = 1.0F;
};
