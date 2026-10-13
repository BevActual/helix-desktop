#pragma once

#include "inference/backend.h"

#include <expected>
#include <filesystem>

namespace helix::inference {

  inline constexpr std::size_t kMaxGeneratedPng = 4 * 1024 * 1024;
  using ImageProgress = std::function<void(double fraction, const std::string& stage)>;
  std::expected<void, std::string> validateImageRequest(const ChatRequest& request);
  std::string encodePng(const std::vector<std::uint8_t>& rgb, std::uint32_t width, std::uint32_t height);

  class ImageGenerator {
  public:
    virtual ~ImageGenerator() = default;
    virtual Outcome generate(const ChatRequest&, const ImageProgress&, const std::function<bool()>& cancelled) = 0;
  };

  class ProcessImageGenerator final : public ImageGenerator {
  public:
    ProcessImageGenerator(std::filesystem::path worker, std::filesystem::path models);
    Outcome generate(const ChatRequest&, const ImageProgress&, const std::function<bool()>& cancelled) override;

  private:
    std::filesystem::path worker_, models_;
  };

  class FakeImageGenerator final : public ImageGenerator {
  public:
    Outcome generate(const ChatRequest&, const ImageProgress&, const std::function<bool()>& cancelled) override;
  };

} // namespace helix::inference
