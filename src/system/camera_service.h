#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Direct V4L2 backend for USB/UVC webcams: device enumeration, a YUYV mmap
// capture loop for a live preview, and the standard v4l2_queryctrl control
// set (brightness, contrast, exposure, white balance, etc.). No libv4l2 or
// other dependency: everything goes through the kernel's <linux/videodev2.h>
// ioctls directly, matching how the rest of the shell talks to Wayland.
struct CameraDevice {
  std::string id;
  std::string name;
};

enum class CameraControlType : std::uint8_t { Integer, Boolean, Menu };

struct CameraControlMenuItem {
  std::int32_t index = 0;
  std::string name;
};

struct CameraControl {
  std::uint32_t id = 0;
  std::string name;
  CameraControlType type = CameraControlType::Integer;
  std::int32_t minimum = 0;
  std::int32_t maximum = 0;
  std::int32_t step = 1;
  std::int32_t defaultValue = 0;
  std::int32_t value = 0;
  bool readOnly = false;
  std::vector<CameraControlMenuItem> menuItems;
};

// Well-known V4L2 control ids, re-exported so callers can group/pair controls
// (e.g. an Auto Exposure toggle next to its manual Shutter Speed slider, the
// way Elgato Camera Hub organizes webcam controls) without depending on
// <linux/videodev2.h> directly.
namespace camera_control_ids {
  extern const std::uint32_t brightness;
  extern const std::uint32_t contrast;
  extern const std::uint32_t saturation;
  extern const std::uint32_t hue;
  extern const std::uint32_t gamma;
  extern const std::uint32_t gain;
  extern const std::uint32_t sharpness;
  extern const std::uint32_t backlightCompensation;
  extern const std::uint32_t powerLineFrequency;
  extern const std::uint32_t autoWhiteBalance;
  extern const std::uint32_t whiteBalanceTemperature;
  extern const std::uint32_t exposureAuto;
  extern const std::uint32_t exposureAbsolute;
  extern const std::uint32_t focusAuto;
  extern const std::uint32_t focusAbsolute;
  extern const std::uint32_t zoomAbsolute;
  extern const std::uint32_t panAbsolute;
  extern const std::uint32_t tiltAbsolute;
} // namespace camera_control_ids

class CameraService {
public:
  CameraService() = default;
  ~CameraService();

  CameraService(const CameraService&) = delete;
  CameraService& operator=(const CameraService&) = delete;

  [[nodiscard]] std::vector<CameraDevice> listDevices() const;

  // Opens the device and negotiates a YUYV or MJPEG preview if the driver supports one.
  // Controls remain usable even when a preview could not be negotiated.
  bool start(const std::string& deviceId);
  void stop();
  [[nodiscard]] bool active() const noexcept { return m_fd >= 0; }
  [[nodiscard]] const std::string& activeDeviceId() const noexcept { return m_deviceId; }
  [[nodiscard]] bool previewAvailable() const noexcept { return m_previewAvailable; }
  [[nodiscard]] int previewWidth() const noexcept { return m_width; }
  [[nodiscard]] int previewHeight() const noexcept { return m_height; }

  [[nodiscard]] std::vector<CameraControl> controls() const;
  void setControl(std::uint32_t id, std::int32_t value);
  void resetControlsToDefaults();

  using FrameCallback = std::function<void(int width, int height, const std::uint8_t* rgba)>;
  void setFrameCallback(FrameCallback callback) { m_frameCallback = std::move(callback); }

  // PollSource integration: fd() is only non-negative while a preview stream is active.
  [[nodiscard]] int fd() const noexcept { return m_previewAvailable ? m_fd : -1; }
  void dispatch();

private:
  struct MappedBuffer {
    void* start = nullptr;
    std::size_t length = 0;
  };

  void closeDevice();
  [[nodiscard]] std::vector<CameraControl> queryControls() const;
  bool startStreaming();
  void stopStreaming();

  bool negotiateFormat();
  bool decodeFrame(const std::uint8_t* data, std::size_t bytesUsed);

  std::string m_deviceId;
  int m_fd = -1;
  int m_width = 0;
  int m_height = 0;
  int m_bytesPerLine = 0;
  std::uint32_t m_pixelFormat = 0;
  bool m_previewAvailable = false;
  std::vector<MappedBuffer> m_buffers;
  std::vector<std::uint8_t> m_rgbaScratch;
  FrameCallback m_frameCallback;
};
