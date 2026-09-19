#include "shell/control_center/tabs/phone_tab.h"

#include "dbus/kdeconnect/phone_connect_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/clipboard_service.h"

#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
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

  [[nodiscard]] const char* glyphForType(std::string_view type) {
    if (type == "tablet") {
      return "device-tablet";
    }
    if (type == "laptop") {
      return "device-laptop";
    }
    if (type == "desktop" || type == "computer") {
      return "device-desktop";
    }
    if (type == "tv") {
      return "device-tv";
    }
    return "device-mobile";
  }

  [[nodiscard]] std::string deviceDetail(const PhoneDevice& device) {
    if (device.pairRequestedByPeer) {
      if (!device.verificationKey.empty()) {
        return i18n::tr("control-center.phone.pair-incoming-code", "code", device.verificationKey);
      }
      return i18n::tr("control-center.phone.pair-incoming");
    }
    if (device.pairRequested) {
      if (!device.verificationKey.empty()) {
        return i18n::tr("control-center.phone.pair-waiting-code", "code", device.verificationKey);
      }
      return i18n::tr("control-center.phone.pair-waiting");
    }
    if (!device.paired) {
      return device.reachable ? i18n::tr("control-center.phone.available") : i18n::tr("control-center.phone.offline");
    }
    std::string out =
        device.reachable ? i18n::tr("control-center.phone.online") : i18n::tr("control-center.phone.offline");
    if (device.hasBattery && device.charge >= 0) {
      out += "  ·  ";
      out += std::to_string(device.charge);
      out += "%";
      if (device.charging) {
        out += "  ·  ";
        out += i18n::tr("control-center.phone.charging");
      }
    }
    return out;
  }

} // namespace

PhoneTab::PhoneTab(const ControlCenterServices& services)
    : m_service(services.phone), m_clipboard(services.clipboard) {
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

std::unique_ptr<Flex> PhoneTab::create() {
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
  well->addChild(
      ui::button({
          .out = &m_activateButton,
          .text = i18n::tr("control-center.phone.start"),
          .glyph = "device-mobile",
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->tryActivate();
                }
              },
      })
  );

  auto devicesCard = ui::column({
      .out = &m_devicesCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*devicesCard, i18n::tr("control-center.phone.devices"), scale);
  devicesCard->addChild(
      ui::column({
          .out = &m_deviceList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );
  m_rootLayout->addChild(std::move(well));
  m_rootLayout->addChild(std::move(devicesCard));

  auto live = ui::column({
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });
  live->addChild(std::move(liveScroll));
  syncUi();
  return live;
}

std::unique_ptr<Flex> PhoneTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.phone.refresh"),
          .onClick =
              [this]() {
                if (m_service == nullptr) {
                  return;
                }
                if (!m_service->available()) {
                  m_service->tryActivate();
                }
                m_service->refresh();
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void PhoneTab::setActive(bool active) {
  if (m_service == nullptr) {
    return;
  }
  m_service->setDiscovery(active);
  if (active) {
    if (!m_service->available()) {
      m_service->tryActivate();
    }
    m_service->refresh();
  }
}

void PhoneTab::onClose() {
  if (m_service != nullptr) {
    m_service->setDiscovery(false);
  }
  m_rootLayout = nullptr;
  m_status = nullptr;
  m_detail = nullptr;
  m_activateButton = nullptr;
  m_devicesCard = nullptr;
  m_deviceList = nullptr;
  m_liveScroll = nullptr;
  m_deviceSignature.clear();
}

void PhoneTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_liveScroll == nullptr) {
    return;
  }
  m_liveScroll->setSize(contentWidth, bodyHeight);
  m_liveScroll->layout(renderer);
}

void PhoneTab::sendFileTo(const std::string& deviceId) {
  if (m_service == nullptr) {
    return;
  }
  (void)FileDialog::open(
      FileDialogOptions{.title = i18n::tr("control-center.phone.send-file")},
      [this, deviceId](std::optional<std::filesystem::path> path) {
        if (!path || m_service == nullptr) {
          return;
        }
        m_service->sharePath(deviceId, path->string());
      }
  );
}

void PhoneTab::sendClipboardTo(const std::string& deviceId) {
  if (m_service == nullptr || m_clipboard == nullptr) {
    return;
  }
  const auto text = m_clipboard->clipboardText();
  if (!text || text->empty()) {
    return;
  }
  m_service->shareText(deviceId, *text);
}

void PhoneTab::syncUi() {
  if (m_status == nullptr) {
    return;
  }

  const PhoneSnapshot empty;
  const PhoneSnapshot& snap = m_service != nullptr ? m_service->snapshot() : empty;
  const bool available = snap.available;

  const bool layoutDirty = setShown(m_devicesCard, available) ||
                            setShown(m_activateButton, !available);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }

  if (!available) {
    m_status->setText(
        snap.activating ? i18n::tr("control-center.phone.starting") : i18n::tr("control-center.phone.stopped")
    );
    m_detail->setText(snap.error.empty() ? i18n::tr("control-center.phone.missing") : snap.error);
    if (m_activateButton != nullptr) {
      m_activateButton->setEnabled(m_service != nullptr && !snap.activating);
    }
    return;
  }

  m_status->setText(
      snap.announcedName.empty() ? i18n::tr("control-center.phone.ready")
                                 : i18n::tr("control-center.phone.visible-as", "name", snap.announcedName)
  );
  m_detail->setText(
      snap.error.empty() ? i18n::tr("control-center.phone.ready-hint") : snap.error
  );
  rebuildDevices();
}

void PhoneTab::rebuildDevices() {
  if (m_deviceList == nullptr || m_service == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  std::string signature;
  for (const auto& device : snap.devices) {
    signature += device.id;
    signature += '\n';
    signature += device.name;
    signature += device.type;
    signature += device.reachable ? '1' : '0';
    signature += device.paired ? '1' : '0';
    signature += device.pairRequested ? '1' : '0';
    signature += device.pairRequestedByPeer ? '1' : '0';
    signature += device.verificationKey;
    signature += std::to_string(device.charge);
    signature += device.charging ? '1' : '0';
    signature += '|';
  }
  {
    const auto clip = m_clipboard != nullptr ? m_clipboard->clipboardText() : std::optional<std::string>{};
    signature += (clip.has_value() && !clip->empty()) ? '1' : '0';
  }
  if (signature == m_deviceSignature && !m_deviceList->children().empty()) {
    return;
  }
  m_deviceSignature = std::move(signature);
  while (!m_deviceList->children().empty()) {
    m_deviceList->removeChild(m_deviceList->children().front().get());
  }

  const float scale = contentScale();
  if (snap.devices.empty()) {
    m_deviceList->addChild(
        ui::label({
            .text = i18n::tr("control-center.phone.devices-empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    return;
  }

  for (const auto& device : snap.devices) {
    auto row = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
    auto header = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
    header->addChild(
        ui::glyph({
            .glyph = glyphForType(device.type),
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = device.name.empty() ? device.id : device.name,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = device.reachable ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    titles->addChild(
        ui::label({
            .text = deviceDetail(device),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    header->addChild(std::move(titles));
    if (device.paired) {
      const bool live = device.reachable;
      const auto clip = m_clipboard != nullptr ? m_clipboard->clipboardText() : std::optional<std::string>{};
      const bool hasClipboard = clip.has_value() && !clip->empty();
      const float iconSize = Style::controlHeightSm * scale;
      auto iconButton = [scale, iconSize](
                            std::string glyph, bool enabled, std::string tooltip, std::function<void()> onClick
                        ) {
        return ui::button({
            .glyph = std::move(glyph),
            .enabled = enabled,
            .variant = ButtonVariant::Ghost,
            .tooltip = std::move(tooltip),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = Style::spaceXs * scale,
            .onClick = std::move(onClick),
        });
      };
      header->addChild(iconButton("bell", live, i18n::tr("control-center.phone.ping"), [this, id = device.id]() {
        if (m_service != nullptr) {
          m_service->ping(id);
        }
      }));
      header->addChild(iconButton("bell-ringing", live, i18n::tr("control-center.phone.ring"), [this, id = device.id]() {
        if (m_service != nullptr) {
          m_service->ring(id);
        }
      }));
      header->addChild(iconButton("paperclip", live, i18n::tr("control-center.phone.send-file"), [this, id = device.id]() {
        sendFileTo(id);
      }));
      header->addChild(iconButton(
          "clipboard", live && hasClipboard, i18n::tr("control-center.phone.send-clipboard"),
          [this, id = device.id]() { sendClipboardTo(id); }
      ));
      header->addChild(iconButton("unlink", true, i18n::tr("control-center.phone.unpair"), [this, id = device.id]() {
        if (m_service != nullptr) {
          m_service->unpair(id);
        }
      }));
    }
    row->addChild(std::move(header));

    if (device.pairRequestedByPeer) {
      auto actions = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
      actions->addChild(
          ui::button({
              .text = i18n::tr("control-center.phone.accept"),
              .glyph = "check",
              .variant = ButtonVariant::Primary,
              .onClick = [this, id = device.id]() {
                if (m_service != nullptr) {
                  m_service->acceptPair(id);
                }
              },
          })
      );
      actions->addChild(
          ui::button({
              .text = i18n::tr("control-center.phone.reject"),
              .glyph = "x",
              .variant = ButtonVariant::Destructive,
              .onClick = [this, id = device.id]() {
                if (m_service != nullptr) {
                  m_service->rejectPair(id);
                }
              },
          })
      );
      row->addChild(std::move(actions));
    } else if (!device.paired) {
      auto actions = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
      actions->addChild(
          ui::button({
              .text = i18n::tr("control-center.phone.pair"),
              .glyph = "link",
              .enabled = device.reachable,
              .onClick = [this, id = device.id]() {
                if (m_service != nullptr) {
                  m_service->requestPair(id);
                }
              },
          })
      );
      row->addChild(std::move(actions));
    }
    m_deviceList->addChild(std::move(row));
  }
}
