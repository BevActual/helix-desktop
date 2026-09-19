#include "shell/control_center/tabs/ai_usage_tab.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/ai_usage_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <string>

using namespace control_center;
using noctalia::ai_usage::OllamaStatus;
using noctalia::ai_usage::ProviderId;
using noctalia::ai_usage::Status;
using noctalia::ai_usage::WindowKey;

namespace {

  [[nodiscard]] std::string providerName(ProviderId id) {
    switch (id) {
    case ProviderId::Claude:
      return i18n::tr("control-center.ai-usage.providers.claude");
    case ProviderId::Codex:
      return i18n::tr("control-center.ai-usage.providers.codex");
    case ProviderId::Gemini:
      return i18n::tr("control-center.ai-usage.providers.gemini");
    case ProviderId::Qwen:
      return i18n::tr("control-center.ai-usage.providers.qwen");
    case ProviderId::Copilot:
      return i18n::tr("control-center.ai-usage.providers.copilot");
    case ProviderId::Cursor:
      return i18n::tr("control-center.ai-usage.providers.cursor");
    case ProviderId::DeepSeek:
      return i18n::tr("control-center.ai-usage.providers.deepseek");
    }
    return {};
  }

  [[nodiscard]] std::string windowName(WindowKey key) {
    switch (key) {
    case WindowKey::Session:
      return i18n::tr("control-center.ai-usage.windows.session");
    case WindowKey::Weekly:
      return i18n::tr("control-center.ai-usage.windows.weekly");
    case WindowKey::Day:
      return i18n::tr("control-center.ai-usage.windows.day");
    case WindowKey::Premium:
      return i18n::tr("control-center.ai-usage.windows.premium");
    case WindowKey::Chat:
      return i18n::tr("control-center.ai-usage.windows.chat");
    case WindowKey::Completions:
      return i18n::tr("control-center.ai-usage.windows.completions");
    }
    return {};
  }

  [[nodiscard]] std::string statusText(Status status) {
    switch (status) {
    case Status::Ok:
      return {};
    case Status::Absent:
      return i18n::tr("control-center.ai-usage.status.absent");
    case Status::Disabled:
      return i18n::tr("control-center.ai-usage.status.disabled");
    case Status::NoCredentials:
      return i18n::tr("control-center.ai-usage.status.no-credentials");
    case Status::NoAuth:
      return i18n::tr("control-center.ai-usage.status.no-auth");
    case Status::Expired:
      return i18n::tr("control-center.ai-usage.status.expired");
    case Status::NoData:
      return i18n::tr("control-center.ai-usage.status.no-data");
    case Status::HttpError:
      return i18n::tr("control-center.ai-usage.status.http-error");
    case Status::Unsupported:
      return i18n::tr("control-center.ai-usage.status.unsupported");
    }
    return {};
  }

  [[nodiscard]] std::string ollamaStatusText(OllamaStatus status) {
    switch (status) {
    case OllamaStatus::Absent:
      return i18n::tr("control-center.ai-usage.ollama.absent");
    case OllamaStatus::Disabled:
      return i18n::tr("control-center.ai-usage.ollama.disabled");
    case OllamaStatus::Stopped:
      return i18n::tr("control-center.ai-usage.ollama.stopped");
    case OllamaStatus::Running:
      return i18n::tr("control-center.ai-usage.ollama.running");
    case OllamaStatus::Error:
      return i18n::tr("control-center.ai-usage.ollama.error");
    }
    return {};
  }

  [[nodiscard]] ColorRole barRole(float percent) {
    if (percent >= 90.0F) {
      return ColorRole::Error;
    }
    if (percent >= 75.0F) {
      return ColorRole::Secondary;
    }
    return ColorRole::Primary;
  }

  [[nodiscard]] bool hiddenStatus(Status status) {
    return status == Status::Absent || status == Status::Disabled;
  }

  [[nodiscard]] bool ollamaHidden(OllamaStatus status) {
    return status == OllamaStatus::Absent || status == OllamaStatus::Disabled;
  }

  [[nodiscard]] std::string formatReset(std::chrono::system_clock::time_point reset) {
    const auto now = std::chrono::system_clock::now();
    if (reset <= now) {
      return i18n::tr("control-center.ai-usage.now");
    }
    return i18n::tr(
        "control-center.ai-usage.resets-in", "time",
        formatDuration(std::chrono::duration_cast<std::chrono::seconds>(reset - now))
    );
  }

} // namespace

AiUsageTab::AiUsageTab(const ControlCenterServices& services)
    : m_service(services.aiUsage),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "control-center", .includedGroups = {"ai-usage"}}
      )) {
  if (m_service != nullptr) {
    m_service->setChangeCallback([this]() {
      syncUi();
      PanelManager::instance().refresh();
    });
  }
}

std::unique_ptr<Flex> AiUsageTab::create() {
  const float scale = contentScale();
  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
      .fillHeight = true,
  });

  auto compact = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { applySectionCardStyle(card, scale, opacity); },
  });
  compact->addChild(
      ui::label({
          .out = &m_empty,
          .text = i18n::tr("control-center.ai-usage.empty"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
          .visible = false,
      })
  );
  compact->addChild(
      ui::label({
          .out = &m_updated,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  );
  tab->addChild(std::move(compact));

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure =
          [](ScrollView& view) {
            view.clearFill();
            view.clearBorder();
          },
  });
  auto list = ui::column({
      .out = &m_list,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });

  auto ollamaCard = ui::column({
      .out = &m_ollamaCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceMd * scale,
      .visible = false,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  ollamaCard->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
          ui::glyph({
              .glyph = "server",
              .glyphSize = Style::fontSizeTitle * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          }),
          ui::label({
              .text = i18n::tr("control-center.ai-usage.ollama.title"),
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .flexGrow = 1.0F,
          }),
          ui::label({
              .out = &m_ollamaVersion,
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::Primary),
              .maxLines = 1,
              .visible = false,
          })
      )
  );
  ollamaCard->addChild(
      ui::label({
          .out = &m_ollamaStatus,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  ollamaCard->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::button({
              .out = &m_ollamaStart,
              .text = i18n::tr("control-center.ai-usage.ollama.start"),
              .variant = ButtonVariant::Primary,
              .minHeight = Style::controlHeightSm * scale,
              .onClick =
                  [this]() {
                    if (m_service != nullptr) {
                      m_service->startOllama();
                    }
                  },
          }),
          ui::button({
              .out = &m_ollamaStop,
              .text = i18n::tr("control-center.ai-usage.ollama.stop"),
              .variant = ButtonVariant::Destructive,
              .minHeight = Style::controlHeightSm * scale,
              .onClick =
                  [this]() {
                    if (m_service != nullptr) {
                      m_service->stopOllama();
                    }
                  },
          })
      )
  );
  for (auto& model : m_ollamaModels) {
    auto row = ui::row({
        .out = &model.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
        .visible = false,
    });
    row->addChild(
        ui::label({
            .out = &model.name,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .flexGrow = 1.0F,
        })
    );
    row->addChild(
        ui::label({
            .out = &model.meta,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );
    ollamaCard->addChild(std::move(row));
  }
  ollamaCard->addChild(
      ui::label({
          .out = &m_ollamaMore,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .visible = false,
      })
  );
  list->addChild(std::move(ollamaCard));

  for (std::size_t i = 0; i < m_providers.size(); ++i) {
    auto& widgets = m_providers[i];
    const auto id = static_cast<ProviderId>(i);
    auto card = ui::column({
        .out = &widgets.card,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceMd * scale,
        .visible = false,
        .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
    });
    card->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
            ui::glyph({
                .out = &widgets.glyph,
                .glyph = std::string(noctalia::ai_usage::providerGlyph(id)),
                .glyphSize = Style::fontSizeTitle * scale,
                .color = colorSpecFromRole(ColorRole::Primary),
            }),
            ui::label({
                .out = &widgets.name,
                .text = providerName(id),
                .fontSize = Style::fontSizeBody * scale,
                .fontWeight = FontWeight::Bold,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .maxLines = 1,
                .flexGrow = 1.0F,
            }),
            ui::label({
                .out = &widgets.plan,
                .fontSize = Style::fontSizeCaption * scale,
                .fontWeight = FontWeight::Medium,
                .color = colorSpecFromRole(ColorRole::Primary),
                .maxLines = 1,
                .visible = false,
            }),
            ui::button({
                .out = &widgets.openButton,
                .text = i18n::tr("control-center.ai-usage.open"),
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeightSm * scale,
                .padding = Style::spaceXs * scale,
                .visible = false,
                .onClick =
                    [this, id]() {
                      if (m_service != nullptr) {
                        m_service->launchProvider(id);
                      }
                    },
            })
        )
    );
    card->addChild(
        ui::label({
            .out = &widgets.status,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
            .visible = false,
        })
    );
    for (auto& window : widgets.windows) {
      auto row = ui::column({
          .out = &window.row,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .fillWidth = true,
          .visible = false,
      });
      row->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
              ui::label({
                  .out = &window.name,
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  .flexGrow = 1.0F,
              }),
              ui::label({
                  .out = &window.caption,
                  .fontSize = Style::fontSizeCaption * scale,
                  .fontWeight = FontWeight::Medium,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 1,
              })
          )
      );
      row->addChild(
          ui::progressBar({
              .out = &window.bar,
              .fill = colorSpecFromRole(ColorRole::Primary),
              .track = colorSpecFromRole(ColorRole::Surface),
              .radius = Style::sliderTrackHeight * scale * 0.5F,
              .progress = 0.0F,
              .height = Style::sliderTrackHeight * scale,
          })
      );
      card->addChild(std::move(row));
    }
    list->addChild(std::move(card));
  }

  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceSm * scale);
  content->addChild(std::move(list));
  tab->addChild(std::move(scroll));
  m_paletteConn = paletteChanged().connect([this] { syncUi(); });
  syncUi();
  return m_settings->wrapLive(std::move(tab), scale, panelCardOpacity());
}

std::unique_ptr<Flex> AiUsageTab::createHeaderActions() {
  const float scale = contentScale();
  auto live = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::button({
          .out = &m_refreshButton,
          .glyph = "refresh",
          .tooltip = i18n::tr("control-center.ai-usage.refresh"),
          .onClick =
              [this]() {
                if (m_service != nullptr) {
                  m_service->refresh(true);
                }
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  return m_settings->embedHeader(std::move(live), scale);
}

void AiUsageTab::onClose() {
  m_paletteConn = {};
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_root = nullptr;
  m_scroll = nullptr;
  m_list = nullptr;
  m_ollamaCard = nullptr;
  m_ollamaStatus = nullptr;
  m_ollamaVersion = nullptr;
  m_ollamaStart = nullptr;
  m_ollamaStop = nullptr;
  m_ollamaModels = {};
  m_ollamaMore = nullptr;
  m_empty = nullptr;
  m_updated = nullptr;
  m_refreshButton = nullptr;
  m_providers = {};
}

void AiUsageTab::setActive(bool active) {
  if (active && m_service != nullptr) {
    m_service->refresh(false);
    syncUi();
  }
}

void AiUsageTab::setConfigureMode(bool show) { m_settings->setConfigureMode(show); }

bool AiUsageTab::configureMode() const { return m_settings->configureMode(); }

void AiUsageTab::setSettingsSearchQuery(std::string_view query) { m_settings->setSearchQuery(std::string(query)); }

void AiUsageTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_settings != nullptr && m_settings->layoutEmbedded(renderer, contentWidth, bodyHeight)) {
    return;
  }
  (void)renderer;
  (void)contentWidth;
  (void)bodyHeight;
}

void AiUsageTab::onPanelCardOpacityChanged(float opacity) {
  // Provider and Ollama cards are leftover wells; their fill does not follow panel glass.
  (void)opacity;
}

void AiUsageTab::syncUi() {
  if (m_root == nullptr) {
    return;
  }
  const auto* snapshot = m_service != nullptr ? &m_service->snapshot() : nullptr;
  bool any = false;
  std::chrono::system_clock::time_point newest{};

  const auto* ollama = snapshot != nullptr ? &snapshot->ollama : nullptr;
  const bool showOllama = ollama != nullptr && !ollamaHidden(ollama->status);
  if (m_ollamaCard != nullptr) {
    m_ollamaCard->setVisible(showOllama);
    m_ollamaCard->setParticipatesInLayout(showOllama);
  }
  if (showOllama && ollama != nullptr) {
    any = true;
    if (m_ollamaStatus != nullptr) {
      m_ollamaStatus->setText(ollamaStatusText(ollama->status));
    }
    if (m_ollamaVersion != nullptr) {
      m_ollamaVersion->setText(ollama->version);
      m_ollamaVersion->setVisible(!ollama->version.empty());
      m_ollamaVersion->setParticipatesInLayout(!ollama->version.empty());
    }
    const bool canStart =
        ollama->hasCli && ollama->local && ollama->status == OllamaStatus::Stopped && !ollama->busy;
    const bool canStop = ollama->local && ollama->status == OllamaStatus::Running && !ollama->busy;
    if (m_ollamaStart != nullptr) {
      m_ollamaStart->setVisible(canStart);
      m_ollamaStart->setParticipatesInLayout(canStart);
      m_ollamaStart->setEnabled(canStart);
    }
    if (m_ollamaStop != nullptr) {
      m_ollamaStop->setVisible(canStop);
      m_ollamaStop->setParticipatesInLayout(canStop);
      m_ollamaStop->setEnabled(canStop);
    }
    const std::size_t shown = std::min(ollama->models.size(), m_ollamaModels.size());
    for (std::size_t i = 0; i < m_ollamaModels.size(); ++i) {
      auto& row = m_ollamaModels[i];
      const bool show = i < shown;
      if (row.row != nullptr) {
        row.row->setVisible(show);
        row.row->setParticipatesInLayout(show);
      }
      if (!show) {
        continue;
      }
      const auto& model = ollama->models[i];
      if (row.name != nullptr) {
        row.name->setText(model.name);
      }
      std::string meta;
      if (model.size > 0) {
        meta = noctalia::ai_usage::formatByteSize(model.size);
      }
      if (model.loaded) {
        if (!meta.empty()) {
          meta += " · ";
        }
        meta += i18n::tr("control-center.ai-usage.ollama.loaded");
      }
      if (row.meta != nullptr) {
        row.meta->setText(meta);
      }
    }
    if (m_ollamaMore != nullptr) {
      if (ollama->models.size() > shown) {
        m_ollamaMore->setText(
            i18n::tr("control-center.ai-usage.ollama.more", "count", std::to_string(ollama->models.size() - shown))
        );
        m_ollamaMore->setVisible(true);
        m_ollamaMore->setParticipatesInLayout(true);
      } else {
        m_ollamaMore->setVisible(false);
        m_ollamaMore->setParticipatesInLayout(false);
      }
    }
  }

  for (std::size_t i = 0; i < m_providers.size(); ++i) {
    auto& widgets = m_providers[i];
    const noctalia::ai_usage::ProviderSnapshot* provider = nullptr;
    if (snapshot != nullptr && i < snapshot->providers.size()) {
      provider = &snapshot->providers[i];
    }
    const bool show = provider != nullptr && !hiddenStatus(provider->status);
    if (widgets.card != nullptr) {
      widgets.card->setVisible(show);
      widgets.card->setParticipatesInLayout(show);
    }
    if (!show || provider == nullptr) {
      continue;
    }
    any = true;
    if (provider->fetchedAt > newest) {
      newest = provider->fetchedAt;
    }
    if (widgets.plan != nullptr) {
      widgets.plan->setText(provider->plan);
      widgets.plan->setVisible(!provider->plan.empty());
    }
    if (widgets.openButton != nullptr) {
      widgets.openButton->setVisible(provider->launchable);
      widgets.openButton->setParticipatesInLayout(provider->launchable);
    }
    const bool ok = provider->status == Status::Ok;
    if (widgets.status != nullptr) {
      if (ok && provider->windows.empty()) {
        widgets.status->setText(i18n::tr("control-center.ai-usage.balance-only"));
        widgets.status->setVisible(true);
      } else if (!ok) {
        widgets.status->setText(statusText(provider->status));
        widgets.status->setVisible(true);
      } else if (provider->stale) {
        widgets.status->setText(i18n::tr("control-center.ai-usage.status.stale"));
        widgets.status->setVisible(true);
      } else if (provider->dataAt.has_value()) {
        const auto age = std::chrono::system_clock::now() - *provider->dataAt;
        if (age > std::chrono::hours(2)) {
          widgets.status->setText(i18n::tr("control-center.ai-usage.as-of", "time", formatTimeAgo(*provider->dataAt)));
          widgets.status->setVisible(true);
        } else {
          widgets.status->setVisible(false);
        }
      } else {
        widgets.status->setVisible(false);
      }
    }
    for (std::size_t w = 0; w < widgets.windows.size(); ++w) {
      auto& row = widgets.windows[w];
      const bool showWindow = ok && w < provider->windows.size();
      if (row.row != nullptr) {
        row.row->setVisible(showWindow);
        row.row->setParticipatesInLayout(showWindow);
      }
      if (!showWindow) {
        continue;
      }
      const auto& window = provider->windows[w];
      if (row.name != nullptr) {
        row.name->setText(windowName(window.key));
      }
      std::string caption = std::format("{:.0f}%", static_cast<double>(window.percent));
      if (window.reset.has_value()) {
        caption += " · " + formatReset(*window.reset);
      }
      if (row.caption != nullptr) {
        row.caption->setText(caption);
      }
      if (row.bar != nullptr) {
        row.bar->setProgress(window.percent / 100.0F);
        row.bar->setFill(colorSpecFromRole(barRole(window.percent)));
      }
    }
  }
  if (m_empty != nullptr) {
    m_empty->setVisible(!any);
    m_empty->setParticipatesInLayout(!any);
  }
  if (m_updated != nullptr) {
    const bool haveTime = newest.time_since_epoch().count() != 0;
    if (!haveTime) {
      m_updated->setText({});
    } else {
      const std::time_t raw = std::chrono::system_clock::to_time_t(newest);
      std::tm tm{};
      localtime_r(&raw, &tm);
      char buf[16]{};
      std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
      m_updated->setText(i18n::tr("control-center.ai-usage.updated-at", "time", buf));
    }
    m_updated->setVisible(haveTime);
    m_updated->setParticipatesInLayout(haveTime);
  }
  if (m_refreshButton != nullptr && snapshot != nullptr) {
    m_refreshButton->setEnabled(!snapshot->refreshing && (ollama == nullptr || !ollama->busy));
  }
}
