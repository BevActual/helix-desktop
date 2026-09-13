#pragma once

#include "capture/annotation_overlay.h"
#include "capture/screenshot_capture.h"
#include "capture/screenshot_region_overlay.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class CompositorPlatform;
class ConfigService;
class IpcService;
class NotificationManager;
struct Config;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;

class ScreenshotService {
public:
  enum class ScanKind : std::uint8_t { Ocr, Qr };

  struct OutputOptions {
    bool saveToFile = true;
    bool copyToClipboard = false;
    bool pipeToCommand = false;
    bool freezeScreen = false;
    bool confirmRegion = false;
    bool rememberLastRegion = false;
    bool showCursor = false;
    bool annotate = false;
    std::optional<ScanKind> scan;
    std::string pipeCommand;
    std::string directory;
    std::string filenamePattern;
  };

  ScreenshotService(
      WaylandConnection& wayland, CompositorPlatform& platform, ConfigService& configService,
      NotificationManager& notifications, ClipboardService* clipboard = nullptr
  );
  ~ScreenshotService();

  [[nodiscard]] bool available() const noexcept;

  void captureFullscreen(const OutputOptions& options, wl_output* output = nullptr);
  void captureFullscreenInteractive(RenderContext& renderContext, const OutputOptions& options);
  void beginRegionCapture(RenderContext& renderContext, const OutputOptions& options);
  void beginFullscreenCapture(RenderContext& renderContext, const OutputOptions& options);
  void beginColorPick(RenderContext& renderContext);
  [[nodiscard]] bool hasOcr() const;
  [[nodiscard]] bool hasQrScan() const;
  void beginOcr(RenderContext& renderContext);
  void beginQrScan(RenderContext& renderContext);
  // Interactive region overlay without capturing a PNG. Used by screen recording.
  using RegionPickCallback = std::function<void(std::optional<LogicalRect>)>;
  void beginRegionPick(RenderContext& renderContext, RegionPickCallback callback);
  // freezeFirst selects the frozen annotator (screenshot-annotate); otherwise the overlay
  // starts transparent over the running desktop (annotate).
  void beginAnnotation(RenderContext& renderContext, const OutputOptions& options, bool freezeFirst);
  // Opens the annotator on an image decoded from disk, with no screencopy involved.
  // Returns the reason when the file cannot be decoded or no output can host the editor.
  [[nodiscard]] std::expected<void, std::string>
  beginImageFileAnnotation(RenderContext& renderContext, const std::string& path, const OutputOptions& options);
  [[nodiscard]] bool overlayBusy() const noexcept;

  void onOutputChange();

  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  [[nodiscard]] bool onKeyboardEvent(const KeyboardEvent& event);

  [[nodiscard]] static OutputOptions outputOptionsFromConfig(const Config& config);

  void registerIpc(IpcService& ipc, const ConfigService& configService);

private:
  struct PendingCapture {
    wl_output* output = nullptr;
    std::optional<LogicalRect> region;
    OutputOptions outputOptions{};
    std::optional<std::filesystem::path> destPath;
  };

  struct AllOutputCaptureTarget {
    wl_output* output = nullptr;
    std::string label;
  };

  struct AllOutputsBatch {
    OutputOptions options{};
    std::vector<AllOutputCaptureTarget> targets;
    std::vector<capture::FrozenScreenshot> frames;
    std::size_t next = 0;
  };

  struct GlobalRegionCaptureTarget {
    wl_output* output = nullptr;
    LogicalRect localRegion{};
  };

  struct GlobalRegionBatch {
    OutputOptions options{};
    LogicalRect globalRegion{};
    std::vector<GlobalRegionCaptureTarget> targets;
    struct Piece {
      wl_output* output = nullptr;
      LogicalRect localRegion{};
      capture::ScreenshotImage image;
    };
    std::vector<Piece> pieces;
    std::size_t next = 0;
  };

  enum class FreezeTarget : std::uint8_t { Region, Annotation, ColorPick };

  struct FreezeRequest {
    wl_output* output = nullptr;
  };

  // Delivery policy captured before the annotator opened, applied when the user hits Done.
  struct PendingDelivery {
    OutputOptions options{};
    std::optional<std::filesystem::path> destPath;
  };

  void captureOutput(
      wl_output* output, std::optional<LogicalRect> region, const std::string& labelBase, const OutputOptions& options,
      int pathSuffix = 0
  );
  void ensureRegionOverlay();
  void startRegionOverlay(RenderContext& renderContext);
  void startFullscreenOverlay(RenderContext& renderContext);
  void beginFreezeCapture();
  void startNextFreezeCapture();
  void
  onFreezeFrameCaptured(FreezeRequest request, std::optional<capture::ScreenshotImage> image, const std::string& error);
  void finishFreezeCapture();
  void abortFreezeCapture(const std::string& message);
  void cancelRegionCapture();
  void cancelPendingRegionPick();
  void finishRegionPick(std::optional<LogicalRect> region);
  void ensureAnnotationOverlay();
  void beginImageAnnotation(
      capture::ScreenshotImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath
  );
  [[nodiscard]] capture::AnnotationToolState loadAnnotationToolState() const;
  void persistAnnotationToolState(std::string_view key, std::string_view value);
  void deliverFrozenRegion(LogicalRect region, wl_output* output, const OutputOptions& options);
  void deliverFrozenGlobalRegion(LogicalRect globalRegion, const OutputOptions& options);
  void captureGlobalRegion(LogicalRect globalRegion, const OutputOptions& options);
  void completeFullscreenSelection(wl_output* output, const OutputOptions& options);
  void startNextGlobalRegionCapture();
  void onGlobalRegionFrameCaptured(
      wl_output* output, LogicalRect localRegion, std::optional<capture::ScreenshotImage> image,
      const std::string& error
  );
  void finishGlobalRegionBatch();
  void cancelGlobalRegionBatch();
  void startNextQueuedCapture();
  void captureAllOutputs(const OutputOptions& options);
  void startNextAllOutputsCapture();
  void onAllOutputsFrameCaptured(
      wl_output* output, const std::string& label, std::optional<capture::ScreenshotImage> image,
      const std::string& error
  );
  void finishAllOutputsBatch();
  void cancelAllOutputsBatch();
  void deliverCaptureResult(
      capture::ScreenshotImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath
  );
  bool
  finishDelivery(ScreencopyImage image, const OutputOptions& options, std::optional<std::filesystem::path> destPath);
  void beginScan(RenderContext& renderContext, ScanKind kind);
  void startTextScan(ScreencopyImage image, ScanKind kind);
  void onCaptureComplete(
      std::optional<capture::ScreenshotImage> image, const std::string& error, OutputOptions options,
      std::optional<std::filesystem::path> destPath
  );
  [[nodiscard]] wl_output* preferredCaptureOutput() const;
  [[nodiscard]] std::filesystem::path outputDirectory(const OutputOptions& options) const;
  [[nodiscard]] std::filesystem::path
  makeScreenshotPath(const OutputOptions& options, const std::string& labelBase, int suffix = 0) const;
  void notifySaved(const std::filesystem::path& path);
  void notifyError(const std::string& message);
  void rememberRegion(const LogicalRect& region);
  [[nodiscard]] std::optional<LogicalRect> loadRememberedRegion() const;

  WaylandConnection& m_wayland;
  CompositorPlatform& m_platform;
  NotificationManager& m_notifications;
  ConfigService& m_configService;
  ClipboardService* m_clipboard = nullptr;
  ScreenshotCapture m_capture;
  std::unique_ptr<capture::ScreenshotRegionOverlay> m_regionOverlay;
  std::vector<PendingCapture> m_captureQueue;
  std::unique_ptr<AllOutputsBatch> m_allOutputsBatch;
  std::unique_ptr<GlobalRegionBatch> m_globalRegionBatch;
  OutputOptions m_regionOutputOptions{};
  RenderContext* m_regionRenderContext = nullptr;
  bool m_regionFullscreenPick = false;
  RegionPickCallback m_regionPickCallback;
  std::vector<capture::FrozenScreenshot> m_frozenScreenshots;
  std::unique_ptr<capture::AnnotationOverlay> m_annotationOverlay;
  std::vector<FreezeRequest> m_pendingFreezeCaptures;
  std::optional<PendingDelivery> m_pendingDelivery;
  FreezeTarget m_freezeTarget = FreezeTarget::Region;
  bool m_freezeCaptureActive = false;
  std::uint64_t m_scanGeneration = 0;
};
