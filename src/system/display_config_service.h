#pragma once

#include "system/display_config.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class CompositorPlatform;

class DisplayConfigService {
public:
  using ChangeCallback = std::function<void()>;

  explicit DisplayConfigService(CompositorPlatform& platform);
  ~DisplayConfigService();

  DisplayConfigService(const DisplayConfigService&) = delete;
  DisplayConfigService& operator=(const DisplayConfigService&) = delete;

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] const std::vector<display_config::Output>& outputs() const noexcept { return m_outputs; }
  [[nodiscard]] const display_config::Output* findOutput(std::string_view name) const;
  [[nodiscard]] std::size_t enabledCount() const noexcept;
  [[nodiscard]] bool hasEnabledExternalOutput() const noexcept;
  [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }
  [[nodiscard]] std::string fingerprint() const;

  void refresh();
  bool setMode(std::string_view name, int width, int height, std::uint32_t refreshMilliHz);
  bool setScale(std::string_view name, double scale);
  bool setTransform(std::string_view name, display_config::Transform transform);
  bool setPosition(std::string_view name, int x, int y);
  bool setOrigin(std::string_view name);
  bool setVrr(std::string_view name, bool enabled);
  bool setEnabled(std::string_view name, bool enabled);
  bool placeRelative(
      std::string_view name, std::string_view referenceName, display_config::LayoutDirection direction, int offset = 0
  );

  void setChangeCallback(ChangeCallback callback);
  void setConfigLoadedCallback(ChangeCallback callback);
  void setConnectedCallback(ChangeCallback callback);

private:
  class EventBridge;

  bool apply(std::string_view name, const nlohmann::json& action, bool refreshAfter = true);
  void onNiriConfigLoaded();
  void onNiriConnected();

  CompositorPlatform& m_platform;
  std::vector<display_config::Output> m_outputs;
  std::string m_lastError;
  ChangeCallback m_changeCallback;
  ChangeCallback m_configLoadedCallback;
  ChangeCallback m_connectedCallback;
  std::unique_ptr<EventBridge> m_eventBridge;
};
