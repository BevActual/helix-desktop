#include "shell/control_center/hub_tab.h"

#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/segmented.h"
#include "ui/style.h"

#include <algorithm>
#include <utility>

HubTab::HubTab(std::vector<Child> children) : m_children(std::move(children)) {}

std::size_t HubTab::indexOf(std::string_view id) const noexcept {
  for (std::size_t i = 0; i < m_children.size(); ++i) {
    if (m_children[i].id == id) {
      return i;
    }
  }
  return m_children.size();
}

bool HubTab::hasChild(std::string_view id) const noexcept { return indexOf(id) < m_children.size(); }

std::string_view HubTab::activeChildId() const noexcept {
  return m_active < m_children.size() ? std::string_view(m_children[m_active].id) : std::string_view{};
}

Tab* HubTab::activeChild() const noexcept {
  return m_active < m_children.size() ? m_children[m_active].tab.get() : nullptr;
}

Tab* HubTab::child(std::string_view id) const noexcept {
  const std::size_t index = indexOf(id);
  return index < m_children.size() ? m_children[index].tab.get() : nullptr;
}

void HubTab::syncChildScale() {
  for (auto& child : m_children) {
    child.tab->setContentScale(contentScale());
    child.tab->setPanelCardOpacity(panelCardOpacity());
  }
}

std::unique_ptr<Flex> HubTab::create() {
  const float scale = contentScale();
  syncChildScale();

  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  std::vector<ui::SegmentedOption> options;
  options.reserve(m_children.size());
  for (const auto& child : m_children) {
    options.push_back({.label = child.title, .glyph = child.glyph, .tooltip = child.title});
  }
  root->addChild(ui::segmented({
      .out = &m_switch,
      .options = std::move(options),
      .selectedIndex = m_active,
      .scale = scale,
      .surfaceOpacity = panelCardOpacity(),
      .onChange =
          [this](std::size_t index) {
            if (!m_syncing && index < m_children.size()) {
              (void)selectChild(m_children[index].id);
            }
          },
  }));

  auto host = ui::column({
      .out = &m_host,
      .align = FlexAlign::Stretch,
      .flexGrow = 1.0F,
  });
  m_bodies.clear();
  for (auto& child : m_children) {
    auto body = child.tab->create();
    body->setFlexGrow(1.0F);
    m_bodies.push_back(body.get());
    host->addChild(std::move(body));
  }
  root->addChild(std::move(host));

  applyChildVisibility();
  return root;
}

std::unique_ptr<Flex> HubTab::createHeaderActions() {
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * contentScale()});
  m_headerActions.assign(m_children.size(), nullptr);
  bool any = false;
  for (std::size_t i = 0; i < m_children.size(); ++i) {
    if (auto actions = m_children[i].tab->createHeaderActions(); actions != nullptr) {
      m_headerActions[i] = actions.get();
      row->addChild(std::move(actions));
      any = true;
    }
  }
  if (!any) {
    return nullptr;
  }
  applyChildVisibility();
  return row;
}

void HubTab::applyChildVisibility() {
  for (std::size_t i = 0; i < m_bodies.size(); ++i) {
    if (m_bodies[i] != nullptr) {
      m_bodies[i]->setVisible(i == m_active);
      m_bodies[i]->setParticipatesInLayout(i == m_active);
    }
  }
  for (std::size_t i = 0; i < m_headerActions.size(); ++i) {
    if (m_headerActions[i] != nullptr) {
      m_headerActions[i]->setVisible(i == m_active);
      m_headerActions[i]->setParticipatesInLayout(i == m_active);
    }
  }
  if (m_switch != nullptr && m_switch->selectedIndex() != m_active) {
    m_syncing = true;
    m_switch->setSelectedIndex(m_active);
    m_syncing = false;
  }
}

bool HubTab::selectChild(std::string_view id) {
  const std::size_t index = indexOf(id);
  if (index >= m_children.size()) {
    return false;
  }
  if (index == m_active) {
    applyChildVisibility();
    return true;
  }
  Tab* previous = activeChild();
  const bool wasConfiguring = previous != nullptr && previous->configureMode();
  if (previous != nullptr && m_isActive) {
    previous->setActive(false);
  }
  m_active = index;
  applyChildVisibility();
  if (Tab* next = activeChild(); next != nullptr) {
    // Keep the gear state across sub-tabs so browsing a group's settings stays in settings.
    next->setConfigureMode(wasConfiguring);
    if (m_isActive) {
      next->setActive(true);
    }
  }
  PanelManager::instance().requestLayout();
  return true;
}

void HubTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  syncChildScale();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
  Tab* child = activeChild();
  if (child == nullptr) {
    return;
  }
  float hostWidth = contentWidth;
  float hostHeight = bodyHeight;
  if (m_host != nullptr && m_host->width() > 1.0F) {
    hostWidth = std::max(1.0F, m_host->width() - m_host->paddingLeft() - m_host->paddingRight());
  }
  if (m_host != nullptr && m_host->height() > 1.0F) {
    hostHeight = std::max(1.0F, m_host->height() - m_host->paddingTop() - m_host->paddingBottom());
  }
  child->layout(renderer, hostWidth, hostHeight);
}

void HubTab::doUpdate(Renderer& renderer) {
  if (Tab* child = activeChild(); child != nullptr) {
    child->update(renderer);
  }
}

void HubTab::onFrameTick(float deltaMs) {
  if (Tab* child = activeChild(); child != nullptr) {
    child->onFrameTick(deltaMs);
  }
}

void HubTab::setActive(bool active) {
  m_isActive = active;
  if (Tab* child = activeChild(); child != nullptr) {
    child->setActive(active);
  }
}

void HubTab::setConfigureMode(bool show) {
  if (Tab* child = activeChild(); child != nullptr) {
    child->setConfigureMode(show);
  }
}

bool HubTab::configureMode() const {
  const Tab* child = activeChild();
  return child != nullptr && child->configureMode();
}

void HubTab::setSettingsSearchQuery(std::string_view query) {
  if (Tab* child = activeChild(); child != nullptr) {
    child->setSettingsSearchQuery(query);
  }
}

SettingsPane* HubTab::settingsPane() {
  Tab* child = activeChild();
  return child != nullptr ? child->settingsPane() : nullptr;
}

void HubTab::onClose() {
  for (auto& child : m_children) {
    child.tab->onClose();
  }
  m_bodies.clear();
  m_headerActions.clear();
  m_root = nullptr;
  m_host = nullptr;
  m_switch = nullptr;
}

bool HubTab::dismissTransientUi() {
  Tab* child = activeChild();
  return child != nullptr && child->dismissTransientUi();
}

bool HubTab::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  Tab* child = activeChild();
  return child != nullptr && child->handleGlobalKey(sym, modifiers, pressed, preedit);
}

void HubTab::onPanelCardOpacityChanged(float opacity) {
  for (auto& child : m_children) {
    child.tab->setPanelCardOpacity(opacity);
  }
  if (m_switch != nullptr) {
    m_switch->setSurfaceOpacity(opacity);
  }
}
