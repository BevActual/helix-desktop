#include "shell/phone_mirror/phone_mirror_window.h"

#include "compositors/niri/niri_phone_cast.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_handle.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("phone-mirror");
  // Roughly one BLE connection interval for an iOS HID peripheral.
  constexpr std::chrono::milliseconds kPointerFlushInterval{8};
  constexpr std::size_t kFrameBytes =
      static_cast<std::size_t>(kPhoneMirrorFrameWidth) * static_cast<std::size_t>(kPhoneMirrorFrameHeight) * 4U;

  std::filesystem::path runtimeDir() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return std::filesystem::path(runtime != nullptr && runtime[0] != '\0' ? runtime : "/tmp");
  }

} // namespace

std::filesystem::path phoneMirrorVideoPath() { return runtimeDir() / "helix-phone-video.fifo"; }
std::filesystem::path phonePointerSocketPath() { return runtimeDir() / "helix-phone-hid.sock"; }

struct PhoneMirrorWindow::TextureState {
  TextureHandle handle;
  bool bound = false;
};

// Persistent, non-blocking client for the HID control socket. Motion is fire-and-forget;
// replies are drained and dropped so the script never blocks on its write side.
class PhoneMirrorWindow::PointerLink {
public:
  ~PointerLink() { disconnect(); }

  bool send(const nlohmann::json& message) {
    if (!ensureConnected()) {
      return false;
    }
    drain();
    const std::string line = message.dump() + "\n";
    const ssize_t sent = ::send(m_fd, line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent == static_cast<ssize_t>(line.size())) {
      return true;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Socket full: drop this event; the next motion report supersedes it.
      return false;
    }
    disconnect();
    return false;
  }

private:
  bool ensureConnected() {
    if (m_fd >= 0) {
      return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastAttempt < std::chrono::milliseconds{500}) {
      return false;
    }
    m_lastAttempt = now;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = phonePointerSocketPath().string();
    if (path.size() >= sizeof(address.sun_path)) {
      ::close(fd);
      return false;
    }
    std::copy(path.begin(), path.end(), address.sun_path);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 && errno != EINPROGRESS) {
      ::close(fd);
      return false;
    }
    m_fd = fd;
    return true;
  }

  void drain() {
    char buffer[1024];
    while (::recv(m_fd, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {
    }
  }

  void disconnect() {
    if (m_fd >= 0) {
      ::close(m_fd);
      m_fd = -1;
    }
  }

  int m_fd = -1;
  std::chrono::steady_clock::time_point m_lastAttempt{};
};

PhoneMirrorWindow::PhoneMirrorWindow(WaylandConnection& wayland, RenderContext& renderContext)
    : m_wayland(wayland), m_renderContext(renderContext), m_link(std::make_unique<PointerLink>()),
      m_texture(std::make_unique<TextureState>()) {}

PhoneMirrorWindow::~PhoneMirrorWindow() {
  close();
  stopReceiving();
}

wl_surface* PhoneMirrorWindow::wlSurface() const noexcept {
  return m_surface != nullptr ? m_surface->wlSurface() : nullptr;
}

void PhoneMirrorWindow::startReceiving() {
  if (m_reader.joinable()) {
    return;
  }
  const auto fifo = phoneMirrorVideoPath();
  std::error_code ec;
  if (!std::filesystem::exists(fifo, ec) || !std::filesystem::is_fifo(fifo, ec)) {
    std::filesystem::remove(fifo, ec);
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
      kLog.warn("could not create {}: {}", fifo.string(), std::strerror(errno));
      return;
    }
  }
  m_stop = false;
  m_frameReady = false;
  m_reader = std::thread([this]() { readerLoop(); });
}

void PhoneMirrorWindow::stopReceiving() {
  m_stop = true;
  if (m_reader.joinable()) {
    m_reader.join();
  }
  std::lock_guard lock(m_frameMutex);
  m_frameReady = false;
  m_updatePosted = false;
}

void PhoneMirrorWindow::open() {
  if (m_surface != nullptr) {
    return;
  }
  startReceiving();

  auto surface = std::make_unique<ToplevelSurface>(m_wayland);
  surface->setRenderContext(&m_renderContext);
  surface->setDebugName("phone-mirror");
  auto* raw = surface.get();
  surface->setConfigureCallback([raw](std::uint32_t, std::uint32_t) { raw->requestLayout(); });
  surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });
  std::weak_ptr<void> lifetime = m_lifetime;
  surface->setClosedCallback([this, lifetime]() {
    // The compositor closed us (user closed the window). Tear down off the callback.
    DeferredCall::callLater([this, lifetime]() {
      if (lifetime.expired()) {
        return;
      }
      close();
      if (m_closedByUser) {
        m_closedByUser();
      }
    });
  });

  const ToplevelSurfaceConfig config{
      .width = static_cast<std::uint32_t>(compositors::niri::kPhoneMirrorWidth),
      .height = static_cast<std::uint32_t>(compositors::niri::kPhoneMirrorHeight),
      .minWidth = 200,
      .minHeight = 400,
      .title = std::string(compositors::niri::kPhoneMirrorWindowTitle),
      .appId = compositors::niri::kPhoneMirrorAppId,
  };
  if (!surface->initialize(nullptr, config)) {
    kLog.warn("could not create the phone mirror window");
    return;
  }
  m_surface = std::move(surface);
  m_haveFrame = false;
  m_surface->requestLayout();
  // A frame may already be waiting from the headless stretch.
  bool pending = false;
  {
    std::lock_guard lock(m_frameMutex);
    pending = m_frameReady;
  }
  if (pending) {
    m_surface->requestUpdate();
  }
  kLog.info("phone mirror window open");
}

void PhoneMirrorWindow::close() {
  m_flushTimer.stop();
  m_pendingPointer = false;
  m_scrollRemainder = 0.0;
  if (m_surface != nullptr) {
    if (m_texture->handle.valid()) {
      m_renderContext.makeCurrent(m_surface->renderTarget());
      m_surface->renderTarget().renderer().textureManager().unload(m_texture->handle);
      m_texture->handle = {};
      m_texture->bound = false;
    }
    m_surface->setSceneRoot(nullptr);
    m_root.reset();
    m_image = nullptr;
    m_waiting = nullptr;
    m_waitingTitle = nullptr;
    m_waitingMirrorStep = nullptr;
    m_waitingPairStep = nullptr;
    m_strip = nullptr;
    m_stripText = nullptr;
    m_surface.reset();
    kLog.info("phone mirror window closed");
  }
  (void)m_link->send({{"type", "release"}});
  m_buttons = 0;
  m_pointerInside = false;
}

void PhoneMirrorWindow::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_surface == nullptr) {
    return;
  }
  m_renderContext.makeCurrent(m_surface->renderTarget());
  Renderer& renderer = m_surface->renderTarget().renderer();
  const float scale = m_surface->effectiveBufferScale();

  if (m_root == nullptr) {
    buildScene(scale);
    needsLayout = true;
  }

  if (needsUpdate) {
    uploadFrame();
  }

  if (needsLayout && m_root != nullptr) {
    m_root->setPosition(0.0F, 0.0F);
    m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
    m_root->layout(renderer);
  }
}

// Black stage. While no picture has arrived the centre carries the two steps the
// user has to take on the phone; once frames flow the picture fills the window and
// a thin strip under it only stays while the pointer still needs pairing.
void PhoneMirrorWindow::buildScene(float scale) {
  const float pad = 28.0F * scale;
  const auto dim = ColorSpec{.fixed = rgba(1.0F, 1.0F, 1.0F, 0.55F)};
  const auto bright = ColorSpec{.fixed = rgba(1.0F, 1.0F, 1.0F, 0.92F)};

  m_root = ui::column({
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .fill = ColorSpec{.fixed = rgba(0.0F, 0.0F, 0.0F, 1.0F)},
  });

  auto waiting = ui::column({
      .out = &m_waiting,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = 18.0F * scale,
      .paddingV = pad,
      .paddingH = pad,
      .flexGrow = 1.0F,
  });
  waiting->addChild(ui::label({
      .out = &m_waitingTitle,
      .text = i18n::tr("control-center.phone.cast-mirror-title"),
      .fontSize = Style::fontSizeTitle * 1.6F * scale,
      .fontWeight = FontWeight::Bold,
      .color = bright,
      .textAlign = TextAlign::Center,
  }));
  waiting->addChild(ui::label({
      .out = &m_waitingMirrorStep,
      .text = i18n::tr("control-center.phone.cast-mirror-step-mirror"),
      .fontSize = Style::fontSizeBody * scale,
      .color = bright,
      .maxLines = 4,
      .textAlign = TextAlign::Center,
  }));
  waiting->addChild(ui::label({
      .out = &m_waitingPairStep,
      .text = i18n::tr("control-center.phone.cast-mirror-step-pair"),
      .fontSize = Style::fontSizeCaption * scale,
      .color = dim,
      .maxLines = 5,
      .textAlign = TextAlign::Center,
  }));
  m_root->addChild(std::move(waiting));

  m_root->addChild(ui::image({
      .out = &m_image,
      .fit = ImageFit::Contain,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
  }));

  auto strip = ui::column({
      .out = &m_strip,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .paddingV = 10.0F * scale,
      .paddingH = 16.0F * scale,
      .fill = ColorSpec{.fixed = rgba(0.08F, 0.08F, 0.09F, 1.0F)},
      .visible = false,
      .participatesInLayout = false,
  });
  strip->addChild(ui::label({
      .out = &m_stripText,
      .text = i18n::tr("control-center.phone.cast-mirror-step-pair-short"),
      .fontSize = Style::fontSizeCaption * scale,
      .color = dim,
      .maxLines = 3,
      .textAlign = TextAlign::Center,
  }));
  m_root->addChild(std::move(strip));

  m_surface->setSceneRoot(m_root.get());
  applyStatus();
}

void PhoneMirrorWindow::setStatus(const PhoneMirrorStatus& status) {
  if (status == m_status) {
    return;
  }
  m_status = status;
  if (m_root != nullptr) {
    applyStatus();
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  }
}

void PhoneMirrorWindow::applyStatus() {
  const bool needPairing = !m_status.pointerConnected;
  const char* pairKey = m_status.pointerBondLost ? "control-center.phone.cast-mirror-pairing-lost"
                                                 : "control-center.phone.cast-mirror-step-pair";
  if (m_waitingPairStep != nullptr) {
    m_waitingPairStep->setText(i18n::tr(needPairing ? pairKey : "control-center.phone.cast-mirror-pointer-ready"));
  }
  if (m_stripText != nullptr) {
    m_stripText->setText(
        i18n::tr(m_status.pointerBondLost ? "control-center.phone.cast-mirror-pairing-lost"
                                          : "control-center.phone.cast-mirror-step-pair-short")
    );
  }
  auto show = [](Node* node, bool shown) {
    if (node != nullptr) {
      node->setVisible(shown);
      node->setParticipatesInLayout(shown);
    }
  };
  show(m_waiting, !m_haveFrame);
  show(m_image, m_haveFrame);
  show(m_strip, m_haveFrame && needPairing);
}

void PhoneMirrorWindow::uploadFrame() {
  std::vector<std::uint8_t> frame;
  {
    std::lock_guard lock(m_frameMutex);
    if (!m_frameReady) {
      return;
    }
    frame.swap(m_readyFrame);
    m_frameReady = false;
  }
  m_updatePosted = false;
  if (frame.size() != kFrameBytes || m_image == nullptr) {
    return;
  }

  Renderer& renderer = m_surface->renderTarget().renderer();
  auto& textures = renderer.textureManager();
  if (!m_texture->handle.valid()) {
    m_texture->handle = textures.createEmpty(
        kPhoneMirrorFrameWidth, kPhoneMirrorFrameHeight, TextureDataFormat::Rgba, TextureFilter::Linear
    );
    m_texture->bound = false;
  }
  textures.updateSubImage(
      m_texture->handle, frame.data(), 0, 0, kPhoneMirrorFrameWidth, kPhoneMirrorFrameHeight, TextureDataFormat::Rgba
  );
  if (!m_texture->bound) {
    m_image->setExternalTexture(renderer, m_texture->handle);
    m_texture->bound = true;
  }
  if (!m_haveFrame) {
    m_haveFrame = true;
    applyStatus();
    m_surface->requestLayout();
  }
  m_surface->requestRedraw();

  // Hand the buffer back so the reader does not reallocate every frame.
  std::lock_guard lock(m_frameMutex);
  if (m_readyFrame.empty()) {
    m_readyFrame.swap(frame);
    m_readyFrame.clear();
  }
}

void PhoneMirrorWindow::readerLoop() {
  const auto fifo = phoneMirrorVideoPath();
  // O_RDWR keeps a writer end open on our side so the FIFO never reports EOF between
  // UxPlay pipeline restarts; poll() then blocks properly while no frames arrive.
  const int fd = ::open(fifo.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    kLog.warn("could not open {}: {}", fifo.string(), std::strerror(errno));
    return;
  }
  (void)::fcntl(fd, F_SETPIPE_SZ, 4 * 1024 * 1024);

  std::vector<std::uint8_t> frame(kFrameBytes);
  std::size_t filled = 0;
  std::weak_ptr<void> lifetime = m_lifetime;
  while (!m_stop.load()) {
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&pfd, 1, 100);
    if (ready <= 0) {
      continue;
    }
    const ssize_t got = ::read(fd, frame.data() + filled, kFrameBytes - filled);
    if (got < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        continue;
      }
      kLog.warn("phone mirror read failed: {}", std::strerror(errno));
      break;
    }
    if (got == 0) {
      continue;
    }
    filled += static_cast<std::size_t>(got);
    if (filled < kFrameBytes) {
      continue;
    }
    filled = 0;
    {
      std::lock_guard lock(m_frameMutex);
      // Drop the previous unconsumed frame: the newest one wins.
      m_readyFrame.swap(frame);
      m_frameReady = true;
      if (frame.size() != kFrameBytes) {
        frame.assign(kFrameBytes, 0);
      }
    }
    if (!m_updatePosted.exchange(true)) {
      DeferredCall::callLater([this, lifetime]() {
        if (!lifetime.expired()) {
          handleFrameReady();
        }
      });
    }
  }
  ::close(fd);
}

void PhoneMirrorWindow::handleFrameReady() {
  if (m_surface != nullptr) {
    m_surface->requestUpdate();
  } else {
    // Headless: drop it. The reader keeps draining so the FIFO never fills.
    std::lock_guard lock(m_frameMutex);
    m_frameReady = false;
    m_updatePosted = false;
  }
}

void PhoneMirrorWindow::queuePointer(double sx, double sy, bool urgent) {
  if (m_image == nullptr || !m_image->visible() || m_image->width() <= 0.0F || m_image->height() <= 0.0F) {
    return;
  }
  // The frame is letterboxed into the image by Contain fit; map through the drawn rect.
  const float frameAspect = static_cast<float>(kPhoneMirrorFrameWidth) / static_cast<float>(kPhoneMirrorFrameHeight);
  float drawW = m_image->width();
  float drawH = m_image->height();
  if (drawW / drawH > frameAspect) {
    drawW = drawH * frameAspect;
  } else {
    drawH = drawW / frameAspect;
  }
  const float left = m_image->x() + (m_image->width() - drawW) * 0.5F;
  const float top = m_image->y() + (m_image->height() - drawH) * 0.5F;
  double fx = std::clamp((sx - left) / drawW, 0.0, 1.0);
  double fy = std::clamp((sy - top) / drawH, 0.0, 1.0);

  // Edge gestures. A press that lands in the outer band (frame pixels, so the band
  // scales with the window) is placed on the exact edge; the drag then continues from
  // the real pointer, so iOS sees a finger that started on the edge and swiped in:
  // bottom is Home (hold for the switcher), left Back, top Control Center/Notifications.
  constexpr double kEdgeBand = 28.0;
  const double bandX = kEdgeBand / static_cast<double>(kPhoneMirrorFrameWidth);
  const double bandY = kEdgeBand / static_cast<double>(kPhoneMirrorFrameHeight);
  if (m_buttons != 0 && urgent && !m_edgeSnapped) {
    m_edgeSnapped = true;
    if (fy >= 1.0 - bandY) {
      fy = 0.995;
    } else if (fy <= bandY) {
      fy = 0.0;
    } else if (fx <= bandX) {
      fx = 0.0;
    } else if (fx >= 1.0 - bandX) {
      fx = 1.0;
    }
  } else if (m_buttons == 0) {
    m_edgeSnapped = false;
  }
  m_pendingX = fx;
  m_pendingY = fy;
  m_pendingPointer = true;
  scheduleFlush(urgent);
}

void PhoneMirrorWindow::queueScroll(double notches) {
  m_scrollRemainder += notches;
  scheduleFlush(false);
}

// Wayland delivers motion at the mouse's report rate (up to 1 kHz) while the phone
// only takes one HID report per BLE connection interval. Collapse everything that
// arrives between flushes into the latest position plus the summed wheel travel so
// the queue never grows and the phone cursor tracks the live position, not history.
void PhoneMirrorWindow::scheduleFlush(bool urgent) {
  if (urgent) {
    flushPointer();
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFlushAt);
  if (elapsed >= kPointerFlushInterval) {
    flushPointer();
    return;
  }
  if (!m_flushTimer.active()) {
    m_flushTimer.start(kPointerFlushInterval - elapsed, [this]() { flushPointer(); });
  }
}

void PhoneMirrorWindow::flushPointer() {
  m_flushTimer.stop();
  const int steps = static_cast<int>(std::trunc(m_scrollRemainder));
  if (!m_pendingPointer && steps == 0) {
    return;
  }
  m_scrollRemainder -= steps;
  m_lastFlushAt = std::chrono::steady_clock::now();
  nlohmann::json message{{"type", "pointer"}, {"buttons", m_buttons}};
  if (m_pendingPointer) {
    message["x"] = m_pendingX;
    message["y"] = m_pendingY;
    m_pendingPointer = false;
  }
  if (steps != 0) {
    message["scroll"] = std::clamp(steps, -127, 127);
  }
  (void)m_link->send(message);
}

bool PhoneMirrorWindow::onPointerEvent(const PointerEvent& event) {
  if (m_surface == nullptr) {
    return false;
  }
  wl_surface* target = event.surface != nullptr ? event.surface : m_wayland.lastPointerSurface();
  if (target != m_surface->wlSurface()) {
    return false;
  }
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_pointerInside = true;
    // iOS draws its own pointer at the mirrored spot; two cursors read as lag.
    if (m_haveFrame) {
      m_wayland.hideCursor();
    }
    queuePointer(event.sx, event.sy, true);
    break;
  case PointerEvent::Type::Leave:
    m_pointerInside = false;
    m_edgeSnapped = false;
    flushPointer();
    if (m_buttons != 0) {
      m_buttons = 0;
      (void)m_link->send({{"type", "release"}});
    }
    break;
  case PointerEvent::Type::Motion:
    if (m_haveFrame) {
      m_wayland.hideCursor();
    }
    queuePointer(event.sx, event.sy, false);
    break;
  case PointerEvent::Type::Button: {
    if (event.button == BTN_MIDDLE) {
      // Middle click: Home, the one gesture people reach for most.
      if (event.pressed) {
        flushPointer();
        (void)m_link->send({{"type", "shortcut"}, {"name", "home"}});
      }
      return true;
    }
    // Right stays iOS-native: AssistiveTouch opens its menu on secondary click.
    const std::uint32_t bit = event.button == BTN_LEFT ? 0x01U : event.button == BTN_RIGHT ? 0x02U : 0U;
    if (bit == 0U) {
      return true;
    }
    if (event.pressed) {
      m_buttons |= bit;
    } else {
      m_buttons &= ~bit;
    }
    // Button edges must never be merged away or delayed behind motion. The release
    // is sent at the pinned position so the swipe completes on the edge axis.
    queuePointer(event.sx, event.sy, true);
    break;
  }
  case PointerEvent::Type::Axis: {
    if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return true;
    }
    // Wheel notches arrive as value120 multiples; touchpads as smooth pixel deltas.
    // iOS reads the HID wheel the Mac way: a positive step moves the content up
    // (scrolls down), which is the same sign Wayland gives for wheel-down, so no flip.
    double notches = 0.0;
    if (event.axisValue120 != 0) {
      notches = static_cast<double>(event.axisValue120) / 120.0;
    } else if (event.axisDiscrete != 0) {
      notches = static_cast<double>(event.axisDiscrete);
    } else {
      // Touchpad: a finer step than a wheel notch so flicks turn into travel promptly.
      notches = event.axisValue / 10.0;
    }
    queueScroll(notches);
    break;
  }
  }
  return true;
}

bool PhoneMirrorWindow::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_surface == nullptr || m_wayland.lastKeyboardSurface() != m_surface->wlSurface()) {
    return false;
  }
  if (event.preedit) {
    return true;
  }
  // HID modifier bits: Ctrl 0x01, Shift 0x02, Alt 0x04, Super 0x08. The seat's mask
  // covers modifiers that arrive as wl_keyboard.modifiers without a matching key event.
  std::uint32_t hidModifiers = 0;
  if ((event.modifiers & KeyMod::Ctrl) != 0) {
    hidModifiers |= 0x01U;
  }
  if ((event.modifiers & KeyMod::Shift) != 0) {
    hidModifiers |= 0x02U;
  }
  if ((event.modifiers & KeyMod::Alt) != 0) {
    hidModifiers |= 0x04U;
  }
  if ((event.modifiers & KeyMod::Super) != 0) {
    hidModifiers |= 0x08U;
  }
  (void)m_link->send(
      {{"type", "keysym"}, {"keysym", event.sym}, {"pressed", event.pressed}, {"modifiers", hidModifiers}}
  );
  return true;
}
