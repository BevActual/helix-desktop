#pragma once

#include "shell/assistant/assistant_session.h"
#include "shell/panel/panel.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

class Button;
class Flex;
class Input;
class InputArea;
class Label;
class Renderer;

// Type what you want in plain English; the assistant works out the action. Reading the desktop happens at once. Anything that
// changes something is shown first and runs only after you press Do it. The panel is only a face: `helix-assistant` does the
// work under its own permissions, so closing or breaking the panel never gives anything the model's authority.
// Like the approval dialog, Enter never confirms and Do it is disabled for a moment after it appears.
class AssistantPanel : public Panel {
public:
  static constexpr std::chrono::milliseconds kArmingDelay{700};

  AssistantPanel();

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(520.0F); }
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] InputArea* takePendingFocusArea() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void submit(const std::string& text);

  std::shared_ptr<int> m_life = std::make_shared<int>(0);  // callbacks from the helper process check this before touching the panel
  helix::assistant_ui::Session m_session;
  helix::assistant_ui::Phase m_shownPhase = helix::assistant_ui::Phase::Idle;
  std::chrono::steady_clock::time_point m_confirmShownSince{};

  bool m_wantInputFocus = false;  // set when the panel goes back to waiting for a request

  Flex* m_rootLayout = nullptr;
  Input* m_input = nullptr;
  Label* m_bodyLabel = nullptr;
  Label* m_reasonLabel = nullptr;
  Label* m_noteLabel = nullptr;
  Flex* m_buttonRow = nullptr;
  Button* m_doButton = nullptr;
  Button* m_cancelButton = nullptr;
};
