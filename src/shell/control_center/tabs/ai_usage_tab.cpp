#include "shell/control_center/tabs/ai_usage_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/ai_usage_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/radio_button.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/spinner.h"
#include "ui/controls/toggle.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace control_center;
using noctalia::ai_usage::OllamaStatus;
using noctalia::ai_usage::ProviderId;
using noctalia::ai_usage::Status;
using noctalia::ai_usage::WindowKey;

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

  [[nodiscard]] std::string providerName(ProviderId id) { return noctalia::ai_usage::providerLabel(id); }

  [[nodiscard]] std::string windowName(WindowKey key) { return noctalia::ai_usage::windowLabel(key); }

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
    return noctalia::ai_usage::ollamaStatusLabel(status);
  }

  [[nodiscard]] ColorRole barRole(float percent, int attention) {
    if (percent + 0.5F >= static_cast<float>(attention)) {
      return ColorRole::Error;
    }
    if (percent >= 75.0F) {
      return ColorRole::Secondary;
    }
    return ColorRole::Primary;
  }

  [[nodiscard]] bool hiddenStatus(Status status) { return !noctalia::ai_usage::providerVisible(status); }

  [[nodiscard]] bool ollamaHidden(OllamaStatus status) { return !noctalia::ai_usage::ollamaVisible(status); }

  [[nodiscard]] bool ollamaModelSelectable(const noctalia::ai_usage::OllamaSnapshot& ollama) {
    if (ollama.busy) {
      return false;
    }
    if (ollama.status == OllamaStatus::Running) {
      return true;
    }
    return ollama.hasCli && ollama.local && ollama.status == OllamaStatus::Stopped;
  }

  [[nodiscard]] const noctalia::ai_usage::ProviderSnapshot*
  firstVisibleProvider(const noctalia::ai_usage::Snapshot& snapshot) {
    for (const auto& provider : snapshot.providers) {
      if (noctalia::ai_usage::providerVisible(provider.status)) {
        return &provider;
      }
    }
    return nullptr;
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

  [[nodiscard]] std::string formatCount(double value) {
    if (std::abs(value - std::round(value)) < 0.05) {
      return std::to_string(static_cast<int>(std::lround(value)));
    }
    return std::format("{:.1f}", value);
  }

  [[nodiscard]] std::string formatWindowMeta(const noctalia::ai_usage::Window& window) {
    std::string meta;
    if (window.used.has_value() && window.max.has_value()) {
      meta = formatCount(*window.used) + "/" + formatCount(*window.max);
    }
    if (window.reset.has_value()) {
      if (!meta.empty()) {
        meta += " · ";
      }
      meta += formatReset(*window.reset);
    }
    return meta;
  }

} // namespace

AiUsageTab::AiUsageTab(const ControlCenterServices& services)
    : m_service(services.aiUsage),
      m_config(services.config),
      m_settings(std::make_unique<SettingsPane>(
          services, SettingsPaneOptions{.section = "control-center", .includedGroups = {"ai-usage"}}
      )) {
  if (m_config != nullptr) {
    if (const std::optional<std::string> source = m_config->stateString("ai_usage_tab", "source")) {
      m_source = sourceFromKey(*source);
    }
    if (const std::optional<std::string> catalog = m_config->stateString("ai_usage_tab", "local_catalog")) {
      m_localCatalogPersisted = true;
      if (*catalog == "library") {
        m_localCatalog = LocalCatalog::Library;
      }
    }
  }
  if (m_service != nullptr) {
    m_service->addChangeCallback([this]() {
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
      .out = &m_compactCard,
      .align = FlexAlign::Stretch,
      .configure =
          [scale, opacity = panelCardOpacity()](Flex& card) { applyCompactCardStyle(card, scale, opacity, false); },
  });
  compact->addChild(
      ui::row(
          {
              .out = &m_headlineRow,
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
              .fillWidth = true,
              .visible = false,
              .participatesInLayout = false,
          },
          ui::label({
              .out = &m_headline,
              .fontSize = Style::fontSizeTitle * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
              .ellipsize = TextEllipsize::End,
              .flexGrow = 1.0F,
              .visible = false,
              .participatesInLayout = false,
          }),
          ui::button({
              .out = &m_compactAction,
              .text = i18n::tr("control-center.ai-usage.open"),
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .visible = false,
              .participatesInLayout = false,
              .onClick =
                  [this]() {
                    if (m_service == nullptr) {
                      return;
                    }
                    const auto& snapshot = m_service->snapshot();
                    if (noctalia::ai_usage::activeOllamaPull(snapshot) != nullptr) {
                      m_service->cancelOllamaPull();
                      return;
                    }
                    if (const auto* failed = noctalia::ai_usage::failedOllamaPull(snapshot)) {
                      m_service->pullOllamaModel(failed->name);
                      return;
                    }
                    if (const auto hit = noctalia::ai_usage::hottestQuota(snapshot)) {
                      m_service->launchProvider(hit->provider);
                      return;
                    }
                    if (noctalia::ai_usage::glanceableOllama(snapshot)) {
                      const auto& ollama = snapshot.ollama;
                      if (ollama.hasCli && ollama.local && ollama.status == OllamaStatus::Stopped && !ollama.busy) {
                        m_service->startOllama();
                        return;
                      }
                      if (ollama.local && ollama.status == OllamaStatus::Running && !ollama.busy
                          && !ollama.selected.empty()
                          && !noctalia::ai_usage::ollamaModelIsLoaded(ollama, ollama.selected)) {
                        m_service->setOllamaModelLoaded(true);
                        return;
                      }
                      m_service->launchOllama();
                      return;
                    }
                    if (const auto* runtime = noctalia::ai_usage::glanceableLocalRuntime(snapshot)) {
                      m_service->launchLocalRuntime(runtime->kind);
                      return;
                    }
                    if (const auto* provider = firstVisibleProvider(snapshot); provider != nullptr) {
                      m_service->launchProvider(provider->id);
                    }
                  },
          })
      )
  );
  compact->addChild(
      ui::label({
          .out = &m_caption,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
          .ellipsize = TextEllipsize::End,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  compact->addChild(
      ui::label({
          .out = &m_empty,
          .text = i18n::tr("control-center.ai-usage.empty"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 4,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  compact->addChild(
      ui::label({
          .out = &m_emptyHint,
          .text = i18n::tr("control-center.ai-usage.empty-hint"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  compact->addChild(
      ui::label({
          .out = &m_updated,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  compact->addChild(
      ui::segmented({
          .out = &m_sourceSwitch,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .equalSegmentWidths = false,
          .visible = false,
          .participatesInLayout = false,
          .onChange = [this](std::size_t index) { applySource(index); },
          .configure = [](Segmented& sources) { sources.setWrap(true); },
      })
  );
  compact->addChild(
      ui::segmented({
          .out = &m_localCatalogSwitch,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("control-center.ai-usage.ollama.installed")},
                  {.label = i18n::tr("control-center.ai-usage.ollama.library")},
              },
          .selectedIndex = m_localCatalog == LocalCatalog::Library ? 1 : 0,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .equalSegmentWidths = false,
          .visible = false,
          .participatesInLayout = false,
          .onChange = [this](std::size_t index) { applyLocalCatalog(index); },
      })
  );
  auto searchHost = ui::column({
      .out = &m_librarySearchHost,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .participatesInLayout = false,
  });
  auto searchRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  searchRow->addChild(
      ui::glyph({
          .glyph = "search",
          .glyphSize = Style::baseGlyphSize * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  searchRow->addChild(
      ui::input({
          .out = &m_librarySearch,
          .placeholder = i18n::tr("control-center.ai-usage.ollama.library-search"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceMd * scale,
          .clearButtonEnabled = true,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onChange = [this](const std::string&) { onLibrarySearchChanged(); },
          .onSubmit = [this](const std::string&) { applyLibrarySearch(true); },
      })
  );
  searchRow->addChild(
      ui::spinner({
          .out = &m_librarySpinner,
          .color = colorSpecFromRole(ColorRole::Primary),
          .spinnerSize = Style::baseGlyphSize * scale,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  searchHost->addChild(std::move(searchRow));
  compact->addChild(std::move(searchHost));
  tab->addChild(std::move(compact));

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
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
      .visible = false,
      .participatesInLayout = false,
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
              .out = &m_localTitle,
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
          }),
          ui::button({
              .out = &m_ollamaOpen,
              .text = i18n::tr("control-center.ai-usage.open"),
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .visible = false,
              .onClick =
                  [this]() {
                    if (m_service != nullptr) {
                      m_service->launchOllama();
                    }
                  },
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
  auto ollamaModels = ui::column({
      .out = &m_ollamaModelsHost,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
      .configure =
          [scale](Flex& host) {
            applyGroupedListStyle(host, scale);
            host.setPadding(Style::spaceXs * scale);
          },
  });
  auto toggleRow = ui::row({
      .out = &m_ollamaToggleRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
      .configure =
          [scale](Flex& host) {
            applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None);
            host.setPadding(0.0F, Style::spaceSm * scale);
          },
  });
  toggleRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.ai-usage.ollama.service"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  toggleRow->addChild(
      ui::toggle({
          .out = &m_ollamaToggle,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool on) {
                if (m_ollamaSyncing || m_service == nullptr) {
                  return;
                }
                if (on) {
                  m_service->startOllama();
                } else {
                  m_service->stopOllama();
                }
              },
      })
  );
  toggleRow->addChild(
      ui::inputArea({
          .out = &m_ollamaToggleHit,
          .propagateEvents = false,
          .zIndex = 1,
          .participatesInLayout = false,
          .onClick = [this](const InputArea::PointerData&) { toggleOllamaService(); },
      })
  );
  ollamaModels->addChild(std::move(toggleRow));
  auto loadedRow = ui::row({
      .out = &m_ollamaLoadedRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
      .configure =
          [scale](Flex& host) {
            applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None);
            host.setPadding(0.0F, Style::spaceSm * scale);
          },
  });
  loadedRow->addChild(
      ui::label({
          .text = i18n::tr("control-center.ai-usage.ollama.loaded-toggle"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0F,
      })
  );
  loadedRow->addChild(
      ui::label({
          .out = &m_ollamaLoadedMeta,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  );
  loadedRow->addChild(
      ui::toggle({
          .out = &m_ollamaLoadedToggle,
          .checkedImmediate = false,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange =
              [this](bool on) {
                if (m_ollamaSyncing || m_service == nullptr) {
                  return;
                }
                m_service->setOllamaModelLoaded(on);
              },
      })
  );
  loadedRow->addChild(
      ui::inputArea({
          .out = &m_ollamaLoadedHit,
          .propagateEvents = false,
          .zIndex = 1,
          .participatesInLayout = false,
          .onClick = [this](const InputArea::PointerData&) { toggleOllamaLoaded(); },
      })
  );
  ollamaModels->addChild(std::move(loadedRow));
  for (auto& model : m_ollamaModels) {
    auto row = ui::row({
        .out = &model.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
        .visible = false,
        .participatesInLayout = false,
        .configure =
            [scale](Flex& host) {
              applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None);
              host.setPadding(0.0F, Style::spaceSm * scale);
            },
    });
    row->addChild(
        ui::radioButton({
            .out = &model.radio,
            .scale = scale,
            .onChange =
                [this, modelPtr = &model](bool checked) {
                  if (!checked) {
                    return;
                  }
                  selectOllamaModelRow(*modelPtr);
                },
        })
    );
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
    row->addChild(
        ui::button({
            .out = &model.remove,
            .text = i18n::tr("control-center.ai-usage.ollama.remove"),
            .fontSize = Style::fontSizeCaption * scale,
            .contentAlign = ButtonContentAlign::Center,
            .variant = ButtonVariant::Destructive,
            .minHeight = Style::controlHeightSm * scale,
            .paddingV = Style::spaceXs * scale,
            .paddingH = Style::spaceSm * scale,
            .radius = Style::scaledRadiusMd(scale),
            .visible = false,
            .participatesInLayout = false,
            .onClick = [this, modelPtr = &model]() { removeOllamaModelRow(*modelPtr); },
            .configure = [](Button& button) { button.setZIndex(2); },
        })
    );
    row->addChild(
        ui::inputArea({
            .out = &model.hit,
            .propagateEvents = false,
            .zIndex = 1,
            .participatesInLayout = false,
            .onClick =
                [this, modelPtr = &model](const InputArea::PointerData&) { selectOllamaModelRow(*modelPtr); },
        })
    );
    ollamaModels->addChild(std::move(row));
  }
  ollamaCard->addChild(std::move(ollamaModels));
  ollamaCard->addChild(
      ui::label({
          .out = &m_ollamaMore,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  auto localRuntimes = ui::column({
      .out = &m_localRuntimesHost,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
      .configure =
          [scale](Flex& host) {
            applyGroupedListStyle(host, scale);
            host.setPadding(Style::spaceXs * scale);
          },
  });
  for (auto& runtime : m_localRuntimes) {
    auto row = ui::row({
        .out = &runtime.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
        .visible = false,
        .participatesInLayout = false,
        .configure =
            [scale](Flex& host) {
              applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None);
              host.setPadding(0.0F, Style::spaceSm * scale);
            },
    });
    row->addChild(
        ui::glyph({
            .out = &runtime.glyph,
            .glyph = "server",
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        })
    );
    row->addChild(
        ui::label({
            .out = &runtime.name,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .flexGrow = 1.0F,
        })
    );
    row->addChild(
        ui::label({
            .out = &runtime.meta,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );
    row->addChild(
        ui::button({
            .out = &runtime.open,
            .text = i18n::tr("control-center.ai-usage.open"),
            .fontSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .onClick =
                [this, runtimePtr = &runtime]() {
                  if (m_service != nullptr) {
                    m_service->launchLocalRuntime(runtimePtr->kind);
                  }
                },
        })
    );
    localRuntimes->addChild(std::move(row));
  }
  ollamaCard->addChild(std::move(localRuntimes));
  list->addChild(std::move(ollamaCard));

  auto libraryCard = ui::column({
      .out = &m_libraryCard,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale](Flex& section) { applyContentCardStyle(section, scale); },
  });
  libraryCard->addChild(
      ui::label({
          .out = &m_libraryStatus,
          .text = i18n::tr("control-center.ai-usage.ollama.library-hint"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  );
  auto libraryHits = ui::column({
      .out = &m_libraryHitsHost,
      .align = FlexAlign::Stretch,
      .visible = false,
      .participatesInLayout = false,
      .configure =
          [scale](Flex& host) {
            applyGroupedListStyle(host, scale);
            host.setPadding(Style::spaceXs * scale);
          },
  });
  for (auto& hit : m_libraryHits) {
    auto row = ui::column({
        .out = &hit.row,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .fillWidth = true,
        .visible = false,
        .participatesInLayout = false,
        .configure =
            [scale](Flex& host) {
              applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None);
              host.setPadding(Style::spaceSm * scale);
            },
    });
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    header->addChild(
        ui::label({
            .out = &hit.name,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
            .flexGrow = 1.0F,
        })
    );
    header->addChild(
        ui::button({
            .out = &hit.action,
            .text = i18n::tr("control-center.ai-usage.ollama.install"),
            .fontSize = Style::fontSizeCaption * scale,
            .contentAlign = ButtonContentAlign::Center,
            .variant = ButtonVariant::Default,
            .minHeight = Style::controlHeightSm * scale,
            .paddingV = Style::spaceXs * scale,
            .paddingH = Style::spaceSm * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [this, hitPtr = &hit]() { installLibraryHit(*hitPtr); },
            .configure = [](Button& button) { button.setZIndex(2); },
        })
    );
    row->addChild(std::move(header));
    row->addChild(
        ui::label({
            .out = &hit.summary,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 2,
            .ellipsize = TextEllipsize::End,
            .visible = false,
            .participatesInLayout = false,
        })
    );
    row->addChild(
        ui::progressBar({
            .out = &hit.bar,
            .fill = colorSpecFromRole(ColorRole::Primary),
            .track = colorSpecFromRole(ColorRole::SurfaceVariant),
            .radius = Style::sliderTrackHeight * scale * 0.5F,
            .progress = 0.0F,
            .height = Style::sliderTrackHeight * scale,
            .visible = false,
            .participatesInLayout = false,
        })
    );
    row->addChild(
        ui::label({
            .out = &hit.meta,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
            .visible = false,
            .participatesInLayout = false,
        })
    );
    row->addChild(
        ui::inputArea({
            .out = &hit.hit,
            .propagateEvents = false,
            .zIndex = 1,
            .participatesInLayout = false,
            .onClick =
                [this, hitPtr = &hit](const InputArea::PointerData&) { activateLibraryHit(*hitPtr); },
        })
    );
    libraryHits->addChild(std::move(row));
  }
  libraryHits->addChild(
      ui::label({
          .out = &m_libraryMore,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  libraryCard->addChild(std::move(libraryHits));
  list->addChild(std::move(libraryCard));

  for (std::size_t i = 0; i < m_providers.size(); ++i) {
    auto& widgets = m_providers[i];
    const auto id = static_cast<ProviderId>(i);
    auto card = ui::column({
        .out = &widgets.card,
        .align = FlexAlign::Stretch,
        .visible = false,
        .participatesInLayout = false,
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
                .fontSize = Style::fontSizeBody * scale,
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
    auto windowsHost = ui::column({
        .out = &widgets.windowsHost,
        .align = FlexAlign::Stretch,
        .visible = false,
        .participatesInLayout = false,
        .configure =
            [scale](Flex& host) {
              applyGroupedListStyle(host, scale);
              host.setPadding(Style::spaceXs * scale);
            },
    });
    for (auto& window : widgets.windows) {
      auto row = ui::column({
          .out = &window.row,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .fillWidth = true,
          .visible = false,
          .participatesInLayout = false,
          .configure =
              [scale](Flex& host) {
                applyGroupedListRowStyle(host, scale, GroupedListRowEmphasis::None);
                host.setPadding(Style::spaceSm * scale);
              },
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
              .track = colorSpecFromRole(ColorRole::SurfaceVariant),
              .radius = Style::sliderTrackHeight * scale * 0.5F,
              .progress = 0.0F,
              .height = Style::sliderTrackHeight * scale,
          })
      );
      windowsHost->addChild(std::move(row));
    }
    card->addChild(std::move(windowsHost));
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
  m_librarySearchTimer.stop();
  m_pendingRemove.reset();
  if (m_settings != nullptr) {
    m_settings->onClose();
  }
  m_root = nullptr;
  m_compactCard = nullptr;
  m_headlineRow = nullptr;
  m_headline = nullptr;
  m_compactAction = nullptr;
  m_caption = nullptr;
  m_sourceSwitch = nullptr;
  m_localCatalogSwitch = nullptr;
  m_librarySearchHost = nullptr;
  m_librarySearch = nullptr;
  m_librarySpinner = nullptr;
  m_scroll = nullptr;
  m_list = nullptr;
  m_ollamaCard = nullptr;
  m_ollamaModelsHost = nullptr;
  m_libraryCard = nullptr;
  m_libraryHitsHost = nullptr;
  m_libraryStatus = nullptr;
  m_localRuntimesHost = nullptr;
  m_localTitle = nullptr;
  m_ollamaStatus = nullptr;
  m_ollamaVersion = nullptr;
  m_ollamaOpen = nullptr;
  m_ollamaToggleRow = nullptr;
  m_ollamaToggleHit = nullptr;
  m_ollamaToggle = nullptr;
  m_ollamaLoadedRow = nullptr;
  m_ollamaLoadedHit = nullptr;
  m_ollamaLoadedToggle = nullptr;
  m_ollamaLoadedMeta = nullptr;
  m_ollamaModels = {};
  m_libraryHits = {};
  m_localRuntimes = {};
  m_ollamaMore = nullptr;
  m_libraryMore = nullptr;
  m_empty = nullptr;
  m_emptyHint = nullptr;
  m_updated = nullptr;
  m_refreshButton = nullptr;
  m_providers = {};
  m_sources.clear();
  m_sourceSyncing = false;
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
  if (m_root == nullptr) {
    return;
  }
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
  layoutOllamaModelHits();
}

void AiUsageTab::layoutOllamaModelHits() {
  const auto layoutHit = [](Flex* row, InputArea* hit) {
    if (hit == nullptr || row == nullptr) {
      return;
    }
    const bool show = row->visible();
    hit->setVisible(show);
    if (!show) {
      return;
    }
    hit->setPosition(0.0F, 0.0F);
    hit->setSize(row->width(), row->height());
  };
  layoutHit(m_ollamaToggleRow, m_ollamaToggleHit);
  layoutHit(m_ollamaLoadedRow, m_ollamaLoadedHit);
  for (auto& model : m_ollamaModels) {
    layoutHit(model.row, model.hit);
  }
  for (auto& hit : m_libraryHits) {
    layoutHit(hit.row, hit.hit);
  }
}

void AiUsageTab::selectOllamaModelRow(const OllamaModelWidgets& model) {
  if (m_ollamaSyncing || m_service == nullptr || model.id.empty()) {
    return;
  }
  clearPendingRemove();
  m_service->selectOllamaModel(model.id);
}

void AiUsageTab::toggleOllamaService() {
  if (m_ollamaSyncing || m_service == nullptr || m_ollamaToggle == nullptr || !m_ollamaToggle->enabled()) {
    return;
  }
  if (m_ollamaToggle->checked()) {
    m_service->stopOllama();
  } else {
    m_service->startOllama();
  }
}

void AiUsageTab::toggleOllamaLoaded() {
  if (m_ollamaSyncing || m_service == nullptr || m_ollamaLoadedToggle == nullptr
      || !m_ollamaLoadedToggle->enabled()) {
    return;
  }
  m_service->setOllamaModelLoaded(!m_ollamaLoadedToggle->checked());
}

void AiUsageTab::onPanelCardOpacityChanged(float opacity) {
  if (m_compactCard != nullptr) {
    applyCompactCardStyle(*m_compactCard, contentScale(), opacity, false);
  }
  if (m_sourceSwitch != nullptr) {
    m_sourceSwitch->setSurfaceOpacity(opacity);
  }
  if (m_localCatalogSwitch != nullptr) {
    m_localCatalogSwitch->setSurfaceOpacity(opacity);
  }
  if (m_librarySearch != nullptr) {
    m_librarySearch->setSurfaceOpacity(opacity);
  }
}

std::string AiUsageTab::sourceKey(const Source& source) {
  if (source.kind == SourceKind::Ollama) {
    return "ollama";
  }
  return std::string(noctalia::ai_usage::providerKey(source.provider));
}

std::optional<AiUsageTab::Source> AiUsageTab::sourceFromKey(std::string_view key) {
  if (key == "ollama") {
    return Source{.kind = SourceKind::Ollama};
  }
  for (std::size_t i = 0; i < noctalia::ai_usage::kProviderCount; ++i) {
    const auto id = static_cast<ProviderId>(i);
    if (noctalia::ai_usage::providerKey(id) == key) {
      return Source{.kind = SourceKind::Provider, .provider = id};
    }
  }
  return std::nullopt;
}

void AiUsageTab::applySource(std::size_t index) {
  if (m_sourceSyncing || index >= m_sources.size()) {
    return;
  }
  if (m_source.has_value() && m_sources[index] == *m_source) {
    return;
  }
  m_source = m_sources[index];
  persistSource();
  if (m_source->kind == SourceKind::Ollama && m_localCatalog == LocalCatalog::Library) {
    ensureLibrarySearch();
  }
  syncUi();
  PanelManager::instance().requestLayout();
}

void AiUsageTab::persistSource() {
  if (m_config == nullptr || !m_source.has_value()) {
    return;
  }
  (void)m_config->setStateString("ai_usage_tab", "source", sourceKey(*m_source));
}

void AiUsageTab::applyLocalCatalog(std::size_t index) {
  if (m_catalogSyncing) {
    return;
  }
  const auto next = index == 1 ? LocalCatalog::Library : LocalCatalog::Installed;
  if (m_localCatalog == next) {
    return;
  }
  m_localCatalog = next;
  m_localCatalogPersisted = true;
  persistLocalCatalog();
  if (m_localCatalog == LocalCatalog::Library) {
    ensureLibrarySearch();
  }
  syncUi();
  PanelManager::instance().requestLayout();
}

void AiUsageTab::persistLocalCatalog() {
  if (m_config == nullptr) {
    return;
  }
  (void)m_config->setStateString(
      "ai_usage_tab", "local_catalog", m_localCatalog == LocalCatalog::Library ? "library" : "installed"
  );
}

void AiUsageTab::ensureLibrarySearch() {
  if (m_service == nullptr) {
    return;
  }
  const auto& library = m_service->snapshot().library;
  if (library.searching) {
    return;
  }
  const std::string query = m_librarySearch != nullptr ? StringUtils::trim(m_librarySearch->value()) : std::string{};
  if (!library.hits.empty() && library.query == query && library.error.empty()) {
    return;
  }
  m_service->searchOllamaLibrary(query);
}

void AiUsageTab::onLibrarySearchChanged() {
  if (m_localCatalog == LocalCatalog::Installed) {
    syncUi();
    PanelManager::instance().requestLayout();
    return;
  }
  m_librarySearchTimer.start(std::chrono::milliseconds(280), [this]() { applyLibrarySearch(false); });
}

void AiUsageTab::applyLibrarySearch(bool force) {
  m_librarySearchTimer.stop();
  if (m_service == nullptr) {
    return;
  }
  if (m_localCatalog == LocalCatalog::Installed) {
    syncUi();
    PanelManager::instance().requestLayout();
    return;
  }
  const std::string query = m_librarySearch != nullptr ? StringUtils::trim(m_librarySearch->value()) : std::string{};
  const auto& library = m_service->snapshot().library;
  if (!force && library.query == query && (library.searching || !library.hits.empty() || !library.error.empty())) {
    return;
  }
  m_service->searchOllamaLibrary(query);
}

void AiUsageTab::installLibraryHit(const LibraryHitWidgets& hit) {
  if (m_service == nullptr || hit.id.empty()) {
    return;
  }
  const auto& snapshot = m_service->snapshot();
  if (const auto* pull = noctalia::ai_usage::activeOllamaPull(snapshot);
      pull != nullptr && pull->name == hit.id) {
    m_service->cancelOllamaPull();
    return;
  }
  if (noctalia::ai_usage::ollamaModelInstalled(snapshot.ollama, hit.id)) {
    m_localCatalogPersisted = true;
    m_localCatalog = LocalCatalog::Installed;
    persistLocalCatalog();
    m_service->selectOllamaModel(hit.id);
    syncUi();
    PanelManager::instance().requestLayout();
    return;
  }
  m_service->pullOllamaModel(hit.id);
}

void AiUsageTab::activateLibraryHit(const LibraryHitWidgets& hit) { installLibraryHit(hit); }

void AiUsageTab::removeOllamaModelRow(const OllamaModelWidgets& model) {
  if (m_service == nullptr || model.id.empty()) {
    return;
  }
  if (!m_pendingRemove.has_value() || *m_pendingRemove != model.id) {
    m_pendingRemove = model.id;
    if (model.remove != nullptr) {
      model.remove->setText(i18n::tr("control-center.ai-usage.ollama.remove-confirm"));
    }
    return;
  }
  m_pendingRemove.reset();
  m_service->deleteOllamaModel(model.id);
}

void AiUsageTab::clearPendingRemove() {
  if (!m_pendingRemove.has_value()) {
    return;
  }
  const std::string name = *m_pendingRemove;
  m_pendingRemove.reset();
  for (auto& model : m_ollamaModels) {
    if (model.id == name && model.remove != nullptr) {
      model.remove->setText(i18n::tr("control-center.ai-usage.ollama.remove"));
    }
  }
}

void AiUsageTab::syncSourceSwitch(bool showLocal, const noctalia::ai_usage::Snapshot* snapshot) {
  std::vector<Source> next;
  if (snapshot != nullptr) {
    for (std::size_t i = 0; i < snapshot->providers.size() && i < m_providers.size(); ++i) {
      if (!hiddenStatus(snapshot->providers[i].status)) {
        next.push_back(Source{.kind = SourceKind::Provider, .provider = static_cast<ProviderId>(i)});
      }
    }
  }
  if (showLocal) {
    next.push_back(Source{.kind = SourceKind::Ollama});
  }

  bool selectedVisible = false;
  if (m_source.has_value()) {
    for (const auto& source : next) {
      if (source == *m_source) {
        selectedVisible = true;
        break;
      }
    }
  }
  if (!selectedVisible) {
    if (next.empty()) {
      m_source.reset();
    } else {
      std::optional<Source> preferred;
      if (snapshot != nullptr) {
        if (const auto hit = noctalia::ai_usage::hottestQuota(*snapshot)) {
          for (const auto& source : next) {
            if (source.kind == SourceKind::Provider && source.provider == hit->provider) {
              preferred = source;
              break;
            }
          }
        }
        if (!preferred.has_value() && showLocal) {
          for (const auto& source : next) {
            if (source.kind == SourceKind::Ollama) {
              preferred = source;
              break;
            }
          }
        }
      }
      m_source = preferred.value_or(next.front());
      persistSource();
    }
  }

  const bool showSwitch = next.size() > 1;
  const bool layoutDirty = setShown(m_sourceSwitch, showSwitch);
  if (m_sourceSwitch != nullptr && showSwitch) {
    const bool same = next == m_sources && m_sourceSwitch->selectedIndex() < next.size() && m_source.has_value()
        && next[m_sourceSwitch->selectedIndex()] == *m_source;
    if (!same) {
      m_sources = std::move(next);
      m_sourceSyncing = true;
      m_sourceSwitch->clearOptions();
      std::size_t selected = 0;
      for (std::size_t i = 0; i < m_sources.size(); ++i) {
        if (m_sources[i].kind == SourceKind::Ollama) {
          m_sourceSwitch->addOption("", "server");
          const bool hasCompat = snapshot != nullptr && !snapshot->localRuntimes.empty();
          m_sourceSwitch->setOptionTooltip(
              i, i18n::tr(hasCompat ? "control-center.ai-usage.local.title" : "control-center.ai-usage.ollama.title")
          );
        } else {
          m_sourceSwitch->addOption("", noctalia::ai_usage::providerGlyph(m_sources[i].provider));
          m_sourceSwitch->setOptionTooltip(i, providerName(m_sources[i].provider));
        }
        if (m_source.has_value() && m_sources[i] == *m_source) {
          selected = i;
        }
      }
      m_sourceSwitch->setSelectedIndex(selected);
      m_sourceSyncing = false;
    }
  } else {
    m_sources = std::move(next);
  }
  syncSourceSwitchTooltips(snapshot);
  if (layoutDirty) {
    PanelManager::instance().requestLayout();
  }
}

void AiUsageTab::syncSourceSwitchTooltips(const noctalia::ai_usage::Snapshot* snapshot) {
  if (m_sourceSwitch == nullptr || m_sources.empty()) {
    return;
  }
  for (std::size_t i = 0; i < m_sources.size(); ++i) {
    const auto& source = m_sources[i];
    std::string tooltip;
    if (source.kind == SourceKind::Ollama) {
      const bool hasCompat = snapshot != nullptr && !snapshot->localRuntimes.empty();
      tooltip = i18n::tr(hasCompat ? "control-center.ai-usage.local.title" : "control-center.ai-usage.ollama.title");
      if (snapshot != nullptr && noctalia::ai_usage::ollamaVisible(snapshot->ollama.status)) {
        tooltip += " · " + ollamaStatusText(snapshot->ollama.status);
        if (const std::string_view model = noctalia::ai_usage::effectiveOllamaModel(snapshot->ollama);
            !model.empty()) {
          tooltip += " · ";
          tooltip += model;
        }
      } else if (hasCompat) {
        tooltip += " · " + noctalia::ai_usage::localRuntimeLabel(snapshot->localRuntimes.front().kind);
      }
    } else {
      tooltip = providerName(source.provider);
      if (snapshot != nullptr) {
        const auto index = static_cast<std::size_t>(source.provider);
        if (index < snapshot->providers.size()) {
          if (const auto* window = noctalia::ai_usage::hottestWindow(snapshot->providers[index]); window != nullptr) {
            tooltip += " · " + std::format("{:.0f}%", static_cast<double>(window->percent));
          }
        }
      }
    }
    m_sourceSwitch->setOptionTooltip(i, tooltip);
  }
}

void AiUsageTab::syncCompact(bool any, const noctalia::ai_usage::Snapshot* snapshot) {
  std::string headline;
  std::string caption;
  ColorRole headlineRole = ColorRole::OnSurface;
  const int attention = m_service != nullptr ? m_service->attentionPercent() : 90;
  bool actionStart = false;
  bool actionLoad = false;
  bool actionOpen = false;
  bool actionCancel = false;
  bool actionRetry = false;

  if (any && snapshot != nullptr) {
    if (const auto* pull = noctalia::ai_usage::activeOllamaPull(*snapshot)) {
      headline = noctalia::ai_usage::snapshotGlanceLabel(*snapshot);
      caption = i18n::tr("control-center.ai-usage.ollama.installing");
      if (!pull->name.empty()) {
        caption += " · " + pull->name;
      }
      const std::string progress = noctalia::ai_usage::formatOllamaPullProgress(*pull);
      if (!progress.empty() && pull->total > 0) {
        caption += " · " + progress;
      }
      actionCancel = true;
    } else if (const auto* failed = noctalia::ai_usage::failedOllamaPull(*snapshot)) {
      headline = failed->name;
      caption = failed->status.empty() ? i18n::tr("control-center.ai-usage.ollama.library-error") : failed->status;
      actionRetry = true;
    } else if (const auto hit = noctalia::ai_usage::hottestQuota(*snapshot)) {
      headline = std::format("{:.0f}%", static_cast<double>(hit->window.percent));
      headlineRole = barRole(hit->window.percent, attention);
      caption = providerName(hit->provider) + " · " + windowName(hit->window.key);
      if (const std::string meta = formatWindowMeta(hit->window); !meta.empty()) {
        caption += " · " + meta;
      }
      const auto index = static_cast<std::size_t>(hit->provider);
      actionOpen = index < snapshot->providers.size() && snapshot->providers[index].launchable;
    } else if (noctalia::ai_usage::glanceableOllama(*snapshot)) {
      const auto& ollama = snapshot->ollama;
      headline = noctalia::ai_usage::ollamaGlanceLabel(ollama);
      if (ollama.busy && ollama.status != OllamaStatus::Running) {
        headline = i18n::tr("control-center.ai-usage.ollama.starting");
        caption = ollama.selected.empty() ? ollama.version : ollama.selected;
      } else if (ollama.busy && ollama.status == OllamaStatus::Running) {
        caption = i18n::tr("control-center.ai-usage.ollama.stopping");
      } else if (!ollama.deleting.empty()) {
        headline = ollama.deleting;
        caption = i18n::tr("control-center.ai-usage.ollama.removing");
      } else if (!ollama.loading.empty()) {
          caption = ollama.wantLoaded ? i18n::tr("control-center.ai-usage.ollama.loading")
                                     : i18n::tr("control-center.ai-usage.ollama.unloading");
        } else if (ollama.status == OllamaStatus::Running && ollama.models.empty()) {
          caption = i18n::tr("control-center.ai-usage.ollama.empty-models");
        } else if (headline == ollamaStatusText(ollama.status)) {
          const std::string_view model = noctalia::ai_usage::effectiveOllamaModel(ollama);
          if (!model.empty()) {
            caption = std::string(model);
          } else if (const auto disk = noctalia::ai_usage::ollamaDiskBytes(ollama); disk > 0) {
            caption = noctalia::ai_usage::formatByteSize(disk);
          } else {
            caption = ollama.version;
          }
        } else {
          caption = ollamaStatusText(ollama.status);
          if (const auto vram = noctalia::ai_usage::ollamaVramBytes(ollama); vram > 0) {
            caption += " · "
                + i18n::tr(
                      "control-center.ai-usage.ollama.in-memory", "size", noctalia::ai_usage::formatByteSize(vram)
                  );
          } else if (const auto disk = noctalia::ai_usage::ollamaDiskBytes(ollama); disk > 0) {
            caption += " · " + noctalia::ai_usage::formatByteSize(disk);
          } else if (!ollama.version.empty()) {
            caption += " · " + ollama.version;
          }
        }
        if (ollama.hasCli && ollama.local && ollama.status == OllamaStatus::Stopped && !ollama.busy) {
          actionStart = true;
        } else if (ollama.local && ollama.status == OllamaStatus::Running && !ollama.busy && !ollama.selected.empty()
                   && ollama.loading.empty()
                   && !noctalia::ai_usage::ollamaModelIsLoaded(ollama, ollama.selected)) {
          actionLoad = true;
        } else if (m_service != nullptr && m_service->canLaunchOllama() && !ollama.busy) {
          actionOpen = true;
        }
    } else if (const auto* runtime = noctalia::ai_usage::glanceableLocalRuntime(*snapshot)) {
      headline = noctalia::ai_usage::localRuntimeLabel(runtime->kind);
      caption = i18n::tr("control-center.ai-usage.ollama.running");
      if (!runtime->models.empty()) {
        caption += " · " + std::to_string(runtime->models.size());
      }
      actionOpen = runtime->launchable;
    } else if (const auto* provider = firstVisibleProvider(*snapshot)) {
      if (provider->status == Status::Ok) {
        if (!provider->plan.empty()) {
          headline = provider->plan;
          caption = i18n::tr("control-center.ai-usage.balance-only");
        } else {
          headline = providerName(provider->id);
          caption = i18n::tr("control-center.ai-usage.balance-only");
        }
      } else {
        headline = statusText(provider->status);
        caption = providerName(provider->id);
      }
      actionOpen = provider->launchable;
    }
  }

  (void)setShown(m_empty, !any);
  (void)setShown(m_emptyHint, !any);
  (void)setShown(m_headline, any && !headline.empty());
  (void)setShown(m_caption, any && !caption.empty());
  if (m_headline != nullptr) {
    m_headline->setText(headline);
    m_headline->setColor(colorSpecFromRole(headlineRole));
  }
  if (m_caption != nullptr) {
    m_caption->setText(caption);
  }

  const bool showAction = any && (actionStart || actionLoad || actionOpen || actionCancel || actionRetry);
  if (m_compactAction != nullptr) {
    if (actionCancel) {
      m_compactAction->setText(i18n::tr("control-center.ai-usage.ollama.cancel"));
    } else if (actionRetry) {
      m_compactAction->setText(i18n::tr("control-center.ai-usage.ollama.retry"));
    } else if (actionStart) {
      m_compactAction->setText(i18n::tr("control-center.ai-usage.ollama.start"));
    } else if (actionLoad) {
      m_compactAction->setText(i18n::tr("control-center.ai-usage.ollama.load"));
    } else {
      m_compactAction->setText(i18n::tr("control-center.ai-usage.open"));
    }
    m_compactAction->setVariant(
        actionStart || actionLoad || actionCancel || actionRetry ? ButtonVariant::Primary : ButtonVariant::Ghost
    );
    (void)setShown(m_compactAction, showAction);
  }
  (void)setShown(m_headlineRow, any && (!headline.empty() || showAction));

  if (m_updated != nullptr) {
    const bool refreshing = snapshot != nullptr && snapshot->refreshing;
    std::chrono::system_clock::time_point newest{};
    if (snapshot != nullptr) {
      for (const auto& provider : snapshot->providers) {
        if (provider.fetchedAt > newest) {
          newest = provider.fetchedAt;
        }
      }
      if (snapshot->fetchedAt > newest) {
        newest = snapshot->fetchedAt;
      }
    }
    const bool haveTime = any && newest.time_since_epoch().count() != 0;
    if (refreshing) {
      m_updated->setText(i18n::tr("control-center.ai-usage.updating"));
    } else if (!haveTime) {
      m_updated->setText({});
    } else {
      const std::time_t raw = std::chrono::system_clock::to_time_t(newest);
      std::tm tm{};
      localtime_r(&raw, &tm);
      char buf[16]{};
      std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
      m_updated->setText(i18n::tr("control-center.ai-usage.updated-at", "time", buf));
    }
    (void)setShown(m_updated, refreshing || haveTime);
  }
}

void AiUsageTab::syncUi() {
  if (m_root == nullptr) {
    return;
  }
  const auto* snapshot = m_service != nullptr ? &m_service->snapshot() : nullptr;
  bool any = false;
  const int attention = m_service != nullptr ? m_service->attentionPercent() : 90;

  const auto* ollama = snapshot != nullptr ? &snapshot->ollama : nullptr;
  const bool showOllama = ollama != nullptr && !ollamaHidden(ollama->status);
  const bool showLocal = showOllama || (snapshot != nullptr && !snapshot->localRuntimes.empty());
  syncSourceSwitch(showLocal, snapshot);
  const bool selectedOllama =
      showLocal && m_source.has_value() && m_source->kind == SourceKind::Ollama;
  if (selectedOllama && !m_localCatalogPersisted && ollama != nullptr && ollama->models.empty()) {
    m_localCatalog = LocalCatalog::Library;
  }
  const bool libraryCatalog = selectedOllama && m_localCatalog == LocalCatalog::Library;
  (void)setShown(m_localCatalogSwitch, selectedOllama);
  if (m_localCatalogSwitch != nullptr && selectedOllama) {
    m_catalogSyncing = true;
    m_localCatalogSwitch->setSelectedIndex(libraryCatalog ? 1 : 0);
    m_catalogSyncing = false;
  }
  (void)setShown(m_librarySearchHost, selectedOllama);
  if (m_librarySpinner != nullptr) {
    const bool spinning = libraryCatalog && snapshot != nullptr && snapshot->library.searching;
    m_librarySpinner->setVisible(spinning);
    if (spinning) {
      m_librarySpinner->start();
    } else {
      m_librarySpinner->stop();
    }
  }
  (void)setShown(m_ollamaCard, selectedOllama && !libraryCatalog);
  (void)setShown(m_libraryCard, libraryCatalog);
  if (libraryCatalog && snapshot != nullptr && !snapshot->library.searching && snapshot->library.hits.empty()
      && snapshot->library.error.empty() && (m_librarySearch == nullptr || m_librarySearch->value().empty())) {
    DeferredCall::callLater([this]() { ensureLibrarySearch(); });
  }
  if (showLocal) {
    any = true;
    const bool hasCompat = snapshot != nullptr && !snapshot->localRuntimes.empty();
    if (m_localTitle != nullptr) {
      m_localTitle->setText(
          i18n::tr(hasCompat ? "control-center.ai-usage.local.title" : "control-center.ai-usage.ollama.title")
      );
    }
    if (showOllama && ollama != nullptr) {
      const bool showToggle = ollama->hasCli && ollama->local
          && (ollama->status == OllamaStatus::Stopped || ollama->status == OllamaStatus::Running);
      if (m_ollamaStatus != nullptr) {
        if (!ollama->loading.empty()) {
          m_ollamaStatus->setText(
              i18n::tr(
                  ollama->wantLoaded ? "control-center.ai-usage.ollama.loading"
                                     : "control-center.ai-usage.ollama.unloading"
              )
              + " · " + ollama->loading
          );
        } else if (!ollama->deleting.empty()) {
          m_ollamaStatus->setText(
              i18n::tr("control-center.ai-usage.ollama.removing") + " · " + ollama->deleting
          );
        } else if (const auto* pull = snapshot != nullptr ? noctalia::ai_usage::activeOllamaPull(*snapshot) : nullptr;
                   pull != nullptr) {
          std::string status = i18n::tr("control-center.ai-usage.ollama.installing");
          status += " · " + pull->name;
          const std::string progress = noctalia::ai_usage::formatOllamaPullProgress(*pull);
          if (!progress.empty()) {
            status += " · " + progress;
          }
          m_ollamaStatus->setText(std::move(status));
        } else if (const auto* failed = snapshot != nullptr ? noctalia::ai_usage::failedOllamaPull(*snapshot) : nullptr;
                   failed != nullptr) {
          std::string status = failed->status.empty()
              ? i18n::tr("control-center.ai-usage.ollama.library-error")
              : failed->status;
          if (!failed->name.empty()) {
            status += " · " + failed->name;
          }
          m_ollamaStatus->setText(std::move(status));
        } else if (ollama->busy && ollama->status != OllamaStatus::Running) {
          m_ollamaStatus->setText(i18n::tr("control-center.ai-usage.ollama.starting"));
        } else if (ollama->busy) {
          m_ollamaStatus->setText(i18n::tr("control-center.ai-usage.ollama.stopping"));
        } else {
          m_ollamaStatus->setText(ollamaStatusText(ollama->status));
        }
        const bool showStatus = !ollama->loading.empty() || !ollama->deleting.empty() || ollama->busy
            || ollama->status == OllamaStatus::Error || !showToggle
            || (snapshot != nullptr
                && (noctalia::ai_usage::activeOllamaPull(*snapshot) != nullptr
                    || noctalia::ai_usage::failedOllamaPull(*snapshot) != nullptr));
        (void)setShown(m_ollamaStatus, showStatus);
      }
      if (m_ollamaVersion != nullptr) {
        std::string version = ollama->version;
        if (const auto disk = noctalia::ai_usage::ollamaDiskBytes(*ollama); disk > 0) {
          if (!version.empty()) {
            version += " · ";
          }
          version += noctalia::ai_usage::formatByteSize(disk);
        }
        if (const auto vram = noctalia::ai_usage::ollamaVramBytes(*ollama); vram > 0) {
          if (!version.empty()) {
            version += " · ";
          }
          version += i18n::tr(
              "control-center.ai-usage.ollama.in-memory", "size", noctalia::ai_usage::formatByteSize(vram)
          );
        }
        m_ollamaVersion->setText(version);
        (void)setShown(m_ollamaVersion, !version.empty());
      }
      if (m_ollamaOpen != nullptr) {
        const bool canOpen = m_service != nullptr && m_service->canLaunchOllama();
        (void)setShown(m_ollamaOpen, canOpen);
        m_ollamaOpen->setEnabled(canOpen && !ollama->busy);
      }
      (void)setShown(m_ollamaToggleRow, showToggle);
      if (m_ollamaToggle != nullptr) {
        m_ollamaSyncing = true;
        m_ollamaToggle->setEnabled(showToggle && !ollama->busy);
        m_ollamaToggle->setCheckedImmediate(ollama->status == OllamaStatus::Running);
        m_ollamaSyncing = false;
      }
      if (m_ollamaToggleHit != nullptr) {
        m_ollamaToggleHit->setEnabled(showToggle && !ollama->busy);
      }
      const bool showLoaded = showToggle && !ollama->selected.empty();
      (void)setShown(m_ollamaLoadedRow, showLoaded);
      if (m_ollamaLoadedToggle != nullptr) {
        const bool loaded =
            ollama->wantLoaded
            && (noctalia::ai_usage::ollamaModelIsLoaded(*ollama, ollama->selected)
                || ollama->loading == ollama->selected);
        m_ollamaSyncing = true;
        m_ollamaLoadedToggle->setEnabled(showLoaded && !ollama->busy && ollama->loading.empty());
        m_ollamaLoadedToggle->setCheckedImmediate(loaded);
        m_ollamaSyncing = false;
      }
      if (m_ollamaLoadedHit != nullptr) {
        m_ollamaLoadedHit->setEnabled(showLoaded && !ollama->busy && ollama->loading.empty());
      }
      if (m_ollamaLoadedMeta != nullptr) {
        std::string loadedMeta = ollama->selected;
        for (const auto& model : ollama->models) {
          if (model.name != ollama->selected) {
            continue;
          }
          if (model.vram > 0) {
            loadedMeta += " · " + noctalia::ai_usage::formatByteSize(model.vram);
          } else if (!model.parameterSize.empty()) {
            loadedMeta += " · " + model.parameterSize;
          }
          break;
        }
        m_ollamaLoadedMeta->setText(loadedMeta);
      }
      const std::string installedQuery =
          !libraryCatalog && m_librarySearch != nullptr ? StringUtils::trim(m_librarySearch->value()) : std::string{};
      std::vector<const noctalia::ai_usage::OllamaModel*> models;
      models.reserve(ollama->models.size());
      for (const auto& model : ollama->models) {
        if (noctalia::ai_usage::ollamaModelMatchesQuery(model, installedQuery)) {
          models.push_back(&model);
        }
      }
      const std::size_t shown = std::min(models.size(), m_ollamaModels.size());
      (void)setShown(m_ollamaModelsHost, showToggle || shown > 0 || !installedQuery.empty());
      m_ollamaSyncing = true;
      for (std::size_t i = 0; i < m_ollamaModels.size(); ++i) {
        auto& row = m_ollamaModels[i];
        const bool show = i < shown;
        (void)setShown(row.row, show);
        if (!show) {
          row.id.clear();
          continue;
        }
        const auto& model = *models[i];
        row.id = model.name;
        const std::string_view inUse = noctalia::ai_usage::effectiveOllamaModel(*ollama);
        const bool selected = !inUse.empty() && model.name == inUse;
        if (row.row != nullptr) {
          applyGroupedListRowStyle(
              *row.row, contentScale(), selected ? GroupedListRowEmphasis::Selected : GroupedListRowEmphasis::None
          );
          row.row->setPadding(0.0F, Style::spaceSm * contentScale());
        }
        const bool canSelect = ollamaModelSelectable(*ollama);
        if (row.radio != nullptr) {
          row.radio->setChecked(selected);
          row.radio->setEnabled(canSelect);
        }
        if (row.hit != nullptr) {
          row.hit->setEnabled(canSelect);
        }
        if (row.name != nullptr) {
          row.name->setText(model.name);
        }
        std::string meta;
        if (!ollama->loading.empty() && model.name == ollama->loading) {
          meta = i18n::tr(
              ollama->wantLoaded ? "control-center.ai-usage.ollama.loading"
                                 : "control-center.ai-usage.ollama.unloading"
          );
        } else if (!ollama->deleting.empty() && model.name == ollama->deleting) {
          meta = i18n::tr("control-center.ai-usage.ollama.removing");
        } else if (selected) {
          meta = i18n::tr("control-center.ai-usage.ollama.in-use");
        } else if (model.loaded) {
          meta = i18n::tr("control-center.ai-usage.ollama.loaded");
        }
        if (!model.parameterSize.empty()) {
          if (!meta.empty()) {
            meta += " · ";
          }
          meta += model.parameterSize;
        }
        if (!model.quantization.empty()) {
          if (!meta.empty()) {
            meta += " · ";
          }
          meta += model.quantization;
        }
        if (model.size > 0) {
          if (!meta.empty()) {
            meta += " · ";
          }
          meta += noctalia::ai_usage::formatByteSize(model.size);
        }
        if (row.meta != nullptr) {
          row.meta->setText(meta);
        }
        const bool removing = ollama->deleting == model.name;
        if (row.remove != nullptr) {
          const bool showRemove = ollama->local && ollama->hasCli && !model.name.empty();
          (void)setShown(row.remove, showRemove);
          const bool pending = m_pendingRemove.has_value() && *m_pendingRemove == model.name;
          row.remove->setText(
              pending ? i18n::tr("control-center.ai-usage.ollama.remove-confirm")
                      : i18n::tr("control-center.ai-usage.ollama.remove")
          );
          row.remove->setEnabled(
              showRemove && !ollama->busy && ollama->loading.empty() && ollama->deleting.empty() && !removing
          );
        }
      }
      m_ollamaSyncing = false;
      if (m_ollamaMore != nullptr) {
        if (ollama->models.empty() && ollama->status == OllamaStatus::Running) {
          m_ollamaMore->setText(i18n::tr("control-center.ai-usage.ollama.empty-models"));
          (void)setShown(m_ollamaMore, true);
        } else if (models.empty() && !installedQuery.empty()) {
          m_ollamaMore->setText(i18n::tr("control-center.ai-usage.ollama.library-empty"));
          (void)setShown(m_ollamaMore, true);
        } else if (models.size() > shown) {
          m_ollamaMore->setText(
              i18n::tr("control-center.ai-usage.ollama.more", "count", std::to_string(models.size() - shown))
          );
          (void)setShown(m_ollamaMore, true);
        } else {
          (void)setShown(m_ollamaMore, false);
        }
      }
    } else {
      if (m_ollamaStatus != nullptr) {
        (void)setShown(m_ollamaStatus, false);
      }
      if (m_ollamaVersion != nullptr) {
        (void)setShown(m_ollamaVersion, false);
      }
      if (m_ollamaOpen != nullptr) {
        (void)setShown(m_ollamaOpen, false);
      }
      (void)setShown(m_ollamaToggleRow, false);
      (void)setShown(m_ollamaLoadedRow, false);
      (void)setShown(m_ollamaModelsHost, false);
      if (m_ollamaMore != nullptr) {
        (void)setShown(m_ollamaMore, false);
      }
    }
    const auto* runtimes = snapshot != nullptr ? &snapshot->localRuntimes : nullptr;
    const std::size_t localShown = runtimes != nullptr ? std::min(runtimes->size(), m_localRuntimes.size()) : 0;
    (void)setShown(m_localRuntimesHost, localShown > 0);
    for (std::size_t i = 0; i < m_localRuntimes.size(); ++i) {
      auto& row = m_localRuntimes[i];
      const bool show = i < localShown;
      (void)setShown(row.row, show);
      if (!show || runtimes == nullptr) {
        continue;
      }
      const auto& runtime = (*runtimes)[i];
      row.kind = runtime.kind;
      if (row.glyph != nullptr) {
        row.glyph->setGlyph(std::string(noctalia::ai_usage::localRuntimeGlyph(runtime.kind)));
      }
      if (row.name != nullptr) {
        row.name->setText(noctalia::ai_usage::localRuntimeLabel(runtime.kind));
      }
      if (row.meta != nullptr) {
        std::string meta = i18n::tr("control-center.ai-usage.ollama.running");
        if (!runtime.models.empty()) {
          meta += " · " + std::to_string(runtime.models.size());
        }
        row.meta->setText(meta);
      }
      if (row.open != nullptr) {
        (void)setShown(row.open, runtime.launchable);
      }
    }
    if (libraryCatalog && snapshot != nullptr) {
      const auto& library = snapshot->library;
      if (m_libraryStatus != nullptr) {
        if (library.searching) {
          m_libraryStatus->setText(i18n::tr("control-center.ai-usage.updating"));
        } else if (!library.error.empty()) {
          m_libraryStatus->setText(i18n::tr("control-center.ai-usage.ollama.library-error"));
        } else if (library.hits.empty()) {
          m_libraryStatus->setText(
              library.query.empty() ? i18n::tr("control-center.ai-usage.ollama.library-hint")
                                    : i18n::tr("control-center.ai-usage.ollama.library-empty")
          );
        } else if (const auto* failed = noctalia::ai_usage::failedOllamaPull(*snapshot); failed != nullptr) {
          std::string status = failed->status.empty()
              ? i18n::tr("control-center.ai-usage.ollama.library-error")
              : failed->status;
          if (!failed->name.empty()) {
            status += " · " + failed->name;
          }
          m_libraryStatus->setText(std::move(status));
        } else if (library.pull.has_value()) {
          std::string status = i18n::tr("control-center.ai-usage.ollama.installing");
          const std::string progress = noctalia::ai_usage::formatOllamaPullProgress(*library.pull);
          if (!progress.empty()) {
            status += " · " + library.pull->name + " · " + progress;
          }
          m_libraryStatus->setText(std::move(status));
        } else {
          m_libraryStatus->setText(i18n::tr("control-center.ai-usage.ollama.library-hint"));
        }
        const bool showStatus = library.searching || !library.error.empty() || library.hits.empty()
            || library.pull.has_value();
        (void)setShown(m_libraryStatus, showStatus);
      }
      const std::size_t shown = std::min(library.hits.size(), m_libraryHits.size());
      (void)setShown(m_libraryHitsHost, shown > 0);
      const bool pulling = noctalia::ai_usage::activeOllamaPull(*snapshot) != nullptr;
      const bool canInstall = ollama != nullptr && ollama->local && ollama->hasCli;
      for (std::size_t i = 0; i < m_libraryHits.size(); ++i) {
        auto& row = m_libraryHits[i];
        const bool show = i < shown;
        (void)setShown(row.row, show);
        if (!show) {
          row.id.clear();
          continue;
        }
        const auto& hit = library.hits[i];
        row.id = hit.name;
        const bool installed = ollama != nullptr && noctalia::ai_usage::ollamaModelInstalled(*ollama, hit.name);
        const bool thisPull = library.pull.has_value() && library.pull->name == hit.name;
        if (row.name != nullptr) {
          row.name->setText(hit.name);
        }
        if (row.summary != nullptr) {
          row.summary->setText(hit.description);
          (void)setShown(row.summary, !hit.description.empty());
        }
        std::string meta;
        if (thisPull) {
          meta = noctalia::ai_usage::formatOllamaPullProgress(*library.pull);
        } else if (installed) {
          meta = i18n::tr("control-center.ai-usage.ollama.installed");
        }
        if (!hit.sizes.empty()) {
          if (!meta.empty()) {
            meta += " · ";
          }
          meta += hit.sizes;
        }
        if (!hit.pulls.empty() && !thisPull) {
          if (!meta.empty()) {
            meta += " · ";
          }
          meta += hit.pulls;
        }
        if (row.meta != nullptr) {
          row.meta->setText(meta);
          (void)setShown(row.meta, !meta.empty());
        }
        if (row.bar != nullptr) {
          const bool showBar = thisPull && !library.pull->error && library.pull->total > 0;
          (void)setShown(row.bar, showBar);
          if (showBar) {
            row.bar->setProgress(noctalia::ai_usage::ollamaPullFraction(*library.pull));
          }
        }
        if (row.action != nullptr) {
          if (thisPull && noctalia::ai_usage::activeOllamaPull(*snapshot) != nullptr) {
            row.action->setText(i18n::tr("control-center.ai-usage.ollama.cancel"));
            row.action->setEnabled(true);
            row.action->setVariant(ButtonVariant::Destructive);
          } else if (thisPull && library.pull->error) {
            row.action->setText(i18n::tr("control-center.ai-usage.ollama.retry"));
            row.action->setEnabled(canInstall);
            row.action->setVariant(ButtonVariant::Primary);
          } else if (installed) {
            row.action->setText(i18n::tr("control-center.ai-usage.ollama.select"));
            row.action->setEnabled(true);
            row.action->setVariant(ButtonVariant::Default);
          } else {
            row.action->setText(i18n::tr("control-center.ai-usage.ollama.install"));
            row.action->setEnabled(canInstall && !pulling);
            row.action->setVariant(ButtonVariant::Default);
          }
        }
        if (row.hit != nullptr) {
          row.hit->setEnabled(thisPull || installed || (canInstall && !pulling));
        }
      }
      if (m_libraryMore != nullptr) {
        if (library.hits.size() > shown) {
          m_libraryMore->setText(
              i18n::tr("control-center.ai-usage.ollama.more", "count", std::to_string(library.hits.size() - shown))
          );
          (void)setShown(m_libraryMore, true);
        } else {
          (void)setShown(m_libraryMore, false);
        }
      }
    } else {
      (void)setShown(m_libraryHitsHost, false);
      if (m_libraryStatus != nullptr) {
        (void)setShown(m_libraryStatus, false);
      }
      if (m_libraryMore != nullptr) {
        (void)setShown(m_libraryMore, false);
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
    const bool selected = show && m_source.has_value() && m_source->kind == SourceKind::Provider
        && static_cast<std::size_t>(m_source->provider) == i;
    (void)setShown(widgets.card, selected);
    if (!show || provider == nullptr) {
      continue;
    }
    any = true;
    if (widgets.plan != nullptr) {
      widgets.plan->setText(provider->plan);
      widgets.plan->setVisible(!provider->plan.empty());
    }
    if (widgets.openButton != nullptr) {
      (void)setShown(widgets.openButton, provider->launchable);
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
    bool anyWindow = false;
    for (std::size_t w = 0; w < widgets.windows.size(); ++w) {
      auto& row = widgets.windows[w];
      const bool showWindow = ok && w < provider->windows.size();
      (void)setShown(row.row, showWindow);
      if (!showWindow) {
        continue;
      }
      anyWindow = true;
      const auto& window = provider->windows[w];
      if (row.name != nullptr) {
        row.name->setText(windowName(window.key));
      }
      std::string caption = std::format("{:.0f}%", static_cast<double>(window.percent));
      if (const std::string meta = formatWindowMeta(window); !meta.empty()) {
        caption += " · " + meta;
      }
      if (row.caption != nullptr) {
        row.caption->setText(caption);
      }
      if (row.bar != nullptr) {
        row.bar->setProgress(window.percent / 100.0F);
        row.bar->setFill(colorSpecFromRole(barRole(window.percent, attention)));
      }
    }
    (void)setShown(widgets.windowsHost, anyWindow);
  }

  (void)setShown(m_scroll, any && m_source.has_value());
  syncCompact(any, snapshot);
  if (m_refreshButton != nullptr && snapshot != nullptr) {
    m_refreshButton->setEnabled(!snapshot->refreshing && (ollama == nullptr || !ollama->busy));
  }
}
