#pragma once

#include "shell/agent/agent_confirmation_queue.h"
#include "shell/panel/panel.h"

#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>

class Button;
class Checkbox;
class Flex;
class Glyph;
class InputArea;
class Label;
class Renderer;

// Asks the user to approve an agent's action or to grant a new app access to the desktop. One request is shown at a
// time; the rest wait in the queue. Deliberately slow to approve: Allow is disabled for a moment after each new
// request, Enter never approves, and closing the panel denies whatever is still pending.
class AgentConfirmPanel : public Panel {
public:
  using QueueProvider = std::function<helix::agent::ConfirmationQueue*()>;
  static constexpr std::chrono::milliseconds kArmingDelay{700};

  explicit AgentConfirmPanel(QueueProvider queue);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(500.0F); }
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return false; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  static constexpr std::size_t kTierRows = 5;

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void allow();
  void deny();
  std::vector<std::string> selectedTiers() const;

  QueueProvider m_queue;
  Flex* m_rootLayout = nullptr;
  InputArea* m_focusArea = nullptr;
  Label* m_titleLabel = nullptr;
  Label* m_subjectLabel = nullptr;
  Label* m_detailLabel = nullptr;
  Label* m_reasonLabel = nullptr;
  Label* m_warningLabel = nullptr;
  Label* m_waitingLabel = nullptr;
  Flex* m_tierList = nullptr;
  std::array<Checkbox*, kTierRows> m_tierChecks{};
  std::array<Label*, kTierRows> m_tierLabels{};
  std::array<std::string, kTierRows> m_tierNames;
  Button* m_allowButton = nullptr;
  Button* m_denyButton = nullptr;

  std::uint64_t m_shownOperation = 0;
  std::chrono::steady_clock::time_point m_shownSince{};
};
