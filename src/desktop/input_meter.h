#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
struct pw_core;
namespace helix {
  // Reads a live float32 buffer without retaining samples; shared by the capture
  // path and its tests, including unaligned and non-finite input.
  double inputPeak(std::span<const std::byte> bytes);
  class InputMeter {
  public:
    InputMeter(pw_core* core, std::uint32_t source);
    ~InputMeter();
    InputMeter(const InputMeter&) = delete;
    InputMeter& operator=(const InputMeter&) = delete;
    double peak() const;
    bool available() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
  };
} // namespace helix
