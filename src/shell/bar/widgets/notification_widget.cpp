#include "shell/bar/widgets/notification_widget.h"

#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

namespace {
  constexpr float kBadgeMinSize = 12.0F;
  constexpr float kBadgeFontSize = 9.0F;
  constexpr float kRingBaseSize = 14.0F;
  constexpr float kRingBaseThickness = 2.5F;
  constexpr float kRingTrackAlpha = 0.22F;
  constexpr std::size_t kBadgeCountCap = 9;
} // namespace

NotificationWidget::NotificationWidget(NotificationManager* manager, wl_output* /*output*/, Options options)
    : m_manager(manager), m_hideWhenNoUnread(options.hideWhenNoUnread),
      m_liveMaxWidth(static_cast<float>(std::max(0, options.liveMaxWidth))) {
  if (m_manager != nullptr) {
    m_eventToken = m_manager->addEventCallback([this](const Notification& n, NotificationEvent) {
      // Only progress notifications can change the chip; everything else is picked up by the
      // regular poll through doUpdate().
      if (n.progress.has_value() || n.id == m_liveId) {
        syncLiveActivity();
        requestUpdate();
      }
    });
  }
}

NotificationWidget::~NotificationWidget() {
  if (m_manager != nullptr && m_eventToken >= 0) {
    m_manager->removeEventCallback(m_eventToken);
  }
  if (m_animations != nullptr && m_ringAnim != 0) {
    m_animations->cancel(m_ringAnim);
  }
}

void NotificationWidget::create() {
  auto area = ui::inputArea({});

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "bell",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  auto badge = ui::box({
      .out = &m_badge,
      .fill = colorSpecFromRole(ColorRole::Primary),
      .visible = false,
      .participatesInLayout = false,
  });
  badge->addChild(ui::label({
      .out = &m_badgeLabel,
      .text = {},
      .fontSize = kBadgeFontSize * m_contentScale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnPrimary),
      .maxLines = 1,
      .visible = false,
      .participatesInLayout = false,
  }));
  area->addChild(std::move(badge));

  // Live activity chip: track ring, progress ring, summary.
  const float ringSize = kRingBaseSize * m_contentScale;
  auto track = std::make_unique<CountdownRingNode>();
  track->setThickness(kRingBaseThickness * m_contentScale);
  track->setProgress(1.0F);
  track->setSize(ringSize, ringSize);
  track->setVisible(false);
  track->setParticipatesInLayout(false);
  m_ringTrack = static_cast<CountdownRingNode*>(area->addChild(std::move(track)));

  auto ring = std::make_unique<CountdownRingNode>();
  ring->setThickness(kRingBaseThickness * m_contentScale);
  ring->setProgress(0.0F);
  ring->setSize(ringSize, ringSize);
  ring->setVisible(false);
  ring->setParticipatesInLayout(false);
  m_ring = static_cast<CountdownRingNode*>(area->addChild(std::move(ring)));

  area->addChild(
      ui::label({
          .out = &m_liveLabel,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxWidth = m_liveMaxWidth * m_contentScale,
          .maxLines = 1,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  area->addChild(
      ui::label({
          .out = &m_liveMore,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurfaceVariant)),
          .maxLines = 1,
          .visible = false,
          .participatesInLayout = false,
      })
  );

  setRoot(std::move(area));
  syncLiveActivity();
  refreshIndicatorState();
}

// hide_when_no_unread would pull the widget out from under the pointer the moment the panel it
// just opened marks everything read. The latch clears itself once that panel closes.
void NotificationWidget::onGestureDispatch(noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action) {
  (void)gesture;
  (void)action;
  m_openedPanelByClick = true;
}

bool NotificationWidget::findLiveActivity(LiveActivity& out) const {
  if (m_manager == nullptr) {
    return false;
  }
  const Notification* newest = nullptr;
  int count = 0;
  for (const auto& n : m_manager->all()) {
    if (!isLiveActivity(n)) {
      continue;
    }
    ++count;
    if (newest == nullptr || n.receivedTime >= newest->receivedTime) {
      newest = &n;
    }
  }
  if (newest == nullptr) {
    return false;
  }
  out.id = newest->id;
  out.summary = newest->summary.empty() ? newest->appName : newest->summary;
  out.progress = newest->progress.value_or(0);
  out.semantic = notificationSemantic(*newest);
  out.others = count - 1;
  return true;
}

void NotificationWidget::syncLiveActivity() {
  LiveActivity activity;
  const bool live = findLiveActivity(activity);

  std::string text = live ? activity.summary : std::string{};
  std::string more = live && activity.others > 0 ? i18n::tr("bar.widgets.notifications.live-more", "count", activity.others)
                                                  : std::string{};

  const bool wasLive = m_live;
  const bool textChanged = text != m_liveText;
  const bool moreChanged = more != m_liveMoreText;
  m_live = live;
  m_liveId = live ? activity.id : 0;
  m_liveSemantic = live ? activity.semantic : NotificationSemantic::None;
  m_liveText = std::move(text);
  m_liveMoreText = std::move(more);

  if (m_liveLabel != nullptr && textChanged) {
    m_liveLabel->setText(m_liveText);
  }
  if (m_liveMore != nullptr && moreChanged) {
    m_liveMore->setText(m_liveMoreText);
  }

  const float target = live ? std::clamp(static_cast<float>(activity.progress) / 100.0F, 0.0F, 1.0F) : 0.0F;
  const bool jump = !wasLive || m_animations == nullptr || m_ring == nullptr;
  if (jump || std::abs(target - m_ringProgress) < 0.001F) {
    m_ringProgress = target;
    if (m_ring != nullptr) {
      m_ring->setProgress(target);
    }
  } else {
    m_animations->cancel(m_ringAnim);
    m_ringAnim = m_animations->animate(
        m_ringProgress, target, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
        [this](float v) {
          m_ringProgress = v;
          if (m_ring != nullptr) {
            m_ring->setProgress(v);
          }
          requestRedraw();
        },
        [this]() { m_ringAnim = 0; }, this
    );
    requestFrameTick();
  }
}

void NotificationWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  refreshIndicatorState();
  if (!rootNode->visible()) {
    rootNode->setSize(0.0F, 0.0F);
    return;
  }

  const bool vertical = containerHeight > containerWidth;
  const bool showLive =
      m_live && m_ring != nullptr && m_ringTrack != nullptr && m_liveLabel != nullptr && m_liveMore != nullptr;

  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setGlyph(m_dndEnabled ? "bell-off" : "bell");
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);

  if (!showLive) {
    m_glyph->setVisible(true);
    if (m_ring != nullptr) {
      m_ring->setVisible(false);
      m_ringTrack->setVisible(false);
      m_liveLabel->setVisible(false);
      if (m_liveMore != nullptr) {
        m_liveMore->setVisible(false);
      }
    }
    m_glyph->setPosition(0.0F, 0.0F);
    syncBadge(&renderer);
    float width = m_glyph->width();
    float height = m_glyph->height();
    if (m_badge != nullptr && m_badge->visible()) {
      width = std::max(width, m_badge->x() + m_badge->width());
      height = std::max(height, m_badge->y() + m_badge->height());
    }
    rootNode->setSize(width, height);
    return;
  }

  // Chip: the ring takes the bell's place; the summary follows in horizontal bars.
  m_glyph->setVisible(false);
  if (m_badge != nullptr) {
    m_badge->setVisible(false);
  }
  if (m_badgeLabel != nullptr) {
    m_badgeLabel->setVisible(false);
  }

  const float ringSize = kRingBaseSize * m_contentScale;
  const float thickness = kRingBaseThickness * m_contentScale;
  const Color accent = m_liveSemantic == NotificationSemantic::None
      ? resolveColorSpec(widgetIconColorOr(colorSpecFromRole(ColorRole::Primary)))
      : semanticAccent(m_liveSemantic, colorForRole(ColorRole::Primary, 1.0F), colorForRole(ColorRole::Error, 1.0F));
  m_ringTrack->setSize(ringSize, ringSize);
  m_ringTrack->setThickness(thickness);
  m_ringTrack->setColor(
      resolveColorSpec(scaleAlpha(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), kRingTrackAlpha))
  );
  m_ringTrack->setVisible(true);
  m_ring->setSize(ringSize, ringSize);
  m_ring->setThickness(thickness);
  m_ring->setColor(accent);
  m_ring->setVisible(true);

  const bool showLabel = !vertical && m_liveMaxWidth > 0.0F && !m_liveText.empty();
  const bool showMore = showLabel && !m_liveMoreText.empty();
  m_liveLabel->setVisible(showLabel);
  m_liveMore->setVisible(showMore);
  float width = ringSize;
  float height = std::max(ringSize, m_glyph->height());
  if (showLabel) {
    // The count keeps its full width; the summary yields to it.
    float moreWidth = 0.0F;
    if (showMore) {
      m_liveMore->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurfaceVariant)));
      m_liveMore->setFontSize(Style::fontSizeBody * fontScale());
      m_liveMore->measure(renderer);
      moreWidth = m_liveMore->width() + Style::spaceXs * m_contentScale;
    }
    m_liveLabel->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_liveLabel->setFontSize(Style::fontSizeBody * fontScale());
    m_liveLabel->setMaxWidth(std::max(0.0F, m_liveMaxWidth * m_contentScale - moreWidth));
    m_liveLabel->measure(renderer);
    height = std::max(height, m_liveLabel->height());
    const float labelX = ringSize + Style::spaceXs * m_contentScale;
    m_liveLabel->setPosition(labelX, std::round((height - m_liveLabel->height()) * 0.5F));
    width = labelX + m_liveLabel->width();
    if (showMore) {
      const float moreX = width + Style::spaceXs * m_contentScale;
      m_liveMore->setPosition(moreX, std::round((height - m_liveMore->height()) * 0.5F));
      width = moreX + m_liveMore->width();
    }
  }

  const float ringY = std::round((height - ringSize) * 0.5F);
  m_ringTrack->setPosition(0.0F, ringY);
  m_ring->setPosition(0.0F, ringY);
  rootNode->setSize(width, height);
}

void NotificationWidget::doUpdate(Renderer& /*renderer*/) {
  syncLiveActivity();
  refreshIndicatorState();
}

void NotificationWidget::syncBadge(Renderer* renderer) {
  if (m_badge == nullptr || m_badgeLabel == nullptr) {
    return;
  }
  const bool show = !m_live && m_hasNotifications && !m_dndEnabled && m_unreadCount > 0;
  m_badge->setVisible(show);
  m_badgeLabel->setVisible(show);
  if (!show) {
    return;
  }
  const std::string text =
      m_unreadCount > kBadgeCountCap ? "9+" : std::format("{}", m_unreadCount);
  m_badgeLabel->setText(text);
  m_badgeLabel->setFontSize(kBadgeFontSize * m_contentScale);
  m_badgeLabel->setColor(colorSpecFromRole(ColorRole::OnPrimary));
  if (renderer != nullptr) {
    m_badgeLabel->measure(*renderer);
  }
  const float pad = Style::spaceXs * 0.5F * m_contentScale;
  const float minSize = kBadgeMinSize * m_contentScale;
  const float badgeW = std::max(minSize, m_badgeLabel->width() + pad * 2.0F);
  const float badgeH = std::max(minSize, m_badgeLabel->height() + pad);
  m_badge->setFill(colorSpecFromRole(ColorRole::Primary));
  m_badge->setRadius(badgeH * 0.5F);
  m_badge->setSize(badgeW, badgeH);
  // Keep the badge inside the widget box: bar capsules clip children, so a negative Y
  // (the usual "overhang" badge) was being cut off on floating glass bars.
  const float glyphW = m_glyph != nullptr ? m_glyph->width() : 0.0F;
  const float glyphH = m_glyph != nullptr ? m_glyph->height() : 0.0F;
  m_badge->setPosition(
      std::max(0.0F, glyphW - badgeW * 0.45F),
      std::max(0.0F, glyphH * 0.05F)
  );
  m_badgeLabel->setPosition(
      std::round((badgeW - m_badgeLabel->width()) * 0.5F),
      std::round((badgeH - m_badgeLabel->height()) * 0.5F)
  );
}

void NotificationWidget::refreshIndicatorState() {
  const bool hasNotifications = (m_manager != nullptr) && m_manager->hasUnreadNotificationHistory();
  const std::size_t unreadCount =
      m_manager != nullptr ? m_manager->unreadNotificationHistoryCount() : 0;
  const bool dndEnabled = (m_manager != nullptr) && m_manager->doNotDisturb();

  if (Node* rootNode = root(); rootNode != nullptr) {
    if (m_openedPanelByClick) {
      m_openedPanelByClick = PanelManager::instance().isOpenPanel("control-center");
    }
    const bool showWidget = m_live || m_openedPanelByClick || !m_hideWhenNoUnread || hasNotifications;
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
    if (!showWidget) {
      rootNode->setSize(0.0F, 0.0F);
    }
  }

  if (hasNotifications == m_hasNotifications && dndEnabled == m_dndEnabled && unreadCount == m_unreadCount) {
    return;
  }
  m_hasNotifications = hasNotifications;
  m_dndEnabled = dndEnabled;
  m_unreadCount = unreadCount;
  if (m_glyph != nullptr) {
    m_glyph->setGlyph(m_dndEnabled ? "bell-off" : "bell");
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  }
  if (!m_live) {
    syncBadge(nullptr);
  }
}
