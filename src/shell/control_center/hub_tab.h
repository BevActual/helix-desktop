#pragma once

#include "shell/control_center/tab.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Flex;
class Renderer;
class Segmented;

// A sidebar group that hosts existing tabs as sub-tabs: a chip row on top, the active
// child's page below. Children are ordinary standalone tabs and stay unaware of the hub;
// the hub forwards layout, update, header actions, the settings gear, search, and keys to
// whichever child is active. See docs/design/helix-hig.md (Settings structure).
class HubTab : public Tab {
public:
  struct Child {
    std::string id;
    std::string title;
    std::string glyph;
    std::unique_ptr<Tab> tab;
  };

  explicit HubTab(std::vector<Child> children);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;
  void onClose() override;
  bool dismissTransientUi() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] bool hasChild(std::string_view id) const noexcept;
  // Shows `id` if it is a child; returns false otherwise.
  bool selectChild(std::string_view id);
  [[nodiscard]] std::string_view activeChildId() const noexcept;
  [[nodiscard]] Tab* activeChild() const noexcept;
  [[nodiscard]] Tab* child(std::string_view id) const noexcept;
  [[nodiscard]] const std::vector<Child>& children() const noexcept { return m_children; }

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void applyChildVisibility();
  void syncChildScale();
  [[nodiscard]] std::size_t indexOf(std::string_view id) const noexcept;

  std::vector<Child> m_children;
  std::vector<Flex*> m_bodies;         // each child's root, owned by m_host
  std::vector<Flex*> m_headerActions;  // each child's header actions, owned by the hub's header row
  Flex* m_root = nullptr;
  Flex* m_host = nullptr;
  Segmented* m_switch = nullptr;
  std::size_t m_active = 0;
  bool m_isActive = false;
  bool m_syncing = false;
};
