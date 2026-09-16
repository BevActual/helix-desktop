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
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/clipboard_service.h"

#include <filesystem>
#include <format>
#include <optional>
#include <utility>

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

} // namespace

LocalSendTab::LocalSendTab(const ControlCenterServices& services)
    : m_service(services.localSend), m_clipboard(services.clipboard), m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(services, SettingsPaneOptions{.section = "localsend"})) {
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

std::unique_ptr<Flex> LocalSendTab::create() {
  const float scale = contentScale();

  auto liveScroll = ui::scrollView({
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
  m_rootLayout = liveScroll->content();
  m_rootLayout->setDirection(FlexDirection::Vertical);
  m_rootLayout->setAlign(FlexAlign::Stretch);
  m_rootLayout->setGap(Style::spaceMd * scale);

  auto statusCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*statusCard, i18n::tr("control-center.localsend.title"), scale);
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
  m_rootLayout->addChild(std::move(statusCard));

  auto incoming = ui::column({
      .out = &m_incomingCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*incoming, i18n::tr("control-center.localsend.incoming-title"), scale);
  incoming->addChild(
      ui::label({
          .out = &m_incomingLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  auto incomingRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  incomingRow->addChild(
      ui::button({
          .out = &m_acceptButton,
          .text = i18n::tr("control-center.localsend.accept"),
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
  m_rootLayout->addChild(std::move(incoming));

  m_rootLayout->addChild(
      ui::label({
          .out = &m_transfer,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  auto historyCard = ui::column({
      .out = &m_historyCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*historyCard, i18n::tr("control-center.localsend.history"), scale);
  historyCard->addChild(ui::column({.out = &m_historyList, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale}));
  m_rootLayout->addChild(std::move(historyCard));

  auto peersCard = ui::column({
      .out = &m_peersCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*peersCard, i18n::tr("control-center.localsend.nearby"), scale);
  peersCard->addChild(ui::column({.out = &m_peerList, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale}));
  m_rootLayout->addChild(std::move(peersCard));

  if (m_enabled != nullptr) {
    m_enabled->setOnChange([this](bool value) {
      if (m_syncing || m_config == nullptr) {
        return;
      }
      (void)m_config->setOverride({"localsend", "enabled"}, value);
    });
  }

  auto live = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
  live->addChild(std::move(liveScroll));
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
  m_incomingCard = nullptr;
  m_incomingLabel = nullptr;
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
  if (m_liveScroll == nullptr) {
    return;
  }
  m_liveScroll->setSize(contentWidth, bodyHeight);
  m_liveScroll->layout(renderer);
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
    m_detail->setText(
        snap.ip.empty() ? std::format("HTTP · {}", snap.port) : std::format("{}:{} · HTTP", snap.ip, snap.port)
    );
  } else {
    m_status->setText(snap.alias);
    m_detail->setText(snap.error.empty() ? i18n::tr("control-center.localsend.port-busy") : snap.error);
  }
  if (m_incomingLabel != nullptr && snap.incoming) {
    std::string files;
    for (const auto& file : snap.incoming->files) {
      if (!files.empty()) {
        files += ", ";
      }
      files += file.fileName;
    }
    m_incomingLabel->setText(std::format("{} · {}", snap.incoming->alias, files));
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
  const bool layoutDirty = setShown(m_pin, snap.enabled) || setShown(m_historyCard, snap.enabled)
      || setShown(m_peersCard, snap.enabled) || setShown(m_incomingCard, snap.enabled && snap.incoming.has_value())
      || setShown(m_transfer, showTransfer);
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
  std::string signature;
  for (const auto& peer : snap.peers) {
    signature += peer.key;
    signature += peer.alias;
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
        })
    );
    return;
  }
  for (const auto& peer : snap.peers) {
    auto row = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
    row->addChild(
        ui::label({
            .text = peer.alias.empty() ? peer.ip : peer.alias,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    row->addChild(
        ui::label({
            .text = std::format("{}:{} · {}", peer.ip, peer.port, peer.deviceType),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    actions->addChild(
        ui::button({
            .text = i18n::tr("control-center.localsend.send-file"),
            .glyph = "paperclip",
            .onClick = [this, key = peer.key]() { sendFileTo(key); },
        })
    );
    actions->addChild(
        ui::button({
            .text = i18n::tr("control-center.localsend.send-folder"),
            .glyph = "folder",
            .onClick = [this, key = peer.key]() { sendFolderTo(key); },
        })
    );
    actions->addChild(
        ui::button({
            .text = i18n::tr("control-center.localsend.send-clipboard"),
            .glyph = "clipboard",
            .onClick = [this, key = peer.key]() { sendClipboardTo(key); },
        })
    );
    row->addChild(std::move(actions));
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
