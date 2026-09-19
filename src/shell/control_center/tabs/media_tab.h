#pragma once

#include "core/timer_manager.h"
#include "dbus/mpris/mpris_service.h"
#include "shell/control_center/settings_pane.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class Button;
class ConfigService;
class Glyph;
class HttpClient;
class Image;
class InputArea;
class Label;
class MprisService;
class PipeWireSpectrum;
class Select;
class Slider;
class AudioVisualizer;
struct ControlCenterServices;

class MediaTab : public Tab {
public:
  MediaTab(
      MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, const ControlCenterServices& services
  );
  ~MediaTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;
  void setConfigureMode(bool show) override;
  [[nodiscard]] bool configureMode() const override;
  void setSettingsSearchQuery(std::string_view query) override;
  [[nodiscard]] SettingsPane* settingsPane() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refresh(Renderer& renderer);
  void clearArt(Renderer& renderer);
  void commitPendingSeek(double valueSeconds);
  void selectPlayer(std::size_t index);
  void syncPlayerSelect(const std::vector<MprisPlayerInfo>& players);
  void pinArtwork();
  void layoutArtworkSlot(float scale);
  void openPlayer();
  void hideActivePlayer();
  void commitVolume(double percent);
  [[nodiscard]] bool syncPlaybackChrome(bool hasPlayer, bool showVisualizer);

  // Guard token for deferred callbacks that run on the next main-loop tick.
  // Callbacks capture a weak_ptr so they can detect destruction without
  // relying on a raw this pointer staying valid.
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  PipeWireSpectrum* m_spectrum = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<SettingsPane> m_settings;
  std::uint64_t m_spectrumListenerId = 0;
  bool m_active = false;
  bool m_stageExpanded = false;

  Flex* m_rootLayout = nullptr;
  Flex* m_nowCard = nullptr;
  Flex* m_artworkWell = nullptr;
  Flex* m_playerSelectHost = nullptr;
  Select* m_playerSelect = nullptr;
  Button* m_hidePlayerButton = nullptr;
  Flex* m_artworkHost = nullptr;
  Flex* m_artworkSlot = nullptr;
  Glyph* m_artworkFallback = nullptr;
  Image* m_artwork = nullptr;
  InputArea* m_artworkHit = nullptr;
  Flex* m_metaColumn = nullptr;
  Label* m_trackTitle = nullptr;
  Label* m_trackArtist = nullptr;
  Label* m_trackAlbum = nullptr;
  Flex* m_progressRow = nullptr;
  Label* m_elapsedLabel = nullptr;
  Slider* m_progressSlider = nullptr;
  Label* m_remainingLabel = nullptr;
  Flex* m_visualizerHost = nullptr;
  Flex* m_visualizerBody = nullptr;
  AudioVisualizer* m_visualizerSpectrum = nullptr;
  Flex* m_controlsHost = nullptr;
  Button* m_prevButton = nullptr;
  Button* m_playPauseButton = nullptr;
  Button* m_nextButton = nullptr;
  Button* m_repeatButton = nullptr;
  Button* m_shuffleButton = nullptr;
  Flex* m_volumeHost = nullptr;
  Slider* m_volumeSlider = nullptr;
  Label* m_volumeValue = nullptr;

  std::string m_lastArtPath;
  std::string m_lastBusName;
  std::string m_lastPlaybackStatus;
  std::string m_lastLoopStatus;
  bool m_lastShuffle = false;
  bool m_syncingProgress = false;
  bool m_syncingVolume = false;
  int m_lastSentVolumePercent = -1;
  std::int64_t m_pendingSeekUs = -1;
  std::string m_pendingSeekBusName;
  std::chrono::steady_clock::time_point m_pendingSeekUntil;
  std::chrono::steady_clock::time_point m_progressSettleUntil;
  std::vector<std::string> m_playerBusNames;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::string m_positionBusName;
  std::string m_positionTrackId;
  std::string m_positionTrackSignature;
  std::int64_t m_positionUs = 0;
  std::int64_t m_lastTrackLengthUs = 0;
  std::chrono::steady_clock::time_point m_positionSampleAt;
  std::optional<MprisPlayerInfo> m_lastActiveSnapshot;
  std::chrono::steady_clock::time_point m_lastActiveSeenAt;
  std::chrono::steady_clock::time_point m_nextRealtimeUpdateAt;
  std::chrono::steady_clock::time_point m_lastRealtimeMprisPollAt;
  Timer m_progressTimer;
};
