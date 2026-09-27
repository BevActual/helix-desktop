#include "shell/bar/widgets/ai_widget.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/tooltip/tooltip_content.h"
#include "system/ai_usage.h"
#include "system/ai_usage_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <optional>
#include <vector>

AiWidget::AiWidget(AiUsageService* aiUsage, Options options)
    : m_aiUsage(aiUsage), m_maxWidth(static_cast<float>(options.maxWidth)) {}

void AiWidget::create() {
  auto area = ui::inputArea({});
  m_area = area.get();

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "sparkles",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .maxWidth = m_maxWidth * m_contentScale,
          .maxLines = 1,
      })
  );

  setRoot(std::move(area));
}

void AiWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_glyph == nullptr || m_label == nullptr || root() == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  sync(renderer);

  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_label->setTextAlign(m_isVertical ? TextAlign::Center : TextAlign::Start);
  m_label->setMaxWidth(m_isVertical ? containerWidth : (m_maxWidth * m_contentScale));
  m_glyph->measure(renderer);
  m_label->measure(renderer);

  const float spacing = m_label->text().empty() ? 0.0F : (Style::spaceXs * m_contentScale);
  if (m_isVertical) {
    const float contentWidth = std::max(m_glyph->width(), m_label->width());
    m_glyph->setPosition(std::round((contentWidth - m_glyph->width()) * 0.5F), 0.0F);
    m_label->setPosition(std::round((contentWidth - m_label->width()) * 0.5F), m_glyph->height() + spacing);
    root()->setSize(contentWidth, m_label->y() + m_label->height());
  } else {
    const float contentHeight = std::max(m_glyph->height(), m_label->height());
    m_glyph->setPosition(0.0F, std::round((contentHeight - m_glyph->height()) * 0.5F));
    m_label->setPosition(m_glyph->width() + spacing, std::round((contentHeight - m_label->height()) * 0.5F));
    root()->setSize(m_label->x() + m_label->width(), contentHeight);
  }
}

void AiWidget::doUpdate(Renderer& renderer) { sync(renderer); }

void AiWidget::sync(Renderer& renderer) {
  if (m_glyph == nullptr || m_label == nullptr) {
    return;
  }

  constexpr const char* kVerticalUnavailable = "-";
  constexpr const char* kVerticalLoading = "...";

  std::string glyph = "sparkles";
  std::string text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.ai.default");
  bool hot = false;
  std::optional<noctalia::ai_usage::QuotaHit> hit;
  const noctalia::ai_usage::Snapshot* snapshot = nullptr;

  if (m_aiUsage != nullptr) {
    snapshot = &m_aiUsage->snapshot();
    hit = noctalia::ai_usage::hottestQuota(*snapshot);
    if (noctalia::ai_usage::activeOllamaPull(*snapshot) != nullptr) {
      glyph = "server";
      text = noctalia::ai_usage::snapshotGlanceLabel(*snapshot);
    } else if (noctalia::ai_usage::activeLocalAiJob(*snapshot) != nullptr) {
      glyph = "stack-2";
      text = noctalia::ai_usage::snapshotGlanceLabel(*snapshot);
    } else if (hit.has_value()) {
      glyph = std::string(noctalia::ai_usage::providerGlyph(hit->provider));
      text = std::format("{:.0f}%", static_cast<double>(hit->window.percent));
      hot = hit->window.percent + 0.5F >= static_cast<float>(m_aiUsage->attentionPercent());
    } else if (noctalia::ai_usage::glanceableLocalAi(*snapshot)) {
      glyph = "stack-2";
      text = noctalia::ai_usage::localAiGlanceLabel(snapshot->localAi);
    } else if (noctalia::ai_usage::glanceableOllama(*snapshot)) {
      glyph = "server";
      text = noctalia::ai_usage::ollamaGlanceLabel(snapshot->ollama);
    } else if (const auto* runtime = noctalia::ai_usage::glanceableLocalRuntime(*snapshot)) {
      glyph = std::string(noctalia::ai_usage::localRuntimeGlyph(runtime->kind));
      text = noctalia::ai_usage::localRuntimeLabel(runtime->kind);
    } else if (snapshot->refreshing) {
      text = m_isVertical ? kVerticalLoading : i18n::tr("bar.widgets.ai.updating");
    }
  }

  const auto color = hot ? colorSpecFromRole(ColorRole::Error)
                         : widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface));
  const bool hotChanged = hot != m_lastHot;

  bool changed = false;
  if (glyph != m_lastGlyph || hotChanged) {
    m_lastGlyph = glyph;
    m_glyph->setGlyph(glyph);
    m_glyph->setColor(color);
    m_glyph->measure(renderer);
    changed = true;
  }

  if (text != m_lastText || hotChanged) {
    m_lastText = text;
    m_label->setText(text);
    m_label->setColor(
        hot ? colorSpecFromRole(ColorRole::Error) : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
    );
    m_label->measure(renderer);
    changed = true;
  }
  m_lastHot = hot;

  if (changed) {
    requestRedraw();
  }

  if (m_area == nullptr) {
    return;
  }

  const bool showLocal = snapshot != nullptr
      && (noctalia::ai_usage::activeOllamaPull(*snapshot) != nullptr
          || noctalia::ai_usage::activeLocalAiJob(*snapshot) != nullptr
          || noctalia::ai_usage::glanceableLocalAi(*snapshot) || noctalia::ai_usage::glanceableOllama(*snapshot)
          || noctalia::ai_usage::glanceableLocalRuntime(*snapshot) != nullptr);
  if (snapshot == nullptr || (!hit.has_value() && !showLocal)) {
    m_area->clearTooltip();
    return;
  }

  std::vector<TooltipRow> rows;
  if (hit.has_value()) {
    rows.push_back({i18n::tr("bar.widgets.ai.tooltip.provider"), noctalia::ai_usage::providerLabel(hit->provider)});
    rows.push_back({i18n::tr("bar.widgets.ai.tooltip.window"), noctalia::ai_usage::windowLabel(hit->window.key)});
    rows.push_back(
        {i18n::tr("bar.widgets.ai.tooltip.quota"), std::format("{:.0f}%", static_cast<double>(hit->window.percent))}
    );
    if (hit->window.used.has_value() && hit->window.max.has_value()) {
      rows.push_back(
          {i18n::tr("bar.widgets.ai.tooltip.used"),
           std::format("{:.0f}/{:.0f}", *hit->window.used, *hit->window.max)}
      );
    }
  }
  if (noctalia::ai_usage::activeOllamaPull(*snapshot) != nullptr) {
    rows.push_back(
        {i18n::tr("bar.widgets.ai.tooltip.ollama"), i18n::tr("control-center.ai-usage.ollama.installing")}
    );
    rows.push_back(
        {i18n::tr("bar.widgets.ai.tooltip.model"), noctalia::ai_usage::activeOllamaPull(*snapshot)->name}
    );
  } else if (const auto* job = noctalia::ai_usage::activeLocalAiJob(*snapshot)) {
    rows.push_back(
        {i18n::tr("control-center.ai-usage.localai.title"),
         i18n::tr(
             job->deletion ? "control-center.ai-usage.ollama.removing" : "control-center.ai-usage.ollama.installing"
         )}
    );
    rows.push_back({i18n::tr("bar.widgets.ai.tooltip.model"), job->model});
  } else if (noctalia::ai_usage::glanceableLocalAi(*snapshot)) {
    rows.push_back(
        {i18n::tr("control-center.ai-usage.localai.title"),
         noctalia::ai_usage::localAiStatusLabel(snapshot->localAi.status)}
    );
    if (const std::string_view model = noctalia::ai_usage::effectiveLocalAiModel(snapshot->localAi); !model.empty()) {
      rows.push_back({i18n::tr("bar.widgets.ai.tooltip.model"), std::string(model)});
    }
  } else if (noctalia::ai_usage::glanceableOllama(*snapshot)) {
    rows.push_back(
        {i18n::tr("bar.widgets.ai.tooltip.ollama"), noctalia::ai_usage::ollamaStatusLabel(snapshot->ollama.status)}
    );
    if (!snapshot->ollama.selected.empty()) {
      rows.push_back({i18n::tr("bar.widgets.ai.tooltip.model"), snapshot->ollama.selected});
    } else if (const std::string_view model = noctalia::ai_usage::effectiveOllamaModel(snapshot->ollama);
               !model.empty()) {
      rows.push_back({i18n::tr("bar.widgets.ai.tooltip.model"), std::string(model)});
    }
    if (!snapshot->ollama.loading.empty()) {
      rows.push_back({i18n::tr("bar.widgets.ai.tooltip.loading"), snapshot->ollama.loading});
    }
  } else if (const auto* runtime = noctalia::ai_usage::glanceableLocalRuntime(*snapshot)) {
    rows.push_back({i18n::tr("bar.widgets.ai.tooltip.local"), noctalia::ai_usage::localRuntimeLabel(runtime->kind)});
  }
  m_area->setTooltip(std::move(rows));
}
