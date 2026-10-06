#include "shell/agent/agent_confirm_panel.h"

#include "core/input/keybind_matcher.h"
#include "desktop_api/display_text.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace {

  using helix::agent::ConfirmRequest;

  // The tier names are a closed set from the service; unknown ones fall back to the raw name.
  std::string tierText(const std::string& tier) {
    const std::string key = "agent.tier." + tier;
    const std::string text = i18n::tr(key);
    return text.empty() || text == key ? tier : text;
  }

  int lines(std::size_t chars, std::size_t perLine, int maxLines) {
    const auto count = static_cast<int>((chars + perLine - 1) / std::max<std::size_t>(perLine, 1));
    return std::clamp(count, 1, maxLines);
  }

} // namespace

AgentConfirmPanel::AgentConfirmPanel(QueueProvider queue) : m_queue(std::move(queue)) {}

float AgentConfirmPanel::preferredHeight() const {
  const float scale = contentScale();
  const float bodyLine = Style::fontSizeBody * scale * 1.35F;
  const float captionLine = Style::fontSizeCaption * scale * 1.35F;
  const float pad = Style::spaceLg * scale;
  const float gap = Style::spaceMd * scale;
  const std::size_t perLine = static_cast<std::size_t>(std::max(20.0F, (preferredWidth() - pad * 3.0F) / (Style::fontSizeBody * scale * 0.55F)));

  float height = pad * 2.0F + Style::fontSizeTitle * scale * 1.35F + bodyLine + gap;  // header
  if (const auto* queue = m_queue ? m_queue() : nullptr; queue != nullptr && queue->front() != nullptr) {
    const ConfirmRequest& request = *queue->front();
    std::size_t detail = request.tool.size();
    for (const auto& arg : request.args) detail += arg.size() + 1;
    height += static_cast<float>(lines(detail, perLine, 4)) * bodyLine + gap;
    const bool grantWithLabel = request.kind == ConfirmRequest::Kind::Grant && !request.label.empty();
    const std::size_t asideChars = grantWithLabel ? request.label.size() + request.client.size() + 30 : (request.reason.empty() ? 0 : request.reason.size() + 20);
    if (asideChars > 0) height += static_cast<float>(lines(asideChars, perLine, 4)) * captionLine + gap;
    if (request.tainted) height += static_cast<float>(lines(180, perLine, 4)) * captionLine + gap;
    if (request.kind == ConfirmRequest::Kind::Grant) {
      // Checkbox rows are taller than a text line; sized from the control, as measured in the nested session.
      const float rowHeight = std::max(bodyLine, (Style::controlHeightSm + 4.0F) * scale) + Style::spaceSm * scale;
      height += static_cast<float>(request.tiers.size()) * rowHeight + gap;
    }
    if (queue->size() > 1) height += captionLine + gap;
  }
  return std::ceil(height + Style::controlHeight * scale + scaled(Style::panelPadding) * 2.0F);
}

void AgentConfirmPanel::create() {
  const float scale = contentScale();
  const float iconSize = scaled(40.0F);
  auto root = ui::column({.out = &m_rootLayout, .align = FlexAlign::Stretch, .gap = Style::spaceMd * scale,
                          .padding = Style::spaceLg * scale});

  auto focusArea = ui::inputArea({});
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  m_focusArea = static_cast<InputArea*>(root->addChild(std::move(focusArea)));

  root->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
      ui::glyph({.glyph = "robot", .glyphSize = iconSize, .color = colorSpecFromRole(ColorRole::Primary)}),
      ui::column(
          {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
          ui::label({.out = &m_titleLabel, .fontSize = Style::fontSizeTitle * scale, .fontWeight = FontWeight::Bold,
                     .color = colorSpecFromRole(ColorRole::Primary)}),
          ui::label({.out = &m_subjectLabel, .fontSize = Style::fontSizeBody * scale,
                     .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 2}))));

  root->addChild(ui::label({.out = &m_detailLabel, .fontSize = Style::fontSizeBody * scale,
                            .color = colorSpecFromRole(ColorRole::OnSurface), .maxLines = 4}));
  root->addChild(ui::label({.out = &m_reasonLabel, .fontSize = Style::fontSizeCaption * scale,
                            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 4}));

  auto tiers = ui::column({.out = &m_tierList, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
  for (std::size_t i = 0; i < kTierRows; ++i) {
    tiers->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::checkbox({.out = &m_tierChecks[i], .scale = scale}),
        ui::label({.out = &m_tierLabels[i], .fontSize = Style::fontSizeBody * scale,
                   .color = colorSpecFromRole(ColorRole::OnSurface), .maxLines = 2, .flexGrow = 1.0F})));
  }
  root->addChild(std::move(tiers));

  root->addChild(ui::label({.out = &m_warningLabel, .fontSize = Style::fontSizeCaption * scale,
                            .color = colorSpecFromRole(ColorRole::Error), .maxLines = 4}));
  root->addChild(ui::label({.out = &m_waitingLabel, .fontSize = Style::fontSizeCaption * scale,
                            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));

  root->addChild(ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::End, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true},
      ui::button({.out = &m_denyButton, .text = i18n::tr("agent.confirm.deny"), .variant = ButtonVariant::Outline,
                  .onClick = [this]() { deny(); }}),
      ui::button({.out = &m_allowButton, .text = i18n::tr("agent.confirm.allow"), .variant = ButtonVariant::Primary,
                  .onClick = [this]() { allow(); }})));
  setRoot(std::move(root));
}

void AgentConfirmPanel::onOpen(std::string_view /*context*/) {
  m_shownOperation = 0;  // arm again for whatever is shown first
}

void AgentConfirmPanel::onClose() {
  // Closing without an answer is a refusal. The service would deny on its own deadline; this is just sooner.
  if (auto* queue = m_queue ? m_queue() : nullptr; queue != nullptr) queue->denyAll();
  clearReleasedRoot();
  m_rootLayout = nullptr;
  m_focusArea = nullptr;
  m_titleLabel = m_subjectLabel = m_detailLabel = m_reasonLabel = m_warningLabel = m_waitingLabel = nullptr;
  m_tierList = nullptr;
  m_tierChecks.fill(nullptr);
  m_tierLabels.fill(nullptr);
  m_allowButton = m_denyButton = nullptr;
}

InputArea* AgentConfirmPanel::initialFocusArea() const { return m_focusArea; }

bool AgentConfirmPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool /*preedit*/) {
  // Only Cancel is bound. Enter must not approve: a keystroke already in flight when the dialog appears would.
  if (!pressed || !KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) return false;
  deny();
  return true;
}

std::vector<std::string> AgentConfirmPanel::selectedTiers() const {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < kTierRows; ++i)
    if (m_tierChecks[i] != nullptr && m_tierChecks[i]->visible() && m_tierChecks[i]->checked()) out.push_back(m_tierNames[i]);
  return out;
}

void AgentConfirmPanel::allow() {
  auto* queue = m_queue ? m_queue() : nullptr;
  if (queue == nullptr || queue->front() == nullptr) return;
  if (std::chrono::steady_clock::now() - m_shownSince < kArmingDelay) return;  // ignore clicks during the arming window
  const bool grant = queue->front()->kind == ConfirmRequest::Kind::Grant;
  auto tiers = grant ? selectedTiers() : std::vector<std::string>{};
  if (grant && tiers.empty()) return;
  queue->answerFront(true, std::move(tiers));
}

void AgentConfirmPanel::deny() {
  if (auto* queue = m_queue ? m_queue() : nullptr; queue != nullptr && queue->front() != nullptr) queue->answerFront(false);
}

void AgentConfirmPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) return;
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
}

void AgentConfirmPanel::doUpdate(Renderer& /*renderer*/) {
  auto* queue = m_queue ? m_queue() : nullptr;
  if (queue == nullptr || queue->front() == nullptr || m_titleLabel == nullptr || m_allowButton == nullptr) return;
  const ConfirmRequest& request = *queue->front();
  const bool grant = request.kind == ConfirmRequest::Kind::Grant;

  if (request.operation != m_shownOperation) {
    m_shownOperation = request.operation;
    m_shownSince = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kTierRows; ++i) {
      const bool used = grant && i < request.tiers.size();
      m_tierNames[i] = used ? request.tiers[i] : std::string();
      m_tierChecks[i]->setVisible(used);
      m_tierLabels[i]->setVisible(used);
      if (used) {
        // Least privilege by default: the tier that needs approval each time is opt-in.
        m_tierChecks[i]->setChecked(request.tiers[i] != "confirm");
        m_tierLabels[i]->setText(tierText(request.tiers[i]));
      }
    }
    if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel("agent-confirm"))
      manager->relayoutActivePanelPreferredSize();
  }

  m_titleLabel->setText(i18n::tr(grant ? "agent.confirm.grant-title" : "agent.confirm.action-title"));
  // The verified identity leads. The app's own label is shown only as something it says about itself.
  const std::string& app = request.clientName;
  m_subjectLabel->setText(i18n::tr(grant ? "agent.confirm.grant-subject" : "agent.confirm.action-subject", "app", app));
  std::string detail = request.tool;
  for (const auto& arg : request.args) detail += " " + arg;
  m_detailLabel->setText(grant ? i18n::tr("agent.confirm.grant-detail") : detail);
  std::string aside = request.reason.empty() ? std::string() : i18n::tr("agent.confirm.reason", "reason", request.reason);
  if (grant && !request.label.empty()) aside = i18n::tr("agent.confirm.label", "label", request.label) + "  " + request.client;
  m_reasonLabel->setText(aside);
  m_reasonLabel->setVisible(!aside.empty());
  m_warningLabel->setText(i18n::tr("agent.confirm.tainted"));
  m_warningLabel->setVisible(request.tainted && !grant);
  m_waitingLabel->setText(i18n::tr("agent.confirm.waiting", "count", std::to_string(queue->size() - 1)));
  m_waitingLabel->setVisible(queue->size() > 1);
  m_tierList->setVisible(grant);

  const bool armed = std::chrono::steady_clock::now() - m_shownSince >= kArmingDelay;
  m_allowButton->setEnabled(armed);
  if (!armed) {
    if (auto* manager = PanelManager::current(); manager != nullptr) manager->requestUpdateOnly();  // keep frames coming
  }
}
