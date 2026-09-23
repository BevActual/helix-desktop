#include "system/camera_service.h"

#include "core/log.h"
#include "render/core/image_decoder.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("camera");

  [[nodiscard]] int xioctl(int fd, unsigned long request, void* arg) {
    int result = 0;
    do {
      result = ::ioctl(fd, request, arg); // NOLINT(cppcoreguidelines-pro-type-vararg)
    } while (result == -1 && errno == EINTR);
    return result;
  }

  [[nodiscard]] std::string fourccName(std::uint32_t fourcc) {
    const char chars[5] = {
        static_cast<char>(fourcc & 0xFFU), static_cast<char>((fourcc >> 8) & 0xFFU),
        static_cast<char>((fourcc >> 16) & 0xFFU), static_cast<char>((fourcc >> 24) & 0xFFU), 0};
    return chars;
  }

  [[nodiscard]] std::string fixedString(const char* data, std::size_t size) {
    return {data, strnlen(data, size)};
  }

  // UVC cards read "Integrated Camera: Integrated C" — the driver repeats the product name after
  // the bus name and the 32-byte field truncates it. Show the bus name alone when the tail is a
  // prefix of it (or vice versa) so the header stays readable.
  [[nodiscard]] std::string tidyCardName(std::string name) {
    const auto sep = name.find(": ");
    if (sep == std::string::npos || sep == 0) {
      return name;
    }
    const std::string_view head{name.data(), sep};
    std::string_view tail{name};
    tail.remove_prefix(sep + 2);
    while (!tail.empty() && tail.back() == ' ') {
      tail.remove_suffix(1);
    }
    if (tail.empty() || head.starts_with(tail) || tail.starts_with(head)) {
      return std::string(head);
    }
    return name;
  }

  [[nodiscard]] std::uint8_t clampByte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
  }

  // BT.601 YUYV (4:2:2 packed) -> RGBA8. bytesPerLine may include driver padding.
  void convertYuyvToRgba(
      const std::uint8_t* yuyv, int width, int height, int bytesPerLine, std::vector<std::uint8_t>& outRgba
  ) {
    const int stride = bytesPerLine > 0 ? bytesPerLine : width * 2;
    outRgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
      const std::uint8_t* src = yuyv + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
      std::uint8_t* dst = outRgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
      for (int x = 0; x + 1 < width; x += 2) {
        const int y0 = src[0];
        const int u = src[1];
        const int y1 = src[2];
        const int v = src[3];
        src += 4;
        const int c0 = y0 - 16;
        const int c1 = y1 - 16;
        const int d = u - 128;
        const int e = v - 128;
        dst[0] = clampByte((298 * c0 + 409 * e + 128) >> 8);
        dst[1] = clampByte((298 * c0 - 100 * d - 208 * e + 128) >> 8);
        dst[2] = clampByte((298 * c0 + 516 * d + 128) >> 8);
        dst[3] = 255;
        dst[4] = clampByte((298 * c1 + 409 * e + 128) >> 8);
        dst[5] = clampByte((298 * c1 - 100 * d - 208 * e + 128) >> 8);
        dst[6] = clampByte((298 * c1 + 516 * d + 128) >> 8);
        dst[7] = 255;
        dst += 8;
      }
    }
  }

  [[nodiscard]] bool isVideoNodeName(const std::string& name) {
    constexpr std::string_view kPrefix = "video";
    if (!name.starts_with(kPrefix) || name.size() <= kPrefix.size()) {
      return false;
    }
    return std::ranges::all_of(name.substr(kPrefix.size()), [](char c) {
      return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
  }

} // namespace

namespace camera_control_ids {
  const std::uint32_t brightness = V4L2_CID_BRIGHTNESS;
  const std::uint32_t contrast = V4L2_CID_CONTRAST;
  const std::uint32_t saturation = V4L2_CID_SATURATION;
  const std::uint32_t hue = V4L2_CID_HUE;
  const std::uint32_t gamma = V4L2_CID_GAMMA;
  const std::uint32_t gain = V4L2_CID_GAIN;
  const std::uint32_t sharpness = V4L2_CID_SHARPNESS;
  const std::uint32_t backlightCompensation = V4L2_CID_BACKLIGHT_COMPENSATION;
  const std::uint32_t powerLineFrequency = V4L2_CID_POWER_LINE_FREQUENCY;
  const std::uint32_t autoWhiteBalance = V4L2_CID_AUTO_WHITE_BALANCE;
  const std::uint32_t whiteBalanceTemperature = V4L2_CID_WHITE_BALANCE_TEMPERATURE;
  const std::uint32_t exposureAuto = V4L2_CID_EXPOSURE_AUTO;
  const std::uint32_t exposureAbsolute = V4L2_CID_EXPOSURE_ABSOLUTE;
  const std::uint32_t focusAuto = V4L2_CID_FOCUS_AUTO;
  const std::uint32_t focusAbsolute = V4L2_CID_FOCUS_ABSOLUTE;
  const std::uint32_t zoomAbsolute = V4L2_CID_ZOOM_ABSOLUTE;
  const std::uint32_t panAbsolute = V4L2_CID_PAN_ABSOLUTE;
  const std::uint32_t tiltAbsolute = V4L2_CID_TILT_ABSOLUTE;
} // namespace camera_control_ids

CameraService::~CameraService() { closeDevice(); }

std::vector<CameraDevice> CameraService::listDevices() const {
  std::vector<CameraDevice> devices;
  std::vector<std::filesystem::path> candidates;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
    if (isVideoNodeName(entry.path().filename().string())) {
      candidates.push_back(entry.path());
    }
  }
  std::ranges::sort(candidates);

  for (const auto& path : candidates) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (fd < 0) {
      continue;
    }
    v4l2_capability cap{};
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) { // NOLINT(cppcoreguidelines-pro-type-vararg)
      const std::uint32_t caps =
          (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? cap.device_caps : cap.capabilities;
      if ((caps & V4L2_CAP_VIDEO_CAPTURE) != 0 && (caps & V4L2_CAP_STREAMING) != 0) {
        std::string name = tidyCardName(fixedString(reinterpret_cast<const char*>(cap.card), sizeof(cap.card)));
        devices.push_back({.id = path.string(), .name = name.empty() ? path.string() : name});
      }
    }
    ::close(fd);
  }
  return devices;
}

bool CameraService::start(const std::string& deviceId) {
  stop();

  const int fd = ::open(deviceId.c_str(), O_RDWR | O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (fd < 0) {
    kLog.warn("open {} failed: {}", deviceId, std::strerror(errno));
    return false;
  }
  m_fd = fd;
  m_deviceId = deviceId;
  m_previewAvailable = startStreaming();
  if (!m_previewAvailable) {
    kLog.warn("{} opened, but no preview stream could be negotiated", deviceId);
  }
  return true;
}

bool CameraService::negotiateFormat() {
  bool hasYuyv = false;
  bool hasMjpeg = false;
  for (std::uint32_t index = 0;; ++index) {
    v4l2_fmtdesc desc{};
    desc.index = index;
    desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_ENUM_FMT, &desc) != 0) {
      break;
    }
    hasYuyv = hasYuyv || desc.pixelformat == V4L2_PIX_FMT_YUYV;
    hasMjpeg = hasMjpeg || desc.pixelformat == V4L2_PIX_FMT_MJPEG;
  }

  struct Candidate {
    std::uint32_t fourcc = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };
  std::vector<Candidate> candidates;
  if (hasYuyv) {
    candidates.push_back({.fourcc = V4L2_PIX_FMT_YUYV, .width = 640, .height = 480});
    candidates.push_back({.fourcc = V4L2_PIX_FMT_YUYV, .width = 1280, .height = 720});
  }
  if (hasMjpeg) {
    candidates.push_back({.fourcc = V4L2_PIX_FMT_MJPEG, .width = 1280, .height = 720});
    candidates.push_back({.fourcc = V4L2_PIX_FMT_MJPEG, .width = 640, .height = 480});
  }
  if (candidates.empty()) {
    kLog.warn("{} has no YUYV or MJPEG capture format", m_deviceId);
    return false;
  }

  for (const auto& candidate : candidates) {
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = candidate.width;
    fmt.fmt.pix.height = candidate.height;
    fmt.fmt.pix.pixelformat = candidate.fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) != 0) {
      continue;
    }
    if (fmt.fmt.pix.pixelformat != candidate.fourcc || fmt.fmt.pix.width == 0 || fmt.fmt.pix.height == 0) {
      continue;
    }
    m_pixelFormat = fmt.fmt.pix.pixelformat;
    m_width = static_cast<int>(fmt.fmt.pix.width);
    m_height = static_cast<int>(fmt.fmt.pix.height);
    m_bytesPerLine = static_cast<int>(fmt.fmt.pix.bytesperline);
    kLog.info(
        "{} preview {} {}x{}", m_deviceId, fourccName(m_pixelFormat), m_width, m_height
    );
    return true;
  }
  kLog.warn("{} rejected YUYV/MJPEG preview sizes", m_deviceId);
  return false;
}

bool CameraService::startStreaming() {
  if (!negotiateFormat()) {
    return false;
  }

  v4l2_requestbuffers req{};
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(m_fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) {
    kLog.warn("{} REQBUFS failed: {}", m_deviceId, std::strerror(errno));
    return false;
  }

  m_buffers.assign(req.count, MappedBuffer{});
  for (std::uint32_t i = 0; i < req.count; ++i) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) != 0) {
      stopStreaming();
      return false;
    }
    void* start = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, static_cast<off_t>(buf.m.offset));
    if (start == MAP_FAILED) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
      kLog.warn("{} mmap failed: {}", m_deviceId, std::strerror(errno));
      stopStreaming();
      return false;
    }
    m_buffers[i] = {.start = start, .length = buf.length};
    if (xioctl(m_fd, VIDIOC_QBUF, &buf) != 0) {
      stopStreaming();
      return false;
    }
  }

  auto bufType = static_cast<int>(V4L2_BUF_TYPE_VIDEO_CAPTURE);
  if (xioctl(m_fd, VIDIOC_STREAMON, &bufType) != 0) {
    kLog.warn("{} STREAMON failed: {}", m_deviceId, std::strerror(errno));
    stopStreaming();
    return false;
  }
  return true;
}

void CameraService::stopStreaming() {
  if (!m_buffers.empty()) {
    const auto bufType = static_cast<int>(V4L2_BUF_TYPE_VIDEO_CAPTURE);
    ::ioctl(m_fd, VIDIOC_STREAMOFF, &bufType); // NOLINT(cppcoreguidelines-pro-type-vararg)
    for (auto& buffer : m_buffers) {
      if (buffer.start != nullptr) {
        ::munmap(buffer.start, buffer.length);
      }
    }
    m_buffers.clear();
  }
  m_previewAvailable = false;
  m_width = 0;
  m_height = 0;
  m_bytesPerLine = 0;
  m_pixelFormat = 0;
}

void CameraService::stop() { closeDevice(); }

void CameraService::closeDevice() {
  if (m_fd < 0) {
    return;
  }
  stopStreaming();
  ::close(m_fd);
  m_fd = -1;
  m_deviceId.clear();
}

void CameraService::dispatch() {
  if (m_fd < 0 || !m_previewAvailable) {
    return;
  }
  v4l2_buffer buf{};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  if (xioctl(m_fd, VIDIOC_DQBUF, &buf) != 0) {
    return; // EAGAIN (no frame ready yet) or a transient driver error; retry next dispatch.
  }
  if (buf.index < m_buffers.size() && m_frameCallback && buf.bytesused > 0) {
    const auto* data = static_cast<const std::uint8_t*>(m_buffers[buf.index].start);
    if (decodeFrame(data, buf.bytesused)) {
      m_frameCallback(m_width, m_height, m_rgbaScratch.data());
    }
  }
  (void)xioctl(m_fd, VIDIOC_QBUF, &buf);
}

bool CameraService::decodeFrame(const std::uint8_t* data, std::size_t bytesUsed) {
  if (m_pixelFormat == V4L2_PIX_FMT_YUYV) {
    convertYuyvToRgba(data, m_width, m_height, m_bytesPerLine, m_rgbaScratch);
    return m_rgbaScratch.size()
        == static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 4;
  }
  if (m_pixelFormat == V4L2_PIX_FMT_MJPEG) {
    auto decoded = decodeRasterImage(data, bytesUsed);
    if (!decoded) {
      return false;
    }
    m_width = decoded->width;
    m_height = decoded->height;
    m_rgbaScratch = std::move(decoded->pixels);
    return m_width > 0 && m_height > 0 && !m_rgbaScratch.empty();
  }
  return false;
}

std::vector<CameraControl> CameraService::controls() const { return queryControls(); }

std::vector<CameraControl> CameraService::queryControls() const {
  std::vector<CameraControl> result;
  if (m_fd < 0) {
    return result;
  }
  v4l2_queryctrl query{};
  query.id = V4L2_CTRL_FLAG_NEXT_CTRL;
  while (::ioctl(m_fd, VIDIOC_QUERYCTRL, &query) == 0) { // NOLINT(cppcoreguidelines-pro-type-vararg)
    const std::uint32_t id = query.id;
    query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    if ((query.flags & V4L2_CTRL_FLAG_DISABLED) != 0) {
      continue;
    }
    CameraControlType type{};
    switch (query.type) {
    case V4L2_CTRL_TYPE_INTEGER:
      type = CameraControlType::Integer;
      break;
    case V4L2_CTRL_TYPE_BOOLEAN:
      type = CameraControlType::Boolean;
      break;
    case V4L2_CTRL_TYPE_MENU:
      type = CameraControlType::Menu;
      break;
    default:
      continue; // Buttons, strings, control-class markers, etc. are not exposed.
    }

    CameraControl control{
        .id = id,
        .name = fixedString(reinterpret_cast<const char*>(query.name), sizeof(query.name)),
        .type = type,
        .minimum = query.minimum,
        .maximum = query.maximum,
        .step = query.step > 0 ? query.step : 1,
        .defaultValue = query.default_value,
        .readOnly = (query.flags & (V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_GRABBED)) != 0,
    };

    v4l2_control current{.id = id, .value = 0};
    control.value =
        ::ioctl(m_fd, VIDIOC_G_CTRL, &current) == 0 ? current.value : control.defaultValue; // NOLINT

    if (type == CameraControlType::Menu) {
      for (std::int32_t index = query.minimum; index <= query.maximum; ++index) {
        v4l2_querymenu menu{};
        menu.id = id;
        menu.index = static_cast<std::uint32_t>(index);
        if (::ioctl(m_fd, VIDIOC_QUERYMENU, &menu) == 0) { // NOLINT(cppcoreguidelines-pro-type-vararg)
          control.menuItems.push_back(
              {.index = index, .name = fixedString(reinterpret_cast<const char*>(menu.name), sizeof(menu.name))}
          );
        }
      }
    }
    result.push_back(std::move(control));
  }
  return result;
}

void CameraService::setControl(std::uint32_t id, std::int32_t value) {
  if (m_fd < 0) {
    return;
  }
  v4l2_control control{.id = id, .value = value};
  (void)::ioctl(m_fd, VIDIOC_S_CTRL, &control); // NOLINT(cppcoreguidelines-pro-type-vararg)
}

void CameraService::resetControlsToDefaults() {
  for (const auto& control : queryControls()) {
    if (!control.readOnly) {
      setControl(control.id, control.defaultValue);
    }
  }
}
