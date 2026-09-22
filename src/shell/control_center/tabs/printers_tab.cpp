#include "shell/control_center/tabs/printers_tab.h"

#include "dbus/cups/printer_service.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
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

#include <filesystem>
#include <optional>
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

  [[nodiscard]] std::string printerTitle(const PrinterInfo& printer) {
    return printer.info.empty() ? printer.name : printer.info;
  }

  [[nodiscard]] std::string printerDetail(const PrinterInfo& printer) {
    std::string out;
    if (printer.isDefault) {
      out = i18n::tr("control-center.printers.default");
    } else if (printer.state == "printing") {
      out = i18n::tr("control-center.printers.printing");
    } else if (printer.state == "paused") {
      out = i18n::tr("control-center.printers.paused");
    } else {
      out = i18n::tr("control-center.printers.idle");
    }
    if (printer.jobCount > 0) {
      out += "  ·  ";
      out += i18n::tr("control-center.printers.jobs", "count", std::to_string(printer.jobCount));
    }
    if (!printer.location.empty()) {
      out += "  ·  ";
      out += printer.location;
    }
    return out;
  }

} // namespace

PrintersTab::PrintersTab(const ControlCenterServices& services) : m_service(services.printers) {
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

std::unique_ptr<Flex> PrintersTab::create() {
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
        applyCompactCardStyle(section, scale, opacity, false);
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
          .out = &m_startButton,
          .text = i18n::tr("control-center.printers.start"),
          .glyph = "printer",
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
  well->addChild(
      ui::button({
          .out = &m_openButton,
          .text = i18n::tr("control-center.printers.open"),
          .glyph = "external-link",
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingH = Style::spaceMd * scale,
          .onClick = []() { (void)net::openInBrowser("http://localhost:631"); },
      })
  );

  auto printersCard = ui::column({
      .out = &m_printersCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  addTitle(*printersCard, i18n::tr("control-center.printers.list"), scale);
  printersCard->addChild(
      ui::column({
          .out = &m_printerList,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
      })
  );
  m_rootLayout->addChild(std::move(well));
  m_rootLayout->addChild(std::move(printersCard));

  auto live = ui::column({
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .flexGrow = 1.0F,
  });
  live->addChild(std::move(liveScroll));
  syncUi();
  return live;
}

std::unique_ptr<Flex> PrintersTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.printers.refresh"),
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

void PrintersTab::setActive(bool active) {
  if (m_service != nullptr) {
    m_service->setWantFastPoll(active);
    if (active) {
      m_service->refresh();
    }
  }
}

void PrintersTab::onClose() {
  if (m_service != nullptr) {
    m_service->setWantFastPoll(false);
  }
  m_rootLayout = nullptr;
  m_status = nullptr;
  m_detail = nullptr;
  m_startButton = nullptr;
  m_openButton = nullptr;
  m_printersCard = nullptr;
  m_printerList = nullptr;
  m_liveScroll = nullptr;
  m_printerSignature.clear();
}

void PrintersTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_liveScroll == nullptr) {
    return;
  }
  m_liveScroll->setSize(contentWidth, bodyHeight);
  m_liveScroll->layout(renderer);
}

void PrintersTab::printTo(const std::string& name) {
  (void)FileDialog::open(
      FileDialogOptions{.title = i18n::tr("control-center.printers.print-file")},
      [this, name](std::optional<std::filesystem::path> path) {
        if (!path || m_service == nullptr) {
          return;
        }
        m_service->printFile(name, path->string());
      }
  );
}

void PrintersTab::syncUi() {
  if (m_status == nullptr) {
    return;
  }

  const PrinterSnapshot empty;
  const PrinterSnapshot& snap = m_service != nullptr ? m_service->snapshot() : empty;
  const bool running = snap.running;
  const bool layoutDirty = setShown(m_startButton, !running) || setShown(m_openButton, running) ||
                            setShown(m_printersCard, running);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }

  if (m_startButton != nullptr) {
    m_startButton->setEnabled(m_service != nullptr && snap.installed && !snap.busy);
  }

  if (!snap.installed) {
    m_status->setText(i18n::tr("control-center.printers.stopped"));
    m_detail->setText(i18n::tr("control-center.printers.missing"));
    return;
  }
  if (!running) {
    m_status->setText(
        snap.busy ? i18n::tr("control-center.printers.starting") : i18n::tr("control-center.printers.stopped")
    );
    m_detail->setText(snap.error.empty() ? i18n::tr("control-center.printers.ready-hint") : snap.error);
    return;
  }

  m_status->setText(i18n::tr("control-center.printers.ready"));
  if (!snap.error.empty()) {
    m_detail->setText(snap.error);
  } else if (snap.printers.empty()) {
    m_detail->setText(i18n::tr("control-center.printers.empty-hint"));
  } else {
    m_detail->setText(i18n::tr("control-center.printers.ready-hint-on"));
  }
  rebuildPrinters();
}

void PrintersTab::rebuildPrinters() {
  if (m_printerList == nullptr || m_service == nullptr) {
    return;
  }
  const auto& snap = m_service->snapshot();
  std::string signature;
  for (const auto& printer : snap.printers) {
    signature += printer.name;
    signature += printer.info;
    signature += printer.state;
    signature += printer.isDefault ? '1' : '0';
    signature += printer.enabled ? '1' : '0';
    signature += std::to_string(printer.jobCount);
    signature += '|';
  }
  if (signature == m_printerSignature && !m_printerList->children().empty()) {
    return;
  }
  m_printerSignature = std::move(signature);
  while (!m_printerList->children().empty()) {
    m_printerList->removeChild(m_printerList->children().front().get());
  }
  const float scale = contentScale();
  if (snap.printers.empty()) {
    m_printerList->addChild(
        ui::label({
            .text = i18n::tr("control-center.printers.empty"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    PanelManager::instance().requestLayout();
    return;
  }

  const float iconSize = Style::controlHeightSm * scale;
  for (const auto& printer : snap.printers) {
    auto row = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
    });
    auto header = ui::row({
        .align = FlexAlign::Center,
        .wrap = true,
        .gap = Style::spaceSm * scale,
    });
    header->addChild(
        ui::glyph({
            .glyph = "printer",
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    auto titles = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = printerTitle(printer),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = printer.isDefault ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    titles->addChild(
        ui::label({
            .text = printerDetail(printer),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
        })
    );
    header->addChild(std::move(titles));
    header->addChild(
        ui::button({
            .glyph = "star",
            .enabled = !printer.isDefault,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.printers.set-default"),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = Style::spaceXs * scale,
            .onClick =
                [this, name = printer.name]() {
                  if (m_service != nullptr) {
                    m_service->setDefault(name);
                  }
                },
        })
    );
    header->addChild(
        ui::button({
            .glyph = printer.enabled ? "player-pause" : "player-play",
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr(
                printer.enabled ? "control-center.printers.pause" : "control-center.printers.resume"
            ),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = Style::spaceXs * scale,
            .onClick =
                [this, name = printer.name, enabled = printer.enabled]() {
                  if (m_service != nullptr) {
                    m_service->setEnabled(name, !enabled);
                  }
                },
        })
    );
    header->addChild(
        ui::button({
            .glyph = "paperclip",
            .enabled = printer.enabled,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.printers.print-file"),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = Style::spaceXs * scale,
            .onClick = [this, name = printer.name]() { printTo(name); },
        })
    );
    header->addChild(
        ui::button({
            .glyph = "x",
            .enabled = printer.jobCount > 0,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.printers.cancel-jobs"),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = Style::spaceXs * scale,
            .onClick =
                [this, name = printer.name]() {
                  if (m_service != nullptr) {
                    m_service->cancelJobs(name);
                  }
                },
        })
    );
    row->addChild(std::move(header));
    m_printerList->addChild(std::move(row));
  }
  PanelManager::instance().requestLayout();
}
