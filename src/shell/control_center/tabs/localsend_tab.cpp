#include "shell/control_center/tabs/localsend_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "net/localsend_service.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/clipboard_service.h"

#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  [[nodiscard]] bool setShown(Node* node, bool shown) {
    if (node == nullptr) {
      return false;
    }
    const bool changed = node->visible() != shown || node->participatesInLayout() != shown;
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return changed;
  }

  [[nodiscard]] std::string peerKind(std::string_view type) {
    if (type == "mobile") {
      return i18n::tr("control-center.localsend.device-mobile");
    }
    if (type == "tablet") {
      return i18n::tr("control-center.localsend.device-tablet");
    }
    if (type == "web") {
      return i18n::tr("control-center.localsend.device-web");
    }
    if (type == "headless" || type == "server") {
      return i18n::tr("control-center.localsend.device-server");
    }
    return i18n::tr("control-center.localsend.device-desktop");
  }

  [[nodiscard]] std::string peerDetail(const LocalSendPeer& peer) {
    std::string text = peerKind(peer.deviceType);
    if (!peer.ip.empty()) {
      text += " · ";
      text += peer.ip;
    }
    return text;
  }

} // namespace

LocalSendTab::Catalog LocalSendTab::catalogFromIndex(std::size_t index) noexcept {
  switch (index) {
  case 1:
    return Catalog::Incoming;
  case 2:
    return Catalog::History;
  default:
    return Catalog::Nearby;
  }
}

std::size_t LocalSendTab::indexFromCatalog(Catalog catalog) noexcept {
  switch (catalog) {
  case Catalog::Incoming:
    return 1;
  case Catalog::History:
    return 2;
  case Catalog::Nearby:
    break;
  }
  return 0;
}

LocalSendTab::LocalSendTab(const ControlCenterServices& services)
    : m_service(services.localSend), m_clipboard(services.clipboard), m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "localsend"})) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> catalog = m_config->stateString("localsend_tab", "catalog")) {
      if (*catalog == "incoming") {
        m_catalog = Catalog::Incoming;
      } else if (*catalog == "history") {
        m_catalog = Catalog::History;
      }
    }
  }
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

std::unique_ptr<Flex> LocalSendTab::create() {
  const float scale = contentScale();

  auto live = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });

  auto statusCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applyCompactCardStyle(section, scale, opacity, false);
      },
  });
  auto enableRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  enableRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.localsend.enable"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  enableRow->addChild(
      ui::toggle({
          .out = &m_enabled,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
      })
  );
  statusCard->addChild(std::move(enableRow));
  statusCard->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  statusCard->addChild(
      ui::label({
          .out = &m_detail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  statusCard->addChild(
      ui::input({
          .out = &m_pin,
          .placeholder = i18n::tr("control-center.localsend.their-pin"),
          .onChange = [this](const std::string& value) { m_pinText = value; },
      })
  );
  statusCard->addChild(
      ui::label({
          .out = &m_transfer,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  statusCard->addChild(
      ui::segmented({
          .out = &m_catalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {
                      .label = "",
                      .glyph = "devices",
                      .tooltip = i18n::tr("control-center.localsend.nearby"),
                  },
                  {
                      .label = "",
                      .glyph = "inbox",
                      .tooltip = i18n::tr("control-center.localsend.incoming-title"),
                  },
                  {
                      .label = "",
                      .glyph = "history",
                      .tooltip = i18n::tr("control-center.localsend.history"),
                  },
              },
          .selectedIndex = indexFromCatalog(m_catalog),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) { applyCatalog(index); },
      })
  );
  live->addChild(std::move(statusCard));

  auto leftover = ui::scrollView({
      .out = &m_liveScroll,
      .state = &m_liveScrollState,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  auto* leftoverContent = leftover->content();
  leftoverContent->setDirection(FlexDirection::Vertical);
  leftoverContent->setAlign(FlexAlign::Stretch);
  leftoverContent->setGap(Style::spaceMd * scale);

  auto incoming = ui::column({
      .out = &m_incomingCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*incoming, i18n::tr("control-center.localsend.incoming-title"), scale);
  incoming->addChild(
      ui::label({
          .out = &m_incomingLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  auto incomingRow = ui::row({
      .out = &m_incomingActions,
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceSm * scale,
  });
  incomingRow->addChild(
      ui::button({
          .out = &m_acceptButton,
          .text = i18n::tr("control-center.localsend.accept"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Primary,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->acceptIncoming();
                }
              },
      })
  );
  incomingRow->addChild(
      ui::button({
          .out = &m_rejectButton,
          .text = i18n::tr("control-center.localsend.decline"),
          .fontSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Destructive,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->rejectIncoming();
                }
              },
      })
  );
  incoming->addChild(std::move(incomingRow));
  leftoverContent->addChild(std::move(incoming));

  auto historyCard = ui::column({
      .out = &m_historyCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*historyCard, i18n::tr("control-center.localsend.history"), scale);
  historyCard->addChild(ui::column({.out = &m_historyList, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale}));
  leftoverContent->addChild(std::move(historyCard));

  auto peersCard = ui::column({
      .out = &m_peersCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*peersCard, i18n::tr("control-center.localsend.nearby"), scale);
  peersCard->addChild(ui::column({.out = &m_peerList, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale}));
  leftoverContent->addChild(std::move(peersCard));
  live->addChild(std::move(leftover));

  if (m_enabled != nullptr) {
    m_enabled->setOnChange([this](bool value) {
      if (m_syncing || m_config == nullptr) {
        return;
      }
      (void)m_config->setOverride({"localsend", "enabled"}, value);
    });
  }

  auto wrapped = m_settings->wrapLive(std::move(live), contentScale(), panelCardOpacity());
  syncUi();
  return wrapped;
}

std::unique_ptr<Flex> LocalSendTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.localsend.announce"),
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->announce();
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return m_settings->embedHeader(std::move(live), scale);
}

void LocalSendTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool LocalSendTab::configureMode() const { return m_settings->configureMode(); }

void LocalSendTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

SettingsPane* LocalSendTab::settingsPane() { return m_settings.get(); }

void LocalSendTab::setActive(bool active) {
  if (active && m_service != nullptr) {
    m_service->announce();
  }
}

void LocalSendTab::onClose() {
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_rootLayout = nullptr;
  m_status = nullptr;
  m_detail = nullptr;
  m_enabled = nullptr;
  m_pin = nullptr;
  m_catalogSwitch = nullptr;
  m_incomingCard = nullptr;
  m_incomingLabel = nullptr;
  m_incomingActions = nullptr;
  m_acceptButton = nullptr;
  m_rejectButton = nullptr;
  m_transfer = nullptr;
  m_historyCard = nullptr;
  m_historyList = nullptr;
  m_peersCard = nullptr;
  m_peerList = nullptr;
  m_liveScroll = nullptr;
  m_peerSignature.clear();
  m_historySignature.clear();
}

void LocalSendTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void LocalSendTab::onPanelCardOpacityChanged(float opacity) {
  if (m_catalogSwitch != nullptr) {
    m_catalogSwitch->setSurfaceOpacity(opacity);
  }
}

void LocalSendTab::persistCatalog() {
  if (m_config == nullptr) {
    return;
  }
  const char* value = "nearby";
  switch (m_catalog) {
  case Catalog::Incoming:
    value = "incoming";
    break;
  case Catalog::History:
    value = "history";
    break;
  case Catalog::Nearby:
    break;
  }
  (void)m_config->setStateString("localsend_tab", "catalog", value);
}

void LocalSendTab::syncCatalog() {
  const bool nearby = m_catalog == Catalog::Nearby;
  const bool incoming = m_catalog == Catalog::Incoming;
  const bool history = m_catalog == Catalog::History;
  (void)setShown(m_peersCard, nearby);
  (void)setShown(m_incomingCard, incoming);
  (void)setShown(m_historyCard, history);
  if (m_catalogSwitch != nullptr && m_catalogSwitch->selectedIndex() != indexFromCatalog(m_catalog)) {
    m_catalogSyncing = true;
    m_catalogSwitch->setSelectedIndex(indexFromCatalog(m_catalog));
    m_catalogSyncing = false;
  }
}

void LocalSendTab::applyCatalog(std::size_t index) {
  if (m_catalogSyncing) {
    return;
  }
  const Catalog next = catalogFromIndex(index);
  if (next == m_catalog) {
    syncCatalog();
    return;
  }
  m_catalog = next;
  persistCatalog();
  syncCatalog();
  PanelManager::instance().requestLayout();
}

void LocalSendTab::sendFileTo(const std::string& peerKey) {
  if (m_service == nullptr) {
    return;
  }
  (void)FileDialog::open(
      FileDialogOptions{.title = i18n::tr("control-center.localsend.send-file")},
      [this, peerKey](std::optional<std::filesystem::path> path) {
        if (!path || m_service == nullptr) {
          return;
        }
        m_service->sendPath(peerKey, *path, m_pinText);
      }
  );
}

void LocalSendTab::sendFolderTo(const std::string& peerKey) {
  if (m_service == nullptr) {
    return;
  }
  (void)FileDialog::open(
      FileDialogOptions{
          .mode = FileDialogMode::SelectFolder,
          .title = i18n::tr("control-center.localsend.send-folder"),
      },
      [this, peerKey](std::optional<std::filesystem::path> path) {
        if (!path || m_service == nullptr) {
          return;
        }
        m_service->sendPath(peerKey, *path, m_pinText);
      }
  );
}

void LocalSendTab::sendClipboardTo(const std::string& peerKey) {
  if (m_service == nullptr || m_clipboard == nullptr) {
    return;
  }
  const auto text = m_clipboard->clipboardText();
  if (!text || text->empty()) {
    return;
  }
  m_service->sendText(peerKey, *text, m_pinText);
}

void LocalSendTab::syncUi() {
  if (m_service == nullptr || m_status == nullptr) {
    return;
  }
  m_syncing = true;
  const auto& snap = m_service->snapshot();
  if (m_enabled != nullptr) {
    m_enabled->setChecked(snap.enabled);
  }
  if (!snap.enabled) {
    m_status->setText(i18n::tr("control-center.localsend.disabled"));
    m_detail->setText(i18n::tr("control-center.localsend.disabled-hint"));
  } else if (snap.listening) {
    m_status->setText(snap.alias);
    m_detail->setText(i18n::tr("control-center.localsend.listening", "name", snap.alias));
  } else {
    m_status->setText(snap.alias);
    m_detail->setText(snap.error.empty() ? i18n::tr("control-center.localsend.port-busy") : snap.error);
  }
  if (m_incomingLabel != nullptr) {
    if (snap.incoming) {
      std::string files;
      for (const auto& file : snap.incoming->files) {
        if (!files.empty()) {
          files += ", ";
        }
        files += file.fileName;
      }
      m_incomingLabel->setText(std::format("{} · {}", snap.incoming->alias, files));
    } else {
      m_incomingLabel->setText(i18n::tr("control-center.localsend.incoming-idle"));
    }
  }
  if (m_transfer != nullptr) {
    if (!snap.enabled || !snap.transfer) {
      m_transfer->setText({});
    } else if (snap.transfer->failed) {
      m_transfer->setText(snap.transfer->error);
    } else if (snap.transfer->incoming) {
      m_transfer->setText(
          i18n::tr(
              "control-center.localsend.progress-receive", "index", snap.transfer->fileIndex, "count",
              snap.transfer->fileCount, "file", snap.transfer->label, "name", snap.transfer->peerAlias
          )
      );
    } else if (snap.transfer->fileCount > 0) {
      m_transfer->setText(
          i18n::tr(
              "control-center.localsend.progress-send", "index", snap.transfer->fileIndex, "count",
              snap.transfer->fileCount, "file", snap.transfer->label, "name", snap.transfer->peerAlias
          )
      );
    } else {
      m_transfer->setText(
          i18n::tr("control-center.localsend.sending", "name", snap.transfer->peerAlias, "file", snap.transfer->label)
      );
    }
  }
  const bool showTransfer = snap.enabled && snap.transfer.has_value();
  if (snap.enabled && snap.incoming.has_value() && m_catalog != Catalog::Incoming) {
    m_catalog = Catalog::Incoming;
    persistCatalog();
  }
  const bool layoutDirty = setShown(m_pin, snap.enabled) || setShown(m_catalogSwitch, snap.enabled) ||
                            setShown(m_incomingActions, snap.enabled && snap.incoming.has_value()) ||
                            setShown(m_transfer, showTransfer) || setShown(m_liveScroll, snap.enabled);
  syncCatalog();
  if (!snap.enabled) {
    (void)setShown(m_peersCard, false);
    (void)setShown(m_incomingCard, false);
    (void)setShown(m_historyCard, false);
  }
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
  if (snap.enabled) {
    rebuildHistory();
    rebuildPeers();
  }
  m_syncing = false;
}

void LocalSendTab::rebuildPeers() {
  if (m_peerList == nullptr || m_service == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  const auto clip = m_clipboard != nullptr ? m_clipboard->clipboardText() : std::optional<std::string>{};
  const bool hasClipboard = clip.has_value() && !clip->empty();
  std::string signature;
  signature += hasClipboard ? '1' : '0';
  for (const auto& peer : snap.peers) {
    signature += peer.key;
    signature += peer.alias;
    signature += peer.ip;
    signature += peer.deviceType;
    signature += "|";
  }
  if (signature == m_peerSignature && !m_peerList->children().empty()) {
    return;
  }
  m_peerSignature = std::move(signature);
  while (!m_peerList->children().empty()) {
    m_peerList->removeChild(m_peerList->children().front().get());
  }
  const float scale = contentScale();
  if (snap.peers.empty()) {
    m_peerList->addChild(
        ui::label({
            .text = i18n::tr("control-center.localsend.nearby-empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    return;
  }
  for (const auto& peer : snap.peers) {
    auto row = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
    auto header = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = peer.alias.empty() ? peer.ip : peer.alias,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    titles->addChild(
        ui::label({
            .text = peerDetail(peer),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    header->addChild(std::move(titles));
    const float iconSize = Style::controlHeightSm * scale;
    auto iconButton = [scale, iconSize](
                          std::string glyph, bool enabled, std::string tooltip, std::function<void()> onClick
                      ) {
      return ui::button({
          .glyph = std::move(glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = enabled,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .minWidth = iconSize,
          .minHeight = iconSize,
          .padding = Style::spaceXs * scale,
          .onClick = std::move(onClick),
      });
    };
    header->addChild(iconButton("paperclip", true, i18n::tr("control-center.localsend.send-file"), [this, key = peer.key]() {
      sendFileTo(key);
    }));
    header->addChild(iconButton("folder", true, i18n::tr("control-center.localsend.send-folder"), [this, key = peer.key]() {
      sendFolderTo(key);
    }));
    header->addChild(iconButton(
        "clipboard", hasClipboard, i18n::tr("control-center.localsend.send-clipboard"),
        [this, key = peer.key]() { sendClipboardTo(key); }
    ));
    row->addChild(std::move(header));
    m_peerList->addChild(std::move(row));
  }
}

void LocalSendTab::rebuildHistory() {
  if (m_historyList == nullptr || m_service == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  std::string signature;
  for (const auto& item : snap.history) {
    signature += item.alias;
    signature += item.label;
    signature += item.incoming ? "i" : "o";
    signature += item.failed ? "f" : "ok";
    signature += "|";
  }
  if (signature == m_historySignature && !m_historyList->children().empty()) {
    return;
  }
  m_historySignature = std::move(signature);
  while (!m_historyList->children().empty()) {
    m_historyList->removeChild(m_historyList->children().front().get());
  }
  const float scale = contentScale();
  if (snap.history.empty()) {
    m_historyList->addChild(
        ui::label({
            .text = i18n::tr("control-center.localsend.history-empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    return;
  }
  for (const auto& item : snap.history) {
    const char* key = item.failed
        ? (item.incoming ? "control-center.localsend.history-failed-receive"
                         : "control-center.localsend.history-failed-send")
        : (item.incoming ? "control-center.localsend.history-received" : "control-center.localsend.history-sent");
    m_historyList->addChild(
        ui::label({
            .text = i18n::tr(key, "name", item.alias, "file", item.label),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(item.failed ? ColorRole::Error : ColorRole::OnSurfaceVariant),
        })
    );
  }
}
