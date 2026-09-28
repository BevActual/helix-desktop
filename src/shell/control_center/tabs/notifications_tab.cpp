#include "shell/control_center/tabs/notifications_tab.h"
#include "core/input/keybind_matcher.h"
#include "ui/controls/scroll_view.h"

#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/uri.h"
#include "notification/notification.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "render/animation/animation.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr Logger kLog("control-center-notifications");

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  void requestPanelLayout() {
    if (auto* panels = PanelManager::current(); panels != nullptr) {
      panels->requestLayout();
    }
  }

  constexpr float kHistoryIconSize = 36.0F;
  constexpr float kHistoryIconGlyphSize = 22.0F;
  constexpr float kHistoryIconReferenceSize = 36.0F;

  float notificationIconRadius(float iconSize, float localScale) {
    const float baseRadius = Style::radiusMd * (iconSize / kHistoryIconReferenceSize);
    return std::min(iconSize * 0.5F, Style::scaledRadius(baseRadius, localScale));
  }
  constexpr float kNotificationActionButtonSize = Style::controlHeightSm;

  std::string historyActionLabel(std::string_view actionKey, std::string_view actionLabel) {
    if (!StringUtils::isBlank(actionLabel)) {
      return std::string(actionLabel);
    }
    if (actionKey == "default") {
      return i18n::tr("notifications.actions.open");
    }
    if (actionKey == "inline-reply") {
      return i18n::tr("notifications.inline-reply.button");
    }
    return i18n::tr("notifications.actions.fallback");
  }

  float measureHistoryActionsRowHeight(
      Renderer& renderer, const std::vector<std::string>& actions, float cardTextWidth, float scale
  ) {
    if (actions.empty()) {
      return 0.0F;
    }
    auto container = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
    });

    std::vector<std::unique_ptr<Button>> buttons;
    const std::size_t limit = std::min(actions.size(), kMaxNotificationActions * 2);
    for (std::size_t i = 0; i + 1 < limit; i += 2) {
      const std::string& actionKey = actions[i];
      if (actionKey.empty()) {
        continue;
      }
      buttons.push_back(
          ui::button({
              .text = historyActionLabel(actionKey, actions[i + 1]),
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Default,
          })
      );
    }

    auto rows = wrapButtonsIntoRows(renderer, buttons, cardTextWidth, Style::spaceXs * scale);
    populateRowContainer(*container, std::move(rows), cardTextWidth, Style::spaceXs * scale);

    if (container->children().empty()) {
      return 0.0F;
    }

    container->setSize(cardTextWidth, 0.0F);
    container->layout(renderer);
    return container->height();
  }
  constexpr int kSummaryMaxLines = 2;
  constexpr int kBodyMaxLines = 3;
  constexpr int kExpandedMaxLines = 500;

  std::filesystem::path remoteNotificationIconCachePath(std::string_view url) {
    return std::filesystem::path("/tmp")
        / "noctalia-notification-icons"
        / (std::to_string(std::hash<std::string_view>{}(url)) + ".img");
  }

  std::string normalizeLocalIconPath(std::string_view iconValue) { return uri::normalizeFileUrl(iconValue); }

  std::string resolveHistoryIconPath(const Notification& n, IconResolver& resolver, int targetSize) {
    if (!n.icon.has_value() || n.icon->empty()) {
      return {};
    }
    const std::string& iconValue = *n.icon;
    if (uri::isRemoteUrl(iconValue)) {
      const auto cached = remoteNotificationIconCachePath(iconValue);
      std::error_code ec;
      if (std::filesystem::exists(cached, ec) && std::filesystem::file_size(cached, ec) > 0) {
        return cached.string();
      }
      return {};
    }

    const std::string localPath = normalizeLocalIconPath(iconValue);
    if (!localPath.empty() && localPath.front() == '/') {
      if (access(localPath.c_str(), R_OK) == 0) {
        return localPath;
      }
      return {};
    }
    if (localPath.empty()) {
      return {};
    }

    const std::string& resolved = resolver.resolve(localPath, targetSize);
    return resolved.empty() ? std::string() : resolved;
  }

  std::string relativeMetaLine(const Notification& n) {
    if (n.receivedWallClock.has_value()) {
      return formatTimeAgo(*n.receivedWallClock);
    }
    return formatElapsedSince(n.receivedTime);
  }

  std::int64_t currentRelativeTimeSlot() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
        / 15;
  }

  bool matchesHistoryFilter(const NotificationHistoryEntry& e, std::size_t filterIndex) {
    if (filterIndex == 0) {
      return true;
    }
    if (!e.notification.receivedWallClock.has_value()) {
      return false;
    }
    const std::time_t entryT = WallClock::to_time_t(*e.notification.receivedWallClock);
    const std::time_t nowT = WallClock::to_time_t(WallClock::now());
    std::tm entryL{};
    std::tm nowL{};
    localtime_r(&entryT, &entryL);
    localtime_r(&nowT, &nowL);
    const bool isToday = entryL.tm_year == nowL.tm_year && entryL.tm_yday == nowL.tm_yday;
    std::tm yRef = nowL;
    yRef.tm_hour = 12;
    yRef.tm_min = 0;
    yRef.tm_sec = 0;
    yRef.tm_mday -= 1;
    mktime(&yRef);
    const bool isYesterday = entryL.tm_year == yRef.tm_year && entryL.tm_yday == yRef.tm_yday;

    if (filterIndex == 1) {
      return isToday;
    }
    if (filterIndex == 2) {
      return isYesterday;
    }
    return !isToday && !isYesterday;
  }

  float measuredTextHeight(
      Renderer& renderer, std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth, int maxLines
  ) {
    if (text.empty()) {
      return 0.0F;
    }
    const auto bounds = renderer.measureText(text, fontSize, fontWeight, maxWidth, maxLines);
    return std::max(0.0F, bounds.bottom - bounds.top);
  }

  bool canExpandText(
      Renderer& renderer, std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth,
      int collapsedMaxLines
  ) {
    if (text.empty()) {
      return false;
    }

    const float collapsedHeight = measuredTextHeight(renderer, text, fontSize, fontWeight, maxWidth, collapsedMaxLines);
    const float expandedHeight = measuredTextHeight(renderer, text, fontSize, fontWeight, maxWidth, kExpandedMaxLines);
    return expandedHeight > collapsedHeight + 0.5F;
  }

  struct NotificationCardMetrics {
    std::string summaryText;
    std::string bodyText;
    std::string metaLine;
    bool canExpand = false;
    bool expanded = false;
    float height = 0.0F;
    float cardTextWidth = 0.0F;
    float metaTextWidth = 0.0F;
  };

  NotificationCardMetrics measureNotificationCard(
      Renderer& renderer, const NotificationHistoryEntry& entry, float scale, float width, bool expandedRequested,
      bool showHistoryActions
  ) {
    NotificationCardMetrics metrics;
    const float cardWidth = std::max(0.0F, width);
    const float cardHorizontalPadding = Style::spaceMd * scale * 2.0F;
    metrics.cardTextWidth = std::max(0.0F, cardWidth - cardHorizontalPadding);
    const std::string summaryText = StringUtils::trimLeadingBlankLines(
        entry.notification.summary.empty() ? i18n::tr("control-center.notifications.untitled")
                                           : entry.notification.summary
    );
    const std::string bodyText = StringUtils::trimLeadingBlankLines(entry.notification.body);
    metrics.summaryText = summaryText;

    const bool summaryExpandable = canExpandText(
        renderer, summaryText, Style::fontSizeBody * scale, FontWeight::Bold, metrics.cardTextWidth, kSummaryMaxLines
    );
    bool bodyLineTruncated = false;
    const std::string collapsedBodyText = StringUtils::truncateByLines(bodyText, kBodyMaxLines, &bodyLineTruncated);
    const bool bodyExpandable = bodyLineTruncated
        || canExpandText(renderer, bodyText, Style::fontSizeBody * scale, FontWeight::Normal, metrics.cardTextWidth,
                         kBodyMaxLines);
    metrics.canExpand = summaryExpandable || bodyExpandable;
    metrics.expanded = metrics.canExpand && expandedRequested;
    metrics.bodyText = metrics.expanded ? bodyText : collapsedBodyText;

    const float iconPx = kHistoryIconSize * scale;
    const float iconColumn = iconPx + Style::spaceSm * scale;
    const float actionButtonSize = kNotificationActionButtonSize * scale;
    const float actionButtonsGap = Style::spaceXs * scale;
    const float headerActionsWidth =
        actionButtonSize + (metrics.canExpand ? (actionButtonsGap + actionButtonSize) : 0.0F);
    const float leftClusterWidth = metrics.cardTextWidth - headerActionsWidth;
    metrics.metaTextWidth = std::max(0.0F, leftClusterWidth - iconColumn);

    metrics.metaLine = notificationDisplayAppName(entry.notification) + " • " + relativeMetaLine(entry.notification);

    const float metaHeight = measuredTextHeight(
        renderer, metrics.metaLine, Style::fontSizeMini * scale, FontWeight::Normal, metrics.metaTextWidth, 0
    );
    const float headerHeight = std::max({iconPx, actionButtonSize, metaHeight});
    const float summaryHeight = measuredTextHeight(
        renderer, metrics.summaryText, Style::fontSizeBody * scale, FontWeight::Bold, metrics.cardTextWidth,
        metrics.expanded ? kExpandedMaxLines : kSummaryMaxLines
    );
    const float bodyHeight = metrics.bodyText.empty()
        ? 0.0F
        : measuredTextHeight(
              renderer, metrics.bodyText, Style::fontSizeBody * scale, FontWeight::Normal, metrics.cardTextWidth,
              metrics.expanded ? kExpandedMaxLines : kBodyMaxLines
          );

    const float actionsRowHeight = showHistoryActions
        ? measureHistoryActionsRowHeight(renderer, entry.notification.actions, metrics.cardTextWidth, scale)
        : 0.0F;

    const float paddingY = (Style::spaceSm + Style::spaceXs) * scale * 2.0F;
    int visibleSegments = 2;
    if (!metrics.bodyText.empty()) {
      ++visibleSegments;
    }
    if (actionsRowHeight > 0.5F) {
      ++visibleSegments;
    }
    const float gaps = Style::spaceSm * scale * static_cast<float>(std::max(0, visibleSegments - 1));
    metrics.height = paddingY + headerHeight + summaryHeight + bodyHeight + actionsRowHeight + gaps;
    return metrics;
  }

  std::uint64_t rawImageKey(const NotificationHistoryEntry& entry) {
    return (static_cast<std::uint64_t>(entry.notification.id) << 32U) ^ entry.eventSerial;
  }

  std::uint64_t revisionForEntry(const NotificationHistoryEntry& entry, bool expanded, std::int64_t relativeSlot) {
    std::uint64_t revision = entry.eventSerial;
    revision ^= static_cast<std::uint64_t>(relativeSlot < 0 ? 0 : relativeSlot) * 0x9E3779B185EBCA87ULL;
    if (expanded) {
      revision ^= 0xD1B54A32D192ED03ULL;
    }
    return revision;
  }

  class NotificationHistoryRow final : public Flex {
  public:
    explicit NotificationHistoryRow(float scale) : m_scale(scale) {
      applyContentCardStyle(*this, scale);
      setFillWidth(true);

      m_header = static_cast<Flex*>(addChild(
          ui::row({
              .align = FlexAlign::Center,
              .justify = FlexJustify::SpaceBetween,
              .gap = Style::spaceSm * scale,
          })
      ));

      m_leftCluster = static_cast<Flex*>(m_header->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
              .flexGrow = 1.0F,
          })
      ));

      m_iconSlot = static_cast<Box*>(m_leftCluster->addChild(
          ui::box({
              .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
              .radius = notificationIconRadius(kHistoryIconSize, scale),
              .width = kHistoryIconSize * scale,
              .height = kHistoryIconSize * scale,
          })
      ));

      m_image = static_cast<Image*>(m_iconSlot->addChild(
          ui::image({
              .visible = false,
          })
      ));

      m_fallback = static_cast<Glyph*>(m_iconSlot->addChild(
          ui::glyph({
              .glyph = "bell",
              .visible = false,
          })
      ));

      m_meta = static_cast<Label*>(m_leftCluster->addChild(
          ui::label({
              .fontSize = Style::fontSizeMini * scale,
              .flexGrow = 1.0F,
          })
      ));

      m_headerActions = static_cast<Flex*>(m_header->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * scale,
          })
      ));

      m_expand = static_cast<Button*>(
          m_headerActions->addChild(makeActionButton("chevron-down", scale, i18n::tr("control-center.notifications.expand")))
      );
      m_dismiss = static_cast<Button*>(
          m_headerActions->addChild(makeActionButton("trash", scale, i18n::tr("control-center.notifications.remove")))
      );

      m_summary = static_cast<Label*>(addChild(
          ui::label({
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
          })
      ));

      m_body = static_cast<Label*>(addChild(
          ui::label({
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      ));

      m_actionsRow = static_cast<Flex*>(addChild(
          ui::column({
              .align = FlexAlign::Stretch,
              .gap = Style::spaceXs * scale,
              .fillWidth = true,
              .visible = false,
          })
      ));
    }

    void bind(
        Renderer& renderer, const NotificationHistoryEntry& entry, float width, bool expanded, bool showHistoryActions,
        IconResolver& iconResolver, std::function<void(uint32_t)> onToggleExpanded,
        std::function<void(uint32_t, bool)> onRemove, const std::function<void(uint32_t, const std::string&)>& onAction
    ) {
      const NotificationCardMetrics metrics =
          measureNotificationCard(renderer, entry, m_scale, width, expanded, showHistoryActions);
      setMinWidth(width);
      setSize(width, metrics.height);

      const float iconPx = kHistoryIconSize * m_scale;
      m_iconSlot->setSize(iconPx, iconPx);

      bindIcon(renderer, entry, iconResolver);

      m_meta->setText(metrics.metaLine);
      m_meta->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_meta->setMaxWidth(metrics.metaTextWidth);
      m_meta->measure(renderer);

      m_expand->setVisible(metrics.canExpand);
      m_expand->setEnabled(metrics.canExpand);
      m_expand->setGlyph(metrics.expanded ? "chevron-up" : "chevron-down");
      m_expand->setTooltip(
          i18n::tr(metrics.expanded ? "control-center.notifications.collapse" : "control-center.notifications.expand")
      );
      m_expand->setOnClick([onToggleExpanded = std::move(onToggleExpanded), id = entry.notification.id]() {
        onToggleExpanded(id);
      });

      m_dismiss->setOnClick([onRemove = std::move(onRemove), id = entry.notification.id, active = entry.active]() {
        onRemove(id, active);
      });

      m_summary->setText(metrics.summaryText);
      m_summary->setMaxWidth(metrics.cardTextWidth);
      m_summary->setMaxLines(metrics.expanded ? kExpandedMaxLines : kSummaryMaxLines);
      m_summary->measure(renderer);

      if (metrics.bodyText.empty()) {
        m_body->setVisible(false);
        m_body->setText("");
      } else {
        m_body->setVisible(true);
        m_body->setText(metrics.bodyText);
        m_body->setMaxWidth(metrics.cardTextWidth);
        m_body->setMaxLines(metrics.expanded ? kExpandedMaxLines : kBodyMaxLines);
        m_body->measure(renderer);
      }

      while (!m_actionsRow->children().empty()) {
        m_actionsRow->removeChild(m_actionsRow->children().back().get());
      }
      m_actionsRow->setVisible(false);
      if (showHistoryActions && !entry.notification.actions.empty()) {
        std::vector<std::unique_ptr<Button>> buttons;
        const std::size_t limit = std::min(entry.notification.actions.size(), kMaxNotificationActions * 2);
        for (std::size_t i = 0; i + 1 < limit; i += 2) {
          const std::string& actionKey = entry.notification.actions[i];
          if (actionKey.empty()) {
            continue;
          }
          auto button = ui::button({
              .text = historyActionLabel(actionKey, entry.notification.actions[i + 1]),
              .fontSize = Style::fontSizeCaption * m_scale,
              .variant = ButtonVariant::Default,
          });
          button->setOnClick([onAction, id = entry.notification.id, key = std::string(actionKey)]() {
            onAction(id, key);
          });
          buttons.push_back(std::move(button));
        }

        auto rows = wrapButtonsIntoRows(renderer, buttons, metrics.cardTextWidth, Style::spaceXs * m_scale);
        populateRowContainer(*m_actionsRow, std::move(rows), metrics.cardTextWidth, Style::spaceXs * m_scale);

        m_actionsRow->setVisible(!m_actionsRow->children().empty());
      }
    }

  private:
    enum class ImageKind {
      None,
      File,
      Raw,
    };

    static std::unique_ptr<Button> makeActionButton(std::string_view glyph, float scale, std::string_view tooltip) {
      return ui::button({
          .glyph = std::string(glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::string(tooltip),
          .minWidth = kNotificationActionButtonSize * scale,
          .minHeight = kNotificationActionButtonSize * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
      });
    }

    void showFallbackIcon(Renderer& renderer) {
      if (m_imageKind != ImageKind::None) {
        m_image->clear(renderer);
      }
      m_imageKind = ImageKind::None;
      m_rawImageKey = 0;
      m_image->setVisible(false);

      const float iconPx = kHistoryIconSize * m_scale;
      m_fallback->setGlyph("bell");
      m_fallback->setGlyphSize(kHistoryIconGlyphSize * m_scale);
      m_fallback->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_fallback->measure(renderer);
      m_fallback->setPosition(
          std::round((iconPx - m_fallback->width()) * 0.5F), std::round((iconPx - m_fallback->height()) * 0.5F)
      );
      m_fallback->setVisible(true);
    }

    void bindIcon(Renderer& renderer, const NotificationHistoryEntry& entry, IconResolver& iconResolver) {
      const float iconPx = kHistoryIconSize * m_scale;
      const float iconRadius = notificationIconRadius(iconPx, m_scale);
      m_iconSlot->setRadius(iconRadius);
      m_image->setSize(iconPx, iconPx);
      m_image->setPosition(0.0F, 0.0F);
      m_image->setRadius(iconRadius);
      m_image->setFit(ImageFit::Cover);

      const int targetSize = static_cast<int>(std::round(iconPx));
      if (entry.notification.imageData.has_value()) {
        const auto& image = *entry.notification.imageData;
        if (image.width > 0 && image.height > 0 && !image.data.empty()) {
          const bool validImageMetadata = image.bitsPerSample == 8
              && ((image.channels == 4 && image.hasAlpha) || (image.channels == 3 && !image.hasAlpha));
          const PixmapFormat format = image.channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
          const std::uint64_t key = rawImageKey(entry);
          bool ready = m_imageKind == ImageKind::Raw && m_rawImageKey == key && m_image->hasImage();
          if (!ready && validImageMetadata) {
            ready = m_image->setSourceRaw(
                renderer, image.data.data(), image.data.size(), image.width, image.height, image.rowStride, format, true
            );
          }
          if (ready) {
            m_image->setForegroundTint(std::nullopt);
            m_image->setFit(ImageFit::Cover);
            m_image->setPadding(0.0F);
            m_imageKind = ImageKind::Raw;
            m_rawImageKey = key;
            m_image->setVisible(true);
            m_fallback->setVisible(false);
            return;
          }
        }
      }

      // Fallback: if no snapshot pixels are available, load from the live icon path.
      // This avoids showing stale HyprCap screenshots when the file gets overwritten later.
      const std::string iconPath = resolveHistoryIconPath(entry.notification, iconResolver, targetSize);
      if (!iconPath.empty()) {
        const bool ready = m_image->setSourceFile(renderer, iconPath, targetSize);
        if (ready) {
          // Symbolic theme icons ship as dark alpha masks; recolor them so they read on the card
          // instead of rendering as a dark square.
          const bool symbolic = iconPath.contains("symbolic");
          m_image->setForegroundTint(
              symbolic ? std::optional{colorSpecFromRole(ColorRole::OnSurfaceVariant)} : std::nullopt
          );
          // Symbolic glyphs are not square artwork; keep them whole and inset like the bell fallback.
          m_image->setFit(symbolic ? ImageFit::Contain : ImageFit::Cover);
          m_image->setPadding(symbolic ? std::round((kHistoryIconSize - kHistoryIconGlyphSize) * 0.5F * m_scale) : 0.0F);
          m_imageKind = ImageKind::File;
          m_rawImageKey = 0;
          m_image->setVisible(true);
          m_fallback->setVisible(false);
          return;
        }
      }

      showFallbackIcon(renderer);
    }

    float m_scale = 1.0F;
    Flex* m_header = nullptr;
    Flex* m_leftCluster = nullptr;
    Box* m_iconSlot = nullptr;
    Image* m_image = nullptr;
    Glyph* m_fallback = nullptr;
    Label* m_meta = nullptr;
    Flex* m_headerActions = nullptr;
    Button* m_expand = nullptr;
    Button* m_dismiss = nullptr;
    Label* m_summary = nullptr;
    Label* m_body = nullptr;
    Flex* m_actionsRow = nullptr;
    ImageKind m_imageKind = ImageKind::None;
    std::uint64_t m_rawImageKey = 0;
  };

} // namespace

class NotificationHistoryAdapter final : public VirtualListAdapter {
public:
  NotificationHistoryAdapter(NotificationsTab& owner, float scale) : m_owner(owner), m_scale(scale) {}

  [[nodiscard]] std::size_t itemCount() const override { return m_owner.m_filtered.size(); }

  [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override {
    if (index >= m_owner.m_filtered.size() || m_owner.m_filtered[index] == nullptr) {
      return static_cast<std::uint64_t>(index);
    }
    return m_owner.m_filtered[index]->notification.id;
  }

  [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override {
    if (index >= m_owner.m_filtered.size() || m_owner.m_filtered[index] == nullptr) {
      return 0;
    }
    const auto& entry = *m_owner.m_filtered[index];
    const bool expanded = m_owner.m_expandedIds.contains(entry.notification.id);
    std::uint64_t revision = revisionForEntry(entry, expanded, m_owner.m_lastRelativeTimeSlot);
    revision ^= static_cast<std::uint64_t>(Style::cornerRadiusScale() * 10000.0F) * 0xC2B2AE3D27D4EB4FULL;
    return revision;
  }

  [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override {
    if (index >= m_owner.m_filtered.size() || m_owner.m_filtered[index] == nullptr) {
      return 1.0F;
    }
    const auto& entry = *m_owner.m_filtered[index];
    const bool expanded = m_owner.m_expandedIds.contains(entry.notification.id);
    const bool showHistoryActions =
        m_owner.m_notifications != nullptr && m_owner.m_notifications->hasPendingDBusClose(entry.notification.id);
    return measureNotificationCard(renderer, entry, m_scale, width, expanded, showHistoryActions).height;
  }

  [[nodiscard]] std::unique_ptr<Node> createItem() override {
    return std::make_unique<NotificationHistoryRow>(m_scale);
  }

  void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool /*hovered*/) override {
    if (index >= m_owner.m_filtered.size() || m_owner.m_filtered[index] == nullptr) {
      return;
    }
    auto* row = dynamic_cast<NotificationHistoryRow*>(&item);
    if (row == nullptr) {
      return;
    }
    const auto& entry = *m_owner.m_filtered[index];
    const bool showHistoryActions =
        m_owner.m_notifications != nullptr && m_owner.m_notifications->hasPendingDBusClose(entry.notification.id);
    row->bind(
        renderer, entry, width, m_owner.m_expandedIds.contains(entry.notification.id), showHistoryActions,
        m_owner.m_iconResolver, [this](uint32_t id) { m_owner.toggleNotificationExpanded(id); },
        [this](uint32_t id, bool active) { m_owner.removeNotificationEntry(id, active); },
        [this](uint32_t id, const std::string& key) { m_owner.invokeNotificationAction(id, key); }
    );
  }

private:
  NotificationsTab& m_owner;
  float m_scale = 1.0F;
};

NotificationsTab::NotificationsTab(
    NotificationManager* notifications, CompositorPlatform* platform, const ControlCenterServices& services
)
    : m_notifications(notifications), m_platform(platform),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "notifications", .inlineSettings = true}
      )) {
  const auto groupPane = [&services](const char* group) {
    return std::make_unique<SettingsPane>(
        services,
        SettingsPaneOptions{.section = "notifications", .includedGroups = {group}, .inlineSettings = true}
    );
  };
  m_generalPane = groupPane("general");
  m_toastsPane = groupPane("toasts");
  m_historyPane = groupPane("history");
  m_filtersPane = groupPane("filtering");
}

NotificationsTab::~NotificationsTab() = default;

std::unique_ptr<Flex> NotificationsTab::create() {
  const float scale = contentScale();
  auto outer = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });
  auto subHeader = makeSubPaneHeader(scale, &m_subTitle, [this]() { openPage(Page::Root); });
  m_subHeader = subHeader.get();
  outer->addChild(std::move(subHeader));

  // Main page: live card, navigation rows into the settings sub-panes, then history.
  auto tab = ui::column({
      .out = &m_rootPage,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto liveCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        applyCompactCardStyle(card, scale, opacity, false);
      },
  });
  m_dndToggle = addToggleRow(
      *liveCard, scale, i18n::tr("control-center.notifications.dnd"),
      [this](bool checked) {
        if (m_syncingDnd || m_notifications == nullptr) {
          return;
        }
        m_notifications->setDoNotDisturb(checked);
        PanelManager::instance().refresh();
      }
  );
  auto historyRow = makeControlRow(i18n::tr("control-center.notifications.history"), scale, nullptr, true);
  historyRow->addChild(
      ui::button({
          .out = &m_clearAllButton,
          .glyph = "trash",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Destructive,
          .tooltip = i18n::tr("control-center.notifications.clear-all"),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .visible = false,
          .participatesInLayout = false,
          .onClick = [this]() { clearAllNotifications(); },
      })
  );
  historyRow->addChild(
      ui::segmented({
          .out = &m_filter,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = i18n::tr("control-center.notifications.filter.all"),
                      .tooltip = i18n::tr("control-center.notifications.filter.all-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.notifications.filter.today"),
                      .tooltip = i18n::tr("control-center.notifications.filter.today-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.notifications.filter.yesterday"),
                      .tooltip = i18n::tr("control-center.notifications.filter.yesterday-tooltip"),
                  },
                  {
                      .label = i18n::tr("control-center.notifications.filter.older"),
                      .tooltip = i18n::tr("control-center.notifications.filter.older-tooltip"),
                  },
              },
          .selectedIndex = m_filterIndex,
          .fontSize = Style::fontSizeCaption,
          .scale = scale,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .flexGrow = 1.0F,
          .onChange =
              [this](std::size_t idx) {
                if (idx == m_filterIndex) {
                  return;
                }

                cancelFilterSlide();
                AnimationManager* animations = m_list != nullptr ? m_list->animationManager() : nullptr;
                if (animations == nullptr || m_list == nullptr) {
                  m_filterIndex = idx;
                  m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
                  if (m_list != nullptr) {
                    m_list->scrollView().setScrollOffset(0.0F);
                  }
                  requestPanelLayout();
                  return;
                }

                beginFilterSlideOut(idx);
              },
      })
  );
  liveCard->addChild(std::move(historyRow));
  tab->addChild(std::move(liveCard));
  syncDndToggle();

  {
    auto nav = ui::column({
        .align = FlexAlign::Stretch,
        .configure = [scale](Flex& list) { applyGroupedListStyle(list, scale); },
    });
    const auto addNav = [&](const char* glyph, const char* titleKey, Page page) {
      if (!nav->children().empty()) {
        nav->addChild(ui::separator());
      }
      nav->addChild(makeNavRow(scale, glyph, i18n::tr(titleKey), [this, page]() { openPage(page); }));
    };
    addNav("bell", "control-center.notifications.pages.general", Page::General);
    addNav("message", "control-center.notifications.pages.toasts", Page::Toasts);
    addNav("history", "control-center.notifications.pages.history", Page::History);
    addNav("filter", "control-center.notifications.pages.filters", Page::Filters);
    tab->addChild(std::move(nav));
  }

  m_adapter = std::make_unique<NotificationHistoryAdapter>(*this, scale);

  tab->addChild(
      ui::virtualListView({
          .out = &m_list,
          .contentScale = scale,
          .itemGap = Style::spaceMd * scale,
          .overscanItems = 3,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0F,
          .configure =
              [](VirtualListView& list) {
                list.setFillWidth(true);
                list.setFillHeight(true);
              },
      })
  );

  {
    control_center::EmptyStateParts parts;
    auto empty = control_center::makeEmptyState(
        scale,
        {
            .glyph = "bell-off",
            .title = i18n::tr("control-center.notifications.empty-title"),
            .body = i18n::tr("control-center.notifications.empty-body"),
        },
        &parts
    );
    m_emptyCard = empty.get();
    m_emptyTitle = parts.title;
    m_emptyBody = parts.body;
    m_emptyGlyph = parts.glyph;
    applyContentCardStyle(*empty, scale);
    empty->setAlign(FlexAlign::Center);
    empty->setJustify(FlexJustify::Center);
    empty->setVisible(false);
    empty->setParticipatesInLayout(false);
    tab->addChild(std::move(empty));
  }
  outer->addChild(std::move(tab));

  // Settings sub-panes, one group each, in one scroll.
  auto subScroll = ui::scrollView({
      .out = &m_subScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& view) {
            view.clearFill();
            view.clearBorder();
          },
  });
  Flex* subContent = subScroll->content();
  subContent->setDirection(FlexDirection::Vertical);
  subContent->setAlign(FlexAlign::Stretch);
  const auto addPage = [&](SettingsPane& pane, Flex** out) {
    auto page = pane.createInline(scale, panelCardOpacity());
    *out = page.get();
    subContent->addChild(std::move(page));
  };
  addPage(*m_generalPane, &m_generalPage);
  addPage(*m_toastsPane, &m_toastsPage);
  addPage(*m_historyPane, &m_historyPage);
  addPage(*m_filtersPane, &m_filtersPage);
  outer->addChild(std::move(subScroll));
  syncPage();

  return m_settings->wrapLive(std::move(outer), contentScale(), panelCardOpacity());
}

std::unique_ptr<Flex> NotificationsTab::createHeaderActions() {
  return m_settings->embedHeader(nullptr, contentScale());
}

void NotificationsTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool NotificationsTab::configureMode() const { return m_settings->configureMode(); }

void NotificationsTab::setSettingsSearchQuery(std::string_view query) {
  m_settings->setSearchQuery(std::string(query));
}

SettingsPane* NotificationsTab::settingsPane() { return m_settings.get(); }

void NotificationsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_root == nullptr || m_filter == nullptr) {
    return;
  }

  if (!filterSlideOutActive()) {
    refreshDataSnapshot();
  }
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);

  if (m_list != nullptr && m_filterSlideAnimId == 0 && !m_startFilterSlideIn) {
    m_filterSlideBaseX = m_list->x();
    m_filterSlideBaseY = m_list->y();
  }

  if (m_startFilterSlideIn && m_list != nullptr) {
    m_startFilterSlideIn = false;
    beginFilterSlideIn();
  }
}

void NotificationsTab::doUpdate(Renderer& renderer) {
  if (filterSlideOutActive()) {
    return;
  }
  if (refreshDataSnapshot() && m_root != nullptr) {
    m_root->layout(renderer);
  }
}

void NotificationsTab::syncPage() {
  const auto shown = [](Node* node, bool visible) {
    if (node != nullptr && (node->visible() != visible || node->participatesInLayout() != visible)) {
      node->setVisible(visible);
      node->setParticipatesInLayout(visible);
    }
  };
  const bool root = m_page == Page::Root;
  shown(m_rootPage, root);
  shown(m_subHeader, !root);
  shown(m_subScroll, !root);
  shown(m_generalPage, m_page == Page::General);
  shown(m_toastsPage, m_page == Page::Toasts);
  shown(m_historyPage, m_page == Page::History);
  shown(m_filtersPage, m_page == Page::Filters);
  if (m_subTitle != nullptr && !root) {
    const char* key = "control-center.notifications.pages.general";
    if (m_page == Page::Toasts) {
      key = "control-center.notifications.pages.toasts";
    } else if (m_page == Page::History) {
      key = "control-center.notifications.pages.history";
    } else if (m_page == Page::Filters) {
      key = "control-center.notifications.pages.filters";
    }
    m_subTitle->setText(i18n::tr(key));
  }
  PanelManager::instance().requestLayout();
}

void NotificationsTab::openPage(Page page) {
  if (page == m_page) {
    return;
  }
  m_page = page;
  if (m_subScroll != nullptr) {
    m_subScroll->setScrollOffset(0.0F);
  }
  syncPage();
}

void NotificationsTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (becameActive && m_page != Page::Root) {
    m_page = Page::Root;
    syncPage();
  }
}

bool NotificationsTab::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  // Cancel (Escape by default) backs out of a sub-pane before it would close the panel.
  if (!pressed || preedit || configureMode() || m_page == Page::Root
      || !KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    return false;
  }
  openPage(Page::Root);
  return true;
}

void NotificationsTab::onClose() {
  m_settings->onClose();
  m_generalPane->onClose();
  m_toastsPane->onClose();
  m_historyPane->onClose();
  m_filtersPane->onClose();
  m_rootPage = nullptr;
  m_subHeader = nullptr;
  m_subTitle = nullptr;
  m_subScroll = nullptr;
  m_generalPage = nullptr;
  m_toastsPage = nullptr;
  m_historyPage = nullptr;
  m_filtersPage = nullptr;
  cancelFilterSlide();
  if (m_list != nullptr) {
    m_list->setAdapter(nullptr);
  }
  m_root = nullptr;
  m_list = nullptr;
  m_emptyCard = nullptr;
  m_emptyTitle = nullptr;
  m_emptyBody = nullptr;
  m_emptyGlyph = nullptr;
  m_filter = nullptr;
  m_clearAllButton = nullptr;
  m_dndToggle = nullptr;
  m_adapter.reset();
  m_filtered.clear();
  m_expandedIds.clear();
  m_lastSerial = 0;
  m_lastRelativeTimeSlot = -1;
  m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
  m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
  m_startFilterSlideIn = false;
  m_filterSlideDirection = 0;
  m_filterSlideBaseX = 0.0F;
  m_filterSlideBaseY = 0.0F;
}

void NotificationsTab::cancelFilterSlide() {
  if (m_filterSlideAnimId != 0 && m_list != nullptr) {
    if (AnimationManager* animations = m_list->animationManager(); animations != nullptr) {
      animations->cancel(m_filterSlideAnimId);
    }
    m_filterSlideAnimId = 0;
  }
  m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
  m_startFilterSlideIn = false;
  m_filterSlideDirection = 0;
  if (m_list != nullptr) {
    m_list->setPosition(m_filterSlideBaseX, m_filterSlideBaseY);
    m_list->setOpacity(1.0F);
  }
}

bool NotificationsTab::filterSlideOutActive() const { return m_filterSlideAnimId != 0 && !m_startFilterSlideIn; }

void NotificationsTab::applyFilterSlide(float progress, bool slidingIn) {
  if (m_list == nullptr || m_root == nullptr) {
    return;
  }

  const float travel = m_root->width();
  if (travel <= 0.0F) {
    return;
  }

  const auto direction = static_cast<float>(m_filterSlideDirection);
  if (slidingIn) {
    m_list->setPosition(m_filterSlideBaseX + direction * travel * (1.0F - progress), m_filterSlideBaseY);
    m_list->setOpacity(0.7F + 0.3F * progress);
  } else {
    m_list->setPosition(m_filterSlideBaseX - direction * travel * progress, m_filterSlideBaseY);
    m_list->setOpacity(1.0F - 0.3F * progress);
  }
}

void NotificationsTab::beginFilterSlideOut(std::size_t nextIndex) {
  AnimationManager* animations = m_list != nullptr ? m_list->animationManager() : nullptr;
  if (animations == nullptr || m_list == nullptr) {
    m_filterIndex = nextIndex;
    m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
    if (m_list != nullptr) {
      m_list->scrollView().setScrollOffset(0.0F);
    }
    requestPanelLayout();
    return;
  }

  m_pendingFilterIndex = nextIndex;
  m_filterSlideDirection = nextIndex > m_filterIndex ? 1 : -1;
  m_filterSlideBaseX = m_list->x();
  m_filterSlideBaseY = m_list->y();

  PanelManager::instance().requestFrameTick();
  m_filterSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyFilterSlide(progress, false);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_filterSlideAnimId = 0;
        if (m_pendingFilterIndex != std::numeric_limits<std::size_t>::max()) {
          m_filterIndex = m_pendingFilterIndex;
        }
        m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
        m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
        if (m_list != nullptr) {
          m_list->scrollView().setScrollOffset(0.0F);
        }
        m_startFilterSlideIn = true;
        requestPanelLayout();
      },
      m_list
  );
}

void NotificationsTab::beginFilterSlideIn() {
  AnimationManager* animations = m_list != nullptr ? m_list->animationManager() : nullptr;
  if (animations == nullptr || m_list == nullptr) {
    return;
  }

  m_filterSlideBaseX = m_list->x();
  m_filterSlideBaseY = m_list->y();

  applyFilterSlide(0.0F, true);
  PanelManager::instance().requestFrameTick();
  m_filterSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyFilterSlide(progress, true);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_filterSlideAnimId = 0;
        if (m_list != nullptr) {
          m_list->setPosition(m_filterSlideBaseX, m_filterSlideBaseY);
          m_list->setOpacity(1.0F);
        }
      },
      m_list
  );
}

void NotificationsTab::clearAllNotifications() {
  if (m_notifications == nullptr) {
    return;
  }

  std::vector<uint32_t> activeIds;
  activeIds.reserve(m_notifications->all().size());
  for (const auto& notification : m_notifications->all()) {
    activeIds.push_back(notification.id);
  }
  for (const uint32_t id : activeIds) {
    (void)m_notifications->close(id, CloseReason::Dismissed);
  }
  m_notifications->clearHistory();
  m_expandedIds.clear();
  m_lastSerial = 0;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  auto* panel = dynamic_cast<ControlCenterPanel*>(PanelManager::instance().activePanel());
  if (panel != nullptr && !panel->showsSidebar()) {
    PanelManager::instance().close();
  } else {
    PanelManager::instance().refresh();
  }
}

void NotificationsTab::removeNotificationEntry(uint32_t id, bool wasActive) {
  if (m_notifications == nullptr) {
    return;
  }

  if (wasActive) {
    (void)m_notifications->close(id, CloseReason::Dismissed);
  }
  m_notifications->removeHistoryEntry(id);
  m_expandedIds.erase(id);
  m_lastSerial = 0;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::toggleNotificationExpanded(uint32_t id) {
  if (m_expandedIds.contains(id)) {
    m_expandedIds.erase(id);
  } else {
    m_expandedIds.insert(id);
  }

  if (m_list != nullptr) {
    if (const auto index = filteredIndexForId(id); index.has_value()) {
      m_list->notifyItemChanged(*index);
    } else {
      m_list->notifyDataChanged();
    }
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::invokeNotificationAction(uint32_t id, const std::string& actionKey) {
  if (m_notifications == nullptr || actionKey.empty() || !m_notifications->hasPendingDBusClose(id)) {
    return;
  }
  const std::string activationToken =
      m_platform != nullptr ? m_platform->requestActivationToken(m_platform->lastPointerSurface()) : std::string{};
  if (!m_notifications->invokeAction(id, actionKey, activationToken, true)) {
    kLog.warn("notification history: failed to invoke action '{}' for #{}", actionKey, id);
    return;
  }

  m_lastSerial = 0;
  if (m_list != nullptr) {
    if (const auto index = filteredIndexForId(id); index.has_value()) {
      m_list->notifyItemChanged(*index);
    } else {
      m_list->notifyDataChanged();
    }
  }
  PanelManager::instance().refresh();
}

bool NotificationsTab::refreshDataSnapshot() {
  const bool hasHistory = m_notifications != nullptr && !m_notifications->history().empty();
  if (setShown(m_clearAllButton, hasHistory)) {
    requestPanelLayout();
  }
  syncDndToggle();

  const std::uint64_t serial = m_notifications != nullptr ? m_notifications->changeSerial() : 0;
  const std::int64_t relativeSlot = currentRelativeTimeSlot();
  const bool changed =
      serial != m_lastSerial || relativeSlot != m_lastRelativeTimeSlot || m_filterIndex != m_lastRebuildFilterIndex;
  if (!changed) {
    updateEmptyState(hasHistory, !m_filtered.empty());
    return false;
  }

  m_filtered.clear();
  if (m_notifications != nullptr) {
    m_filtered.reserve(m_notifications->history().size());
    for (const auto& historyEntry : std::views::reverse(m_notifications->history())) {
      if (matchesHistoryFilter(historyEntry, m_filterIndex)) {
        m_filtered.push_back(&historyEntry);
      }
    }
  }

  m_lastSerial = serial;
  m_lastRelativeTimeSlot = relativeSlot;
  m_lastRebuildFilterIndex = m_filterIndex;

  updateEmptyState(hasHistory, !m_filtered.empty());
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  return true;
}

void NotificationsTab::syncDndToggle() {
  if (m_dndToggle == nullptr) {
    return;
  }

  m_dndToggle->setEnabled(m_notifications != nullptr);
  m_syncingDnd = true;
  m_dndToggle->setCheckedImmediate(m_notifications != nullptr && m_notifications->doNotDisturb());
  m_syncingDnd = false;
}

void NotificationsTab::updateEmptyState(bool hasHistory, bool hasFiltered) {
  bool layoutDirty = false;
  layoutDirty |= setShown(m_list, hasFiltered);
  layoutDirty |= setShown(m_emptyCard, !hasFiltered);
  if (layoutDirty) {
    requestPanelLayout();
  }

  if (m_emptyTitle == nullptr || m_emptyBody == nullptr) {
    return;
  }

  if (hasHistory) {
    m_emptyTitle->setText(i18n::tr("control-center.notifications.filter-empty-title"));
    m_emptyBody->setText(i18n::tr("control-center.notifications.filter-empty-body"));
    if (m_emptyGlyph != nullptr) {
      m_emptyGlyph->setGlyph("filter");
    }
  } else {
    m_emptyTitle->setText(i18n::tr("control-center.notifications.empty-title"));
    m_emptyBody->setText(i18n::tr("control-center.notifications.empty-body"));
    if (m_emptyGlyph != nullptr) {
      m_emptyGlyph->setGlyph("bell-off");
    }
  }
}

std::optional<std::size_t> NotificationsTab::filteredIndexForId(uint32_t id) const {
  for (std::size_t i = 0; i < m_filtered.size(); ++i) {
    if (m_filtered[i] != nullptr && m_filtered[i]->notification.id == id) {
      return i;
    }
  }
  return std::nullopt;
}

void NotificationsTab::onPanelCardOpacityChanged(float opacity) {
  if (m_filter != nullptr) {
    m_filter->setSurfaceOpacity(opacity);
  }
}
