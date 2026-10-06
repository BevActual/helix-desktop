#pragma once

#include "notification/notification_semantics.h"
#include "shell/bar/widget.h"

#include <cstdint>
#include <string>

struct wl_output;
class Box;
class CountdownRingNode;
class Glyph;
class Label;
class NotificationManager;

// Bell with an unread count badge. While a notification reports progress (the freedesktop `value`
// hint: a download, copy, update, phone transfer) the bell gives way to a live activity chip, a
// progress ring beside the activity's summary, the way iOS Live Activities sit in the Mac menu bar
// and Android Live Updates sit in the status bar. Click opens Notification Center.
class NotificationWidget : public Widget {
public:
  struct Options {
    bool hideWhenNoUnread = false;
    // Live activity chip label width in logical px; 0 hides the label and keeps only the ring.
    int liveMaxWidth = 160;
  };

  NotificationWidget(NotificationManager* manager, wl_output* output, Options options);
  ~NotificationWidget() override;

  void create() override;

private:
  struct LiveActivity {
    uint32_t id = 0;
    std::string summary;
    int progress = 0;
    NotificationSemantic semantic = NotificationSemantic::None;
    int others = 0; // further live activities beyond the one shown
  };

  void onGestureDispatch(noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action) override;
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refreshIndicatorState();
  void syncBadge(Renderer* renderer);
  [[nodiscard]] bool findLiveActivity(LiveActivity& out) const;
  void syncLiveActivity();

  NotificationManager* m_manager = nullptr;
  int m_eventToken = -1;
  Glyph* m_glyph = nullptr;
  Box* m_badge = nullptr;
  Label* m_badgeLabel = nullptr;
  CountdownRingNode* m_ringTrack = nullptr;
  CountdownRingNode* m_ring = nullptr;
  Label* m_liveLabel = nullptr;
  Label* m_liveMore = nullptr; // "+N" for further activities; never truncated
  bool m_hideWhenNoUnread = false;
  float m_liveMaxWidth = 160.0F;
  bool m_hasNotifications = false;
  bool m_dndEnabled = false;
  bool m_openedPanelByClick = false;
  std::size_t m_unreadCount = 0;

  bool m_live = false;
  uint32_t m_liveId = 0;
  NotificationSemantic m_liveSemantic = NotificationSemantic::None;
  std::string m_liveText;
  std::string m_liveMoreText;
  float m_ringProgress = 0.0F;
  std::uint32_t m_ringAnim = 0;
};
