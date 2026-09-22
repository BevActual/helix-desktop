#pragma once

// Helix Phone mirror: a shell-owned xdg toplevel that shows the iPhone's AirPlay
// picture and forwards pointer/keyboard input to the Helix Phone BLE HID script.
//
// UxPlay decodes the stream and writes fixed-size RGBA frames to a FIFO; a reader
// thread here turns them into one GL texture. Input arrives through Wayland on this
// surface (no Xwayland grabs) and goes out over the HID control socket.

#include "core/timer_manager.h"
#include "wayland/wayland_seat.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Flex;
class Image;
class Label;
class RenderContext;
class ToplevelSurface;
class WaylandConnection;
struct wl_surface;

// Fixed frame geometry UxPlay is asked to deliver into the FIFO (19.5:9, letterboxed).
inline constexpr int kPhoneMirrorFrameWidth = 720;
inline constexpr int kPhoneMirrorFrameHeight = 1560;

[[nodiscard]] std::filesystem::path phoneMirrorVideoPath();
[[nodiscard]] std::filesystem::path phonePointerSocketPath();

// What the window tells the user while it waits, and once the picture is up.
struct PhoneMirrorStatus {
  bool pointerRunning = false;   // helix-phone-hid.py answers
  bool pointerConnected = false; // iPhone subscribed to input reports
  bool pointerBondLost = false;  // iPhone bonded before, not connecting now

  bool operator==(const PhoneMirrorStatus&) const = default;
};

class PhoneMirrorWindow {
public:
  PhoneMirrorWindow(WaylandConnection& wayland, RenderContext& renderContext);
  ~PhoneMirrorWindow();

  PhoneMirrorWindow(const PhoneMirrorWindow&) = delete;
  PhoneMirrorWindow& operator=(const PhoneMirrorWindow&) = delete;

  // Creates the FIFO (if needed), starts the frame reader, and maps the window.
  // Receiver side: holds the video FIFO open and drains frames whether or not a window
  // shows them, so UxPlay never blocks on a missing reader while it idles headless.
  void startReceiving();
  void stopReceiving();
  [[nodiscard]] bool receiving() const noexcept { return m_reader.joinable(); }
  // Window side: the surface only. open() also starts receiving if needed.
  void open();
  void close();
  // Fires after the compositor closes the window (user action), once it is torn down.
  void setClosedByUserCallback(std::function<void()> callback) { m_closedByUser = std::move(callback); }
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }
  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  void setStatus(const PhoneMirrorStatus& status);

  // Event routing from Application. Return true when the event belonged to this window.
  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);

private:
  class PointerLink;

  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(float scale);
  void applyStatus();
  void uploadFrame();
  void readerLoop();
  void handleFrameReady();
  void queuePointer(double sx, double sy, bool urgent);
  void queueScroll(double notches);
  void scheduleFlush(bool urgent);
  void flushPointer();

  WaylandConnection& m_wayland;
  RenderContext& m_renderContext;
  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<Flex> m_root;
  Image* m_image = nullptr;
  // Waiting overlay (no picture yet) and the pairing strip under the picture.
  Flex* m_waiting = nullptr;
  Label* m_waitingTitle = nullptr;
  Label* m_waitingMirrorStep = nullptr;
  Label* m_waitingPairStep = nullptr;
  Flex* m_strip = nullptr;
  Label* m_stripText = nullptr;
  PhoneMirrorStatus m_status;
  std::unique_ptr<PointerLink> m_link;
  std::function<void()> m_closedByUser;

  // Frame transport.
  std::thread m_reader;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_updatePosted{false};
  std::mutex m_frameMutex;
  std::vector<std::uint8_t> m_readyFrame;
  bool m_frameReady = false;
  bool m_haveFrame = false;
  struct TextureState;
  std::unique_ptr<TextureState> m_texture;

  // Input state. Motion and wheel are coalesced and flushed on m_flushTimer.
  std::uint32_t m_buttons = 0;
  double m_pendingX = 0.0;
  double m_pendingY = 0.0;
  bool m_pendingPointer = false;
  double m_scrollRemainder = 0.0;
  Timer m_flushTimer;
  std::chrono::steady_clock::time_point m_lastFlushAt{};
  bool m_pointerInside = false;
  // Edge gestures: the press position is snapped to the edge once per drag so iOS
  // sees a real edge swipe (bottom Home, left Back, top Control Center/Notifications).
  bool m_edgeSnapped = false;
  std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
};
