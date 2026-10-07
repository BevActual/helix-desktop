#include "shell/assistant/assistant_panel.h"

#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

using helix::assistant_ui::Phase;

namespace {

  // Starts helix-assistant and delivers its output on the main thread. If the panel is gone by then, nothing happens.
  bool runAssistant(const std::weak_ptr<int>& life, std::vector<std::string> arguments, helix::assistant_ui::Session::Done done) {
    std::vector<std::string> command{"helix-assistant"};
    command.insert(command.end(), std::make_move_iterator(arguments.begin()), std::make_move_iterator(arguments.end()));
    process::RunCallbacks callbacks;
    callbacks.onExit = [life, done = std::move(done)](process::RunResult result) mutable {
      DeferredCall::callLater([life, done = std::move(done), result = std::move(result)]() mutable {
        if (life.lock() == nullptr) return;
        done(result.exitCode, std::move(result.out));
      });
    };
    process::RunOptions options;
    options.timeout = std::chrono::minutes(6);  // a permission dialog can wait 90 s, and the first request loads the model
    options.maxOutputBytes = 64 * 1024;
    return process::runAsync(command, std::move(callbacks), std::move(options));
  }

  int lines(std::size_t chars, std::size_t perLine, int maxLines) {
    const auto count = static_cast<int>((chars + perLine - 1) / std::max<std::size_t>(perLine, 1));
    return std::clamp(count, 1, maxLines);
  }

} // namespace

AssistantPanel::AssistantPanel()
    : m_session(
          [this](std::vector<std::string> arguments, helix::assistant_ui::Session::Done done) {
            return runAssistant(std::weak_ptr<int>(m_life), std::move(arguments), std::move(done));
          },
          [] {
            if (auto* manager = PanelManager::current(); manager != nullptr) manager->requestUpdateOnly();
          }) {}

float AssistantPanel::preferredHeight() const {
  const float scale = contentScale();
  const float bodyLine = Style::fontSizeBody * scale * 1.35F;
  const float captionLine = Style::fontSizeCaption * scale * 1.35F;
  const float pad = Style::spaceLg * scale;
  const float gap = Style::spaceMd * scale;
  const std::size_t perLine = static_cast<std::size_t>(std::max(20.0F, (preferredWidth() - pad * 2.0F) / (Style::fontSizeBody * scale * 0.55F)));
  float height = pad * 2.0F + Style::fontSizeTitle * scale * 1.35F + gap + Style::controlHeight * scale;
  const auto& view = m_session.view();
  switch (view.phase) {
    case Phase::Idle: break;
    case Phase::Thinking:
    case Phase::Acting: height += gap + bodyLine; break;
    case Phase::Confirm:
      height += gap + static_cast<float>(lines(view.description.size(), perLine, 4)) * bodyLine;
      if (!view.reason.empty()) height += gap + static_cast<float>(lines(view.reason.size() + 12, perLine, 3)) * captionLine;
      height += gap + Style::controlHeight * scale;
      break;
    case Phase::Result:
      height += gap + static_cast<float>(lines(view.text.size(), perLine, 8)) * bodyLine;
      if (view.unverified) height += gap + captionLine;
      break;
  }
  return std::ceil(height + scaled(Style::panelPadding));
}

void AssistantPanel::create() {
  const float scale = contentScale();
  auto root = ui::column({.out = &m_rootLayout, .align = FlexAlign::Stretch, .gap = Style::spaceMd * scale, .padding = Style::spaceLg * scale});

  root->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
      ui::glyph({.glyph = "robot", .glyphSize = scaled(28.0F), .color = colorSpecFromRole(ColorRole::Primary)}),
      ui::label({.text = i18n::tr("assistant.title"), .fontSize = Style::fontSizeTitle * scale, .fontWeight = FontWeight::Bold,
                 .color = colorSpecFromRole(ColorRole::Primary)})));

  root->addChild(ui::input({
      .out = &m_input,
      .placeholder = i18n::tr("assistant.placeholder"),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .horizontalPadding = Style::spaceMd * scale,
      .lineEditing = true,
      .onSubmit = [this](const std::string& text) { submit(text); },
  }));

  root->addChild(ui::label({.out = &m_bodyLabel, .fontSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(ColorRole::OnSurface), .maxLines = 8}));
  root->addChild(ui::label({.out = &m_reasonLabel, .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 3}));
  root->addChild(ui::label({.out = &m_noteLabel, .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));

  root->addChild(ui::row(
      {.out = &m_buttonRow, .align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true},
      ui::button({.out = &m_cancelButton, .text = i18n::tr("assistant.cancel"), .variant = ButtonVariant::Outline, .onClick = [this]() { m_session.cancel(); }}),
      ui::button({.out = &m_doButton, .text = i18n::tr("assistant.do-it"), .variant = ButtonVariant::Primary, .onClick = [this]() {
        // A click that lands while the dialog is still arming is ignored, like the approval dialog.
        if (std::chrono::steady_clock::now() - m_confirmShownSince < kArmingDelay) return;
        m_session.confirm();
      }})));
  setRoot(std::move(root));
}

void AssistantPanel::onOpen(std::string_view /*context*/) {
  m_session.cancel();  // always start fresh; nothing from an earlier visit can still be waiting for a yes
  m_shownPhase = Phase::Idle;
  if (m_input != nullptr) m_input->setValue("");
}

void AssistantPanel::onClose() {
  m_session.cancel();
  clearReleasedRoot();
  m_rootLayout = nullptr;
  m_input = nullptr;
  m_bodyLabel = m_reasonLabel = m_noteLabel = nullptr;
  m_buttonRow = nullptr;
  m_doButton = m_cancelButton = nullptr;
}

InputArea* AssistantPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

InputArea* AssistantPanel::takePendingFocusArea() {
  if (!m_wantInputFocus || m_input == nullptr) return nullptr;
  m_wantInputFocus = false;
  return m_input->inputArea();
}

bool AssistantPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit || !KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) return false;
  // Escape first backs out of a pending request or change; only when there is nothing to back out of does it close the panel.
  if (m_session.view().phase == Phase::Idle) return false;
  m_session.cancel();
  if (m_input != nullptr) m_input->setValue("");
  return true;
}

void AssistantPanel::submit(const std::string& text) {
  m_session.submit(text);
  if (m_session.view().phase == Phase::Thinking && m_input != nullptr) m_input->setValue("");
}

void AssistantPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) return;
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
}

void AssistantPanel::doUpdate(Renderer& /*renderer*/) {
  if (m_bodyLabel == nullptr || m_doButton == nullptr) return;
  const auto& view = m_session.view();

  std::string body;
  std::string reason;
  std::string note;
  switch (view.phase) {
    case Phase::Idle: break;
    case Phase::Thinking: body = i18n::tr("assistant.thinking"); break;
    case Phase::Acting: body = i18n::tr("assistant.acting"); break;
    case Phase::Confirm:
      body = i18n::tr("assistant.about", "what", view.description);
      if (!view.reason.empty()) reason = i18n::tr("assistant.reason", "reason", view.reason);
      break;
    case Phase::Result:
      body = view.text;
      if (view.unverified) note = i18n::tr("assistant.unverified");
      break;
  }
  m_bodyLabel->setText(body);
  m_bodyLabel->setVisible(!body.empty());
  m_reasonLabel->setText(reason);
  m_reasonLabel->setVisible(!reason.empty());
  m_noteLabel->setText(note);
  m_noteLabel->setVisible(!note.empty());
  const bool confirming = view.phase == Phase::Confirm;
  m_buttonRow->setVisible(confirming);

  if (view.phase != m_shownPhase) {
    if (confirming) m_confirmShownSince = std::chrono::steady_clock::now();
    m_shownPhase = view.phase;
    if (view.phase == Phase::Idle || view.phase == Phase::Result) m_wantInputFocus = true;  // ready for the next request
    if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel("assistant"))
      manager->relayoutActivePanelPreferredSize();
  }

  const bool armed = std::chrono::steady_clock::now() - m_confirmShownSince >= kArmingDelay;
  m_doButton->setEnabled(confirming && armed);
  if (confirming && !armed) {
    if (auto* manager = PanelManager::current(); manager != nullptr) manager->requestUpdateOnly();  // keep frames coming
  }
}
