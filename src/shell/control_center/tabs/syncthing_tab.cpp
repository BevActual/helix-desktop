#include "shell/control_center/tabs/syncthing_tab.h"

#include "i18n/i18n.h"
#include "net/syncthing_service.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/clipboard_service.h"

#include <string>
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

  [[nodiscard]] std::string folderName(const SyncthingFolder& folder) {
    return folder.label.empty() ? folder.id : folder.label;
  }

  [[nodiscard]] std::string folderTypeLabel(std::string_view type) {
    if (type == "sendonly") {
      return i18n::tr("control-center.syncthing.type-send");
    }
    if (type == "receiveonly") {
      return i18n::tr("control-center.syncthing.type-receive");
    }
    if (type == "receiveencrypted") {
      return i18n::tr("control-center.syncthing.type-encrypted");
    }
    return i18n::tr("control-center.syncthing.type-both");
  }

  [[nodiscard]] std::string folderDetail(const SyncthingFolder& folder) {
    if (folder.paused) {
      return i18n::tr("control-center.syncthing.paused");
    }
    std::string out;
    if (folder.state == "scanning") {
      out = i18n::tr("control-center.syncthing.scanning");
    } else if (folder.state == "syncing" || folder.needBytes > 0) {
      out = i18n::tr("control-center.syncthing.syncing");
      const int percent = static_cast<int>(folder.completion + 0.5);
      if (percent >= 0 && percent < 100) {
        out += "  ·  ";
        out += std::to_string(percent);
        out += "%";
      }
    } else if (folder.state == "error") {
      out = i18n::tr("control-center.syncthing.folder-error");
    } else {
      out = i18n::tr("control-center.syncthing.up-to-date");
    }
    out += "  ·  ";
    out += folderTypeLabel(folder.type);
    return out;
  }

  [[nodiscard]] std::string shortDeviceId(std::string_view id) {
    const auto dash = id.find('-');
    if (dash != std::string_view::npos && dash >= 5) {
      return std::string(id.substr(0, dash));
    }
    if (id.size() > 7) {
      return std::string(id.substr(0, 7));
    }
    return std::string(id);
  }

} // namespace

SyncthingTab::SyncthingTab(const ControlCenterServices& services)
    : m_service(services.syncthing), m_clipboard(services.clipboard) {
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

std::unique_ptr<Flex> SyncthingTab::create() {
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

  auto well = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& section) {
        applySectionCardStyle(section, scale, opacity);
      },
  });
  addTitle(*well, i18n::tr("control-center.syncthing.title"), scale);
  well->addChild(
      ui::label({
          .out = &m_status,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  well->addChild(
      ui::label({
          .out = &m_detail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );

  auto actions = ui::row({
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceSm * scale,
  });
  actions->addChild(
      ui::button({
          .out = &m_startButton,
          .text = i18n::tr("control-center.syncthing.start"),
          .glyph = "folder",
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->tryStart();
                }
              },
      })
  );
  actions->addChild(
      ui::button({
          .out = &m_stopButton,
          .text = i18n::tr("control-center.syncthing.stop"),
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->shutdown();
                }
              },
      })
  );
  actions->addChild(
      ui::button({
          .out = &m_guiButton,
          .text = i18n::tr("control-center.syncthing.open"),
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->openGui();
                }
              },
      })
  );
  actions->addChild(
      ui::button({
          .out = &m_copyIdButton,
          .glyph = "copy",
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.syncthing.copy-id"),
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick = [this]() { copyDeviceId(); },
      })
  );
  well->addChild(std::move(actions));

  m_foldersDivider = addWellDivider(*well, scale);
  auto foldersCard = ui::column({
      .out = &m_foldersCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  addTitle(*foldersCard, i18n::tr("control-center.syncthing.folders"), scale);
  foldersCard->addChild(
      ui::column({
          .out = &m_folderList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );
  well->addChild(std::move(foldersCard));

  m_remotesDivider = addWellDivider(*well, scale);
  auto remotesCard = ui::column({
      .out = &m_remotesCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  addTitle(*remotesCard, i18n::tr("control-center.syncthing.devices"), scale);
  remotesCard->addChild(
      ui::column({
          .out = &m_remoteList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );
  well->addChild(std::move(remotesCard));
  m_rootLayout->addChild(std::move(well));

  auto live = ui::column({
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });
  live->addChild(std::move(liveScroll));
  syncUi();
  return live;
}

std::unique_ptr<Flex> SyncthingTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.syncthing.refresh"),
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->refresh();
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void SyncthingTab::setActive(bool active) {
  if (m_service != nullptr) {
    m_service->setWantFastPoll(active);
    if (active) {
      m_service->refresh();
    }
  }
}

void SyncthingTab::onClose() {
  if (m_service != nullptr) {
    m_service->setWantFastPoll(false);
  }
  m_rootLayout = nullptr;
  m_status = nullptr;
  m_detail = nullptr;
  m_startButton = nullptr;
  m_stopButton = nullptr;
  m_guiButton = nullptr;
  m_copyIdButton = nullptr;
  m_foldersCard = nullptr;
  m_foldersDivider = nullptr;
  m_folderList = nullptr;
  m_remotesCard = nullptr;
  m_remotesDivider = nullptr;
  m_remoteList = nullptr;
  m_liveScroll = nullptr;
  m_folderSignature.clear();
  m_remoteSignature.clear();
}

void SyncthingTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_liveScroll == nullptr) {
    return;
  }
  m_liveScroll->setSize(contentWidth, bodyHeight);
  m_liveScroll->layout(renderer);
}

void SyncthingTab::copyDeviceId() {
  if (m_service == nullptr || m_clipboard == nullptr) {
    return;
  }
  const std::string& id = m_service->snapshot().myId;
  if (id.empty()) {
    return;
  }
  m_clipboard->setClipboardText(id);
}

void SyncthingTab::syncUi() {
  if (m_status == nullptr) {
    return;
  }

  const SyncthingSnapshot empty;
  const SyncthingSnapshot& snap = m_service != nullptr ? m_service->snapshot() : empty;
  const bool running = snap.running;
  const bool layoutDirty = setShown(m_foldersDivider, running) || setShown(m_foldersCard, running) ||
                            setShown(m_remotesDivider, running) || setShown(m_remotesCard, running) ||
                            setShown(m_startButton, !running) || setShown(m_stopButton, running) ||
                            setShown(m_guiButton, running) || setShown(m_copyIdButton, running && !snap.myId.empty());
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }

  if (m_startButton != nullptr) {
    m_startButton->setEnabled(m_service != nullptr && snap.installed && !snap.busy);
  }
  if (m_stopButton != nullptr) {
    m_stopButton->setEnabled(m_service != nullptr && !snap.busy);
  }

  if (!snap.installed) {
    m_status->setText(i18n::tr("control-center.syncthing.stopped"));
    m_detail->setText(i18n::tr("control-center.syncthing.missing"));
    return;
  }
  if (!running) {
    m_status->setText(
        snap.busy ? i18n::tr("control-center.syncthing.starting") : i18n::tr("control-center.syncthing.stopped")
    );
    m_detail->setText(snap.error.empty() ? i18n::tr("control-center.syncthing.ready-hint") : snap.error);
    return;
  }

  m_status->setText(
      snap.myName.empty() ? i18n::tr("control-center.syncthing.ready")
                          : i18n::tr("control-center.syncthing.visible-as", "name", snap.myName)
  );
  std::string detail = snap.error.empty()
                           ? i18n::tr(
                                 "control-center.syncthing.connected-count", "count",
                                 std::to_string(snap.connectedCount)
                             )
                           : snap.error;
  if (!snap.myId.empty()) {
    if (!detail.empty()) {
      detail += "  ·  ";
    }
    detail += shortDeviceId(snap.myId);
  }
  m_detail->setText(detail);
  rebuildFolders();
  rebuildRemotes();
}

void SyncthingTab::rebuildFolders() {
  if (m_folderList == nullptr || m_service == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  std::string signature;
  for (const auto& folder : snap.folders) {
    signature += folder.id;
    signature += folder.label;
    signature += folder.path;
    signature += folder.type;
    signature += folder.state;
    signature += folder.paused ? '1' : '0';
    signature += std::to_string(static_cast<int>(folder.completion));
    signature += std::to_string(folder.needBytes);
    signature += '|';
  }
  if (signature == m_folderSignature && !m_folderList->children().empty()) {
    return;
  }
  m_folderSignature = std::move(signature);
  while (!m_folderList->children().empty()) {
    m_folderList->removeChild(m_folderList->children().front().get());
  }

  const float scale = contentScale();
  if (snap.folders.empty()) {
    m_folderList->addChild(
        ui::label({
            .text = i18n::tr("control-center.syncthing.folders-empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    return;
  }

  for (const auto& folder : snap.folders) {
    auto row = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
    auto header = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
    header->addChild(
        ui::glyph({
            .glyph = "folder",
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = folderName(folder),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = folder.paused ? FontWeight::Normal : FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    titles->addChild(
        ui::label({
            .text = folderDetail(folder),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    header->addChild(std::move(titles));
    const float iconSize = Style::controlHeightSm * scale;
    header->addChild(
        ui::button({
            .glyph = "refresh",
            .enabled = !folder.paused,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.syncthing.scan"),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = Style::spaceXs * scale,
            .onClick =
                [this, id = folder.id]() {
                  if (m_service != nullptr) {
                    m_service->scanFolder(id);
                  }
                },
        })
    );
    header->addChild(
        ui::button({
            .glyph = folder.paused ? "player-play" : "player-pause",
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr(
                folder.paused ? "control-center.syncthing.resume" : "control-center.syncthing.pause"
            ),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = Style::spaceXs * scale,
            .onClick =
                [this, id = folder.id, paused = folder.paused]() {
                  if (m_service != nullptr) {
                    m_service->setFolderPaused(id, !paused);
                  }
                },
        })
    );
    row->addChild(std::move(header));
    m_folderList->addChild(std::move(row));
  }
}

void SyncthingTab::rebuildRemotes() {
  if (m_remoteList == nullptr || m_service == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  std::string signature;
  for (const auto& remote : snap.remotes) {
    signature += remote.id;
    signature += remote.name;
    signature += remote.connected ? '1' : '0';
    signature += remote.paused ? '1' : '0';
    signature += '|';
  }
  if (signature == m_remoteSignature && !m_remoteList->children().empty()) {
    return;
  }
  m_remoteSignature = std::move(signature);
  while (!m_remoteList->children().empty()) {
    m_remoteList->removeChild(m_remoteList->children().front().get());
  }

  const float scale = contentScale();
  if (snap.remotes.empty()) {
    m_remoteList->addChild(
        ui::label({
            .text = i18n::tr("control-center.syncthing.devices-empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    return;
  }

  for (const auto& remote : snap.remotes) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    row->addChild(
        ui::glyph({
            .glyph = "device-laptop",
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = remote.name.empty() ? shortDeviceId(remote.id) : remote.name,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = remote.connected ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    titles->addChild(
        ui::label({
            .text = remote.paused ? i18n::tr("control-center.syncthing.paused")
                                 : (remote.connected ? i18n::tr("control-center.syncthing.online")
                                                    : i18n::tr("control-center.syncthing.offline")),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    row->addChild(std::move(titles));
    m_remoteList->addChild(std::move(row));
  }
}
