#include "shell/settings/settings_hosted_editors.h"

#include "calendar/calendar_discovery_state.h"
#include "calendar/calendar_service.h"
#include "calendar/vdir_reader.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_content_plugins.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/template_store_content.h"
#include "theme/community_templates.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/search_picker.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "wayland/wayland_seat.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    constexpr std::string_view kCalendarDiscoveryOwner = "calendar_discovery";

  std::string calendarCredentialError(CalendarService::CredentialOperationResult result) {
    switch (result) {
    case CalendarService::CredentialOperationResult::Unavailable:
      return i18n::tr("settings.calendar-accounts.secret-service-unavailable");
    case CalendarService::CredentialOperationResult::Cancelled:
      return i18n::tr("settings.calendar-accounts.secret-service-cancelled");
    case CalendarService::CredentialOperationResult::DeniedOrLocked:
      return i18n::tr("settings.calendar-accounts.secret-service-locked");
    case CalendarService::CredentialOperationResult::CleanupError:
      return i18n::tr("settings.calendar-accounts.secret-cleanup-error");
    case CalendarService::CredentialOperationResult::FileError:
      return i18n::tr("settings.calendar-accounts.password-file-error");
    case CalendarService::CredentialOperationResult::ConfigError:
      return i18n::tr("settings.calendar-accounts.save-error");
    case CalendarService::CredentialOperationResult::MissingCredential:
    case CalendarService::CredentialOperationResult::BackendError:
      return i18n::tr("settings.calendar-accounts.secret-service-error");
    case CalendarService::CredentialOperationResult::Success:
      return {};
    }
    return i18n::tr("settings.calendar-accounts.secret-service-error");
  }

  struct PluginSourceDraft {
    PluginSourceKind kind = PluginSourceKind::Git;
    std::string name;
    std::string location;
    bool enabled = true;
    bool editing = false;
    bool nameInvalid = false;
    bool locationInvalid = false;
    std::string error;
  };

  enum class CalendarAccountProvider : std::uint8_t {
    ICloud,
    CustomCalDav,
    Google,
    IcsFileURL,
    Vdir,
  };

  struct CalendarAccountDraft {
    bool creating = true;
    CalendarAccountProvider provider = CalendarAccountProvider::ICloud;
    std::string id = "personal_icloud";
    std::string name;
    std::string username;
    std::string password;
    CalendarCredentialSource credentialSource = CalendarCredentialSource::SecretService;
    std::string passwordFile;
    std::string serverUrl;
    std::string path;
    std::string color;
    std::vector<std::string> calendars;
    std::vector<CalendarSource> discoveredCalendars;
    bool idInvalid = false;
    bool usernameInvalid = false;
    bool passwordInvalid = false;
    bool passwordFileInvalid = false;
    bool serverUrlInvalid = false;
    bool pathInvalid = false;
    bool credentialOperationInFlight = false;
  };

  bool validCalendarAccountId(std::string_view id) {
    if (id.empty()) {
      return false;
    }
    return std::ranges::all_of(id, [](char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    });
  }

  bool calendarAccountIdExists(const Config& cfg, std::string_view id) {
    return std::ranges::contains(cfg.calendar.accounts, id, &CalendarConfig::Account::id);
  }

  bool pluginSourceNameExists(const Config& cfg, std::string_view name) {
    return std::ranges::contains(cfg.plugins.sources, name, &PluginSourceConfig::name);
  }

  std::size_t pluginSourceKindIndex(PluginSourceKind kind) { return kind == PluginSourceKind::Path ? 1U : 0U; }

  const CalendarConfig::Account* findCalendarAccount(const Config& cfg, std::string_view id) {
    const auto it = std::ranges::find(cfg.calendar.accounts, id, &CalendarConfig::Account::id);
    return it != cfg.calendar.accounts.end() ? &*it : nullptr;
  }

  std::string calendarProviderKey(CalendarAccountProvider provider) {
    switch (provider) {
    case CalendarAccountProvider::ICloud:
      return "icloud";
    case CalendarAccountProvider::CustomCalDav:
      return "custom";
    case CalendarAccountProvider::Google:
      return "google";
    case CalendarAccountProvider::IcsFileURL:
      return "ics";
    case CalendarAccountProvider::Vdir:
      return "vdir";
    }
    return "icloud";
  }

  std::string calendarProviderTitle(CalendarAccountProvider provider) {
    switch (provider) {
    case CalendarAccountProvider::ICloud:
      return i18n::tr("settings.calendar-accounts.provider.icloud");
    case CalendarAccountProvider::CustomCalDav:
      return i18n::tr("settings.calendar-accounts.provider.custom");
    case CalendarAccountProvider::Google:
      return i18n::tr("settings.calendar-accounts.provider.google");
    case CalendarAccountProvider::IcsFileURL:
      return i18n::tr("settings.calendar-accounts.provider.ics");
    case CalendarAccountProvider::Vdir:
      return i18n::tr("settings.calendar-accounts.provider.vdir");
    }
    return i18n::tr("settings.calendar-accounts.provider.icloud");
  }

  bool calendarSourceChecked(const CalendarAccountDraft& draft, const CalendarSource& source) {
    return draft.calendars.empty() || std::ranges::contains(draft.calendars, source.id);
  }

  std::string trimInput(Input* input) { return input != nullptr ? StringUtils::trim(input->value()) : std::string{}; }

  } // namespace

void HostedEditors::openCalendarAccount(std::optional<std::string> accountId) {
  if (m_b.config == nullptr) {
    return;
  }
  prepareOpen();
  if (m_sheet == nullptr) {
    return;
  }

  const Config& cfg = m_b.config->config();
  auto draft = std::make_shared<CalendarAccountDraft>();
  if (accountId.has_value()) {
    const CalendarConfig::Account* account = findCalendarAccount(cfg, *accountId);
    if (account == nullptr
        || (account->type != "caldav"
            && account->type != "google"
            && account->type != "ics"
            && account->type != "vdir")) {
      return;
    }
    draft->creating = false;
    draft->id = account->id;
    draft->name = account->displayName;
    draft->username = account->username;
    draft->serverUrl = account->serverUrl;
    draft->credentialSource = account->credentialSource;
    draft->passwordFile = account->passwordFile;
    draft->color = account->color;
    draft->calendars = account->calendars;
    if (account->type == "google") {
      draft->provider = CalendarAccountProvider::Google;
    } else if (account->type == "ics") {
      draft->provider = CalendarAccountProvider::IcsFileURL;
    } else if (account->type == "vdir") {
      draft->provider = CalendarAccountProvider::Vdir;
      draft->path = account->path;
      const std::string rawDiscovery =
          m_b.config->stateString(kCalendarDiscoveryOwner, account->id + "_calendars").value_or(std::string{});
      draft->discoveredCalendars = calendar::parseCalendarSources(rawDiscovery);
    } else {
      draft->provider =
          account->provider == "custom" ? CalendarAccountProvider::CustomCalDav : CalendarAccountProvider::ICloud;
      const std::string rawDiscovery =
          m_b.config->stateString(kCalendarDiscoveryOwner, account->id + "_calendars").value_or(std::string{});
      draft->discoveredCalendars = calendar::parseCalendarSources(rawDiscovery);
    }
  }

  const float scale = uiScale();
  const std::string title = draft->creating ? i18n::tr("settings.calendar-accounts.add-title")
                                            : i18n::tr("settings.calendar-accounts.edit-title");

  std::function<void()> removeAccount;
  if (!draft->creating && m_b.config->isOverrideOnlyCalendarAccount(draft->id)) {
    removeAccount = [this, draft, accountId = draft->id]() {
      if (m_b.config == nullptr || m_b.calendar == nullptr) {
        return;
      }
      if (draft->credentialOperationInFlight) {
        return;
      }
      draft->credentialOperationInFlight = true;
      m_b.calendar->deleteAccount(
          accountId, [this, accountId]() { return m_b.config->deleteCalendarAccountOverride(accountId); },
          [this, draft, accountId](CalendarService::CredentialOperationResult result) {
            draft->credentialOperationInFlight = false;
            if (result != CalendarService::CredentialOperationResult::Success) {
              const std::string message = result == CalendarService::CredentialOperationResult::ConfigError
                  ? i18n::tr("settings.calendar-accounts.delete-error")
                  : calendarCredentialError(result);
              markWriteError(message);
              return;
            }
            (void)m_b.config->setStateString(kCalendarDiscoveryOwner, accountId + "_calendars", "");
            markWriteSuccess(true);
            if (m_sheet != nullptr) {
              m_sheet->close();
            }
          }
      );
    };
  }

  auto populateSheetBody = [this, draft, scale](Flex& body) mutable {
    if (m_b.config != nullptr && m_b.config->config().shell.offlineMode) {
      body.addChild(
          settings::makeOfflineModeNotice(scale, i18n::tr("settings.window.offline-mode-notice.calendar-account"))
      );
    }

    auto addField = [scale](Flex& parent, const std::string& label, std::unique_ptr<Node> control) {
      auto field = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      });
      field->addChild(
          ui::label({
              .text = label,
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      field->addChild(std::move(control));
      parent.addChild(std::move(field));
    };

    const auto providerIndex = [](CalendarAccountProvider provider) -> std::size_t {
      switch (provider) {
      case CalendarAccountProvider::ICloud:
        return 0;
      case CalendarAccountProvider::CustomCalDav:
        return 1;
      case CalendarAccountProvider::Google:
        return 2;
      case CalendarAccountProvider::IcsFileURL:
        return 3;
      case CalendarAccountProvider::Vdir:
        return 4;
      }
      return 0;
    };
    addField(
        body, i18n::tr("settings.calendar-accounts.provider-label"),
        ui::segmented({
            .options =
                std::vector<ui::SegmentedOption>{
                    {.label = calendarProviderTitle(CalendarAccountProvider::ICloud), .glyph = "brand-apple"},
                    {.label = calendarProviderTitle(CalendarAccountProvider::CustomCalDav), .glyph = "calendar-cog"},
                    {.label = calendarProviderTitle(CalendarAccountProvider::Google), .glyph = "brand-google"},
                    {.label = calendarProviderTitle(CalendarAccountProvider::IcsFileURL), .glyph = "link"},
                    {.label = calendarProviderTitle(CalendarAccountProvider::Vdir), .glyph = "folder"}
                },
            .selectedIndex = providerIndex(draft->provider),
            .scale = scale,
            .enabled = draft->creating,
            .equalSegmentWidths = true,
            .onChange = [this, draft](std::size_t index) {
              CalendarAccountProvider provider = CalendarAccountProvider::ICloud;
              if (index == 1) {
                provider = CalendarAccountProvider::CustomCalDav;
              } else if (index == 2) {
                provider = CalendarAccountProvider::Google;
              } else if (index == 3) {
                provider = CalendarAccountProvider::IcsFileURL;
              } else if (index == 4) {
                provider = CalendarAccountProvider::Vdir;
              }

              draft->provider = provider;
              if (provider == CalendarAccountProvider::Google || provider == CalendarAccountProvider::Vdir) {
                draft->credentialSource = CalendarCredentialSource::SecretService;
                draft->passwordFile.clear();
              }
              bool isDefaultId = draft->id.empty()
                  || draft->id == "personal_icloud"
                  || draft->id == "home_nextcloud"
                  || draft->id == "personal_google"
                  || draft->id == "subscription"
                  || draft->id == "local_calendar";
              if (provider == CalendarAccountProvider::Google && isDefaultId) {
                draft->id = "personal_google";
              } else if (provider == CalendarAccountProvider::CustomCalDav && isDefaultId) {
                draft->id = "home_nextcloud";
              } else if (provider == CalendarAccountProvider::ICloud && isDefaultId) {
                draft->id = "personal_icloud";
              } else if (provider == CalendarAccountProvider::IcsFileURL && isDefaultId) {
                draft->id = "subscription";
              } else if (provider == CalendarAccountProvider::Vdir && isDefaultId) {
                draft->id = "local_calendar";
              }
              if (m_sheet != nullptr) {
                m_sheet->rebuildBody();
              }
            },
        })
    );

    Input* idInput = nullptr;
    addField(
        body, i18n::tr("settings.calendar-accounts.id-label"),
        ui::input({
            .out = &idInput,
            .value = draft->id,
            .placeholder = "personal_icloud",
            .invalid = draft->idInvalid,
            .enabled = draft->creating,
            .onChange = [draft](const std::string& value) {
              if (draft->creating) {
                draft->id = value;
              }
              draft->idInvalid = false;
            },
        })
    );

    Input* nameInput = nullptr;
    addField(
        body, i18n::tr("settings.calendar-accounts.name-label"),
        ui::input({
            .out = &nameInput,
            .value = draft->name,
            .placeholder = i18n::tr("settings.calendar-accounts.name-placeholder"),
            .onChange = [draft](const std::string& value) { draft->name = value; },
        })
    );

    Input* usernameInput = nullptr;
    Input* passwordInput = nullptr;
    Input* passwordFileInput = nullptr;
    Input* serverInput = nullptr;
    Input* pathInput = nullptr;
    if (draft->provider != CalendarAccountProvider::Google
        && draft->provider != CalendarAccountProvider::IcsFileURL
        && draft->provider != CalendarAccountProvider::Vdir) {
      addField(
          body, i18n::tr("settings.calendar-accounts.credential-source-label"),
          ui::segmented({
              .options =
                  std::vector<ui::SegmentedOption>{
                      {.label = i18n::tr("settings.calendar-accounts.credential-source-keyring"), .glyph = "key"},
                      {.label = i18n::tr("settings.calendar-accounts.credential-source-file"), .glyph = "file-lock"},
                  },
              .selectedIndex = draft->credentialSource == CalendarCredentialSource::File ? 1U : 0U,
              .scale = scale,
              .enabled = draft->creating,
              .equalSegmentWidths = true,
              .onChange = [this, draft](std::size_t index) {
                draft->credentialSource =
                    index == 1 ? CalendarCredentialSource::File : CalendarCredentialSource::SecretService;
                draft->password.clear();
                draft->passwordFile.clear();
                draft->passwordInvalid = false;
                draft->passwordFileInvalid = false;
                if (m_sheet != nullptr) {
                  m_sheet->rebuildBody();
                }
              },
          })
      );
      addField(
          body, i18n::tr("settings.calendar-accounts.username-label"),
          ui::input({
              .out = &usernameInput,
              .value = draft->username,
              .placeholder = i18n::tr("settings.calendar-accounts.username-placeholder"),
              .invalid = draft->usernameInvalid,
              .onChange = [draft](const std::string& value) {
                draft->username = value;
                draft->usernameInvalid = false;
              },
          })
      );
      if (draft->credentialSource == CalendarCredentialSource::SecretService) {
        addField(
            body, i18n::tr("settings.calendar-accounts.password-label"),
            ui::input({
                .out = &passwordInput,
                .value = {},
                .placeholder = draft->creating ? i18n::tr("settings.calendar-accounts.password-placeholder")
                                               : i18n::tr("settings.calendar-accounts.password-keep-placeholder"),
                .passwordMode = true,
                .invalid = draft->passwordInvalid,
                .onChange = [draft](const std::string& value) {
                  draft->password = value;
                  draft->passwordInvalid = false;
                },
            })
        );
      } else {
        addField(
            body, i18n::tr("settings.calendar-accounts.password-file-label"),
            ui::input({
                .out = &passwordFileInput,
                .value = draft->passwordFile,
                .placeholder = "/run/agenix/noctalia-caldav",
                .invalid = draft->passwordFileInvalid,
                .enabled = draft->creating,
                .onChange = [draft](const std::string& value) {
                  draft->passwordFile = value;
                  draft->passwordFileInvalid = false;
                },
            })
        );
      }
    }
    if (draft->provider == CalendarAccountProvider::CustomCalDav) {
      addField(
          body, i18n::tr("settings.calendar-accounts.server-url-label"),
          ui::input({
              .out = &serverInput,
              .value = draft->serverUrl,
              .placeholder = "https://cloud.example.com/remote.php/dav/",
              .invalid = draft->serverUrlInvalid,
              .onChange = [draft](const std::string& value) {
                draft->serverUrl = value;
                draft->serverUrlInvalid = false;
              },
          })
      );
    }
    if (draft->provider == CalendarAccountProvider::IcsFileURL) {
      addField(
          body, i18n::tr("settings.calendar-accounts.ics-url-label"),
          ui::input({
              .out = &serverInput,
              .value = draft->serverUrl,
              .placeholder = "https://example.com/calendar.ics",
              .invalid = draft->serverUrlInvalid,
              .onChange = [draft](const std::string& value) {
                draft->serverUrl = value;
                draft->serverUrlInvalid = false;
              },
          })
      );
    }
    if (draft->provider == CalendarAccountProvider::Vdir) {
      addField(
          body, i18n::tr("settings.calendar-accounts.vdir-path-label"),
          ui::input({
              .out = &pathInput,
              .value = draft->path,
              .placeholder = "~/.local/share/calendars",
              .invalid = draft->pathInvalid,
              .onChange = [draft](const std::string& value) {
                draft->path = value;
                draft->pathInvalid = false;
              },
          })
      );
    }

    addField(
        body, i18n::tr("settings.calendar-accounts.color-label"),
        settings::makeColorSpecSelect(
            settings::ColorSpecSelectOptions{
                .roles = {},
                .selectedValue = draft->color,
                .allowNone = true,
                .allowCustomColor = true,
                .noneLabel = {},
                .fontSize = Style::fontSizeBody * scale,
                .controlHeight = Style::controlHeight * scale,
                .glyphSize = Style::fontSizeBody * scale,
                .flexGrow = true,
            },
            [draft](std::string value) { draft->color = StringUtils::trim(value); }, [draft]() { draft->color.clear(); }
        )
    );

    if (!draft->creating && draft->provider != CalendarAccountProvider::Google && !draft->discoveredCalendars.empty()) {
      auto calendars = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      });
      calendars->addChild(
          ui::label({
              .text = i18n::tr("settings.calendar-accounts.calendars-label"),
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );

      auto list = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .padding = Style::spaceSm * scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.35F),
          .radius = Style::scaledRadiusMd(scale),
      });
      for (const CalendarSource& source : draft->discoveredCalendars) {
        const bool checked = calendarSourceChecked(*draft, source);
        auto row = ui::row({
            .align = FlexAlign::Center,
            .gap = Style::spaceSm * scale,
            .fillWidth = true,
        });
        auto info = ui::column({
            .align = FlexAlign::Start,
            .gap = 2.0F * scale,
            .flexGrow = 1.0F,
        });
        info->addChild(
            ui::label({
                .text = source.name.empty() ? source.id : source.name,
                .fontSize = Style::fontSizeBody * scale,
                .fontWeight = FontWeight::Medium,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
            })
        );
        if (!source.name.empty()) {
          info->addChild(
              ui::label({
                  .text = source.id,
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  .ellipsize = TextEllipsize::End,
              })
          );
        }
        row->addChild(std::move(info));
        row->addChild(
            ui::toggle({
                .checked = checked,
                .scale = scale,
                .onChange = [this, draft, sourceId = source.id](bool on) {
                  draft->calendars =
                      calendar::setCalendarSourceChecked(draft->discoveredCalendars, draft->calendars, sourceId, on);
                  if (m_sheet != nullptr) {
                    m_sheet->rebuildBody();
                  }
                },
            })
        );
        list->addChild(std::move(row));
      }
      calendars->addChild(std::move(list));
      body.addChild(std::move(calendars));
    }

    const auto persistAccount = [this, draft, idInput, nameInput, usernameInput, passwordInput, passwordFileInput,
                                 serverInput, pathInput](bool closeAfter, bool connectAfter) {
      if (m_b.config == nullptr) {
        return;
      }
      if (draft->credentialOperationInFlight) {
        return;
      }

      draft->id = draft->creating ? trimInput(idInput) : draft->id;
      draft->name = trimInput(nameInput);
      draft->color = StringUtils::trim(draft->color);
      draft->username = trimInput(usernameInput);
      draft->password = trimInput(passwordInput);
      if (passwordFileInput != nullptr) {
        draft->passwordFile = trimInput(passwordFileInput);
      }
      draft->serverUrl = trimInput(serverInput);
      if (pathInput != nullptr) {
        draft->path = trimInput(pathInput);
      }

      draft->idInvalid = false;
      draft->usernameInvalid = false;
      draft->passwordInvalid = false;
      draft->passwordFileInvalid = false;
      draft->serverUrlInvalid = false;
      draft->pathInvalid = false;

      if (!validCalendarAccountId(draft->id)) {
        draft->idInvalid = true;
      }
      if (draft->creating && calendarAccountIdExists(m_b.config->config(), draft->id)) {
        draft->idInvalid = true;
      }

      const bool caldav = draft->provider == CalendarAccountProvider::ICloud
          || draft->provider == CalendarAccountProvider::CustomCalDav;
      const bool ics = draft->provider == CalendarAccountProvider::IcsFileURL;
      const bool vdir = draft->provider == CalendarAccountProvider::Vdir;
      if (caldav && draft->username.empty()) {
        draft->usernameInvalid = true;
      }
      if ((draft->provider == CalendarAccountProvider::CustomCalDav || ics) && draft->serverUrl.empty()) {
        draft->serverUrlInvalid = true;
      }
      if (caldav
          && draft->credentialSource == CalendarCredentialSource::File
          && (draft->passwordFile.empty() || !std::filesystem::path(draft->passwordFile).is_absolute())) {
        draft->passwordFileInvalid = true;
      }
      if (vdir) {
        const std::filesystem::path checkPath =
            draft->path.empty() ? calendar::defaultVdirPath() : FileUtils::expandUserPath(draft->path);
        std::error_code ec;
        if (!std::filesystem::exists(checkPath, ec) || !std::filesystem::is_directory(checkPath, ec)) {
          draft->pathInvalid = true;
        }
      }
      if (draft->idInvalid
          || draft->usernameInvalid
          || draft->passwordInvalid
          || draft->passwordFileInvalid
          || draft->serverUrlInvalid
          || draft->pathInvalid) {
        showStatus(i18n::tr("settings.calendar-accounts.invalid"), true);
        return;
      }

      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
      if (draft->creating) {
        overrides.push_back({{"calendar", "enabled"}, true});
      }
      const std::vector<std::string> base = {"calendar", "account", draft->id};

      std::string type = "caldav";
      if (draft->provider == CalendarAccountProvider::Google)
        type = "google";
      else if (ics)
        type = "ics";
      else if (vdir)
        type = "vdir";

      overrides.push_back({{base[0], base[1], base[2], "type"}, type});
      overrides.push_back({{base[0], base[1], base[2], "name"}, draft->name});
      overrides.push_back({{base[0], base[1], base[2], "color"}, draft->color});
      // Manual calendar selection is currently populated by CalDAV discovery; Google uses CalendarList selected.
      overrides.push_back({{base[0], base[1], base[2], "calendars"}, draft->calendars});
      if (caldav) {
        overrides.push_back({{base[0], base[1], base[2], "provider"}, calendarProviderKey(draft->provider)});
        overrides.push_back({{base[0], base[1], base[2], "username"}, draft->username});
        if (draft->creating) {
          overrides.push_back(
              {{base[0], base[1], base[2], "credential_source"},
               draft->credentialSource == CalendarCredentialSource::File ? std::string("file")
                                                                         : std::string("secret-service")}
          );
          overrides.push_back({{base[0], base[1], base[2], "password_file"}, draft->passwordFile});
        }
      }
      if (draft->provider == CalendarAccountProvider::CustomCalDav || ics) {
        overrides.push_back({{base[0], base[1], base[2], "server_url"}, draft->serverUrl});
      }
      if (vdir) {
        overrides.push_back({{base[0], base[1], base[2], "path"}, draft->path});
      }

      // Keys the target provider does not own. They ship in the same commit as the new values: a
      // separate clear would publish an account that is neither the old shape nor the new one.
      std::vector<std::vector<std::string>> staleKeys;
      if (!draft->creating) {
        if (!caldav) {
          staleKeys.push_back({base[0], base[1], base[2], "provider"});
          staleKeys.push_back({base[0], base[1], base[2], "username"});
          staleKeys.push_back({base[0], base[1], base[2], "credential_source"});
          staleKeys.push_back({base[0], base[1], base[2], "password_file"});
        }
        if (draft->provider != CalendarAccountProvider::CustomCalDav && !ics) {
          staleKeys.push_back({base[0], base[1], base[2], "server_url"});
        }
        if (!vdir) {
          staleKeys.push_back({base[0], base[1], base[2], "path"});
        }
      }

      std::string connectActivationToken;
      if (connectAfter && m_b.requestActivationToken) {
        connectActivationToken = m_b.requestActivationToken();
      }

      if (!caldav) {
        if (!m_b.config->mutateOverrides(overrides, staleKeys, nullptr)) {
          markWriteError(i18n::tr("settings.calendar-accounts.save-error"));
          return;
        }
        markWriteSuccess(closeAfter);
        if (connectAfter && m_b.calendar != nullptr) {
          DeferredCall::callLater([this, accountId = draft->id, activationToken = std::move(connectActivationToken)]() {
            m_b.calendar->connectGoogleAccount(accountId, activationToken);
          });
        }
        if (closeAfter && m_sheet != nullptr) {
          m_sheet->close();
        }
        return;
      }

      if (m_b.calendar == nullptr) {
        markWriteError(i18n::tr("settings.calendar-accounts.secret-service-error"));
        return;
      }
      std::string password = std::move(draft->password);
      draft->password.clear();
      if (passwordInput != nullptr) {
        passwordInput->setValue("");
      }
      draft->credentialOperationInFlight = true;
      m_b.calendar->saveCalDavAccount(
          draft->id, draft->credentialSource, draft->passwordFile, std::move(password),
          [this, overrides = std::move(overrides), staleKeys = std::move(staleKeys)]() mutable {
            return m_b.config->mutateOverrides(overrides, staleKeys, nullptr);
          },
          [this, draft, closeAfter](CalendarService::CredentialOperationResult result) {
            draft->credentialOperationInFlight = false;
            if (result == CalendarService::CredentialOperationResult::MissingCredential) {
              draft->passwordInvalid = true;
              showStatus(i18n::tr("settings.calendar-accounts.invalid"), true);
              if (m_sheet != nullptr) {
                m_sheet->rebuildBody();
              }
              return;
            }
            if (result != CalendarService::CredentialOperationResult::Success) {
              markWriteError(calendarCredentialError(result));
              return;
            }
            m_b.calendar->requestRefresh();
            markWriteSuccess(closeAfter);
            if (closeAfter && m_sheet != nullptr) {
              m_sheet->close();
            }
          }
      );
    };

    auto actions = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::End,
        .gap = Style::spaceSm * scale,
    });
    actions->addChild(
        ui::button({
            .text = i18n::tr("common.actions.cancel"),
            .variant = ButtonVariant::Secondary,
            .minHeight = Style::controlHeight * scale,
            .paddingH = Style::spaceMd * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [this]() {
              if (m_sheet != nullptr) {
                m_sheet->close();
              }
            },
        })
    );
    const bool google = draft->provider == CalendarAccountProvider::Google;
    if (!draft->creating && google) {
      actions->addChild(
          ui::button({
              .text = i18n::tr("settings.calendar-accounts.save"),
              .glyph = "device-floppy",
              .variant = ButtonVariant::Secondary,
              .minHeight = Style::controlHeight * scale,
              .paddingH = Style::spaceMd * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [persistAccount]() { persistAccount(true, false); },
          })
      );
    }
    actions->addChild(
        ui::button({
            .text = google ? i18n::tr("settings.calendar-accounts.save-connect")
                           : i18n::tr("settings.calendar-accounts.save"),
            .glyph = google ? "brand-google" : "device-floppy",
            .variant = ButtonVariant::Primary,
            .minHeight = Style::controlHeight * scale,
            .paddingH = Style::spaceMd * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [persistAccount, google]() { persistAccount(true, google); },
        })
    );
    body.addChild(std::move(actions));
  };

  m_sheet->open(
      settings::SettingsSheetRequest{
          .sheetTitle = title,
          .removeAction = removeAccount,
          .populateSheetBody = std::move(populateSheetBody),
          .scale = scale,
      }
  );
}


void HostedEditors::openPluginSource(std::optional<PluginSourceConfig> existing) {
  DeferredCall::callLater([this, existing = std::move(existing)]() {
    if (m_b.config == nullptr || m_b.pluginManager == nullptr) {
      return;
    }

    auto draft = std::make_shared<PluginSourceDraft>();
    if (existing.has_value()) {
      draft->kind = existing->kind;
      draft->name = existing->name;
      draft->location = existing->location;
      draft->enabled = existing->enabled;
      draft->editing = true;
    }
    const bool nameLocked = draft->editing;
    const bool fieldsLocked = draft->editing && isDefaultPluginSourceName(draft->name);
    const std::string title = draft->editing ? i18n::tr("settings.plugins.sources.edit-title")
                                             : i18n::tr("settings.plugins.sources.add-title");

    std::function<void()> removeAction;
    if (draft->editing && !isDefaultPluginSourceName(draft->name)) {
      removeAction = [this, name = draft->name]() {
        if (m_b.pluginManager == nullptr) {
          return;
        }
        m_b.pluginManager->removeSource(name);
        if (m_b.onPluginToggled) { m_b.onPluginToggled({}, false, nullptr); };
        markWriteSuccess(false);
        DeferredCall::callLater([this]() {
          if (m_sheet != nullptr) {
            m_sheet->close();
          }
          if (m_b.requestRebuild) { m_b.requestRebuild(); };
        });
      };
    }

    openBarWidgetEditorSheet(
        title,
        [this, draft, nameLocked, fieldsLocked](Flex& body) {
          const float scale = uiScale();
          auto addField = [scale](Flex& parent, const std::string& label, std::unique_ptr<Node> control) {
            auto field = ui::column({
                .align = FlexAlign::Stretch,
                .gap = Style::spaceXs * scale,
            });
            field->addChild(
                ui::label({
                    .text = label,
                    .fontSize = Style::fontSizeCaption * scale,
                    .fontWeight = FontWeight::Medium,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                })
            );
            field->addChild(std::move(control));
            parent.addChild(std::move(field));
          };

          if (!draft->error.empty()) {
            body.addChild(
                ui::label({
                    .text = draft->error,
                    .fontSize = Style::fontSizeCaption * scale,
                    .fontWeight = FontWeight::Medium,
                    .color = colorSpecFromRole(ColorRole::Error),
                })
            );
          }

          addField(
              body, i18n::tr("settings.plugins.sources.kind-label"),
              ui::segmented({
                  .options =
                      std::vector<ui::SegmentedOption>{
                          {.label = i18n::tr("settings.plugins.sources.kind.git"), .glyph = "brand-git"},
                          {.label = i18n::tr("settings.plugins.sources.kind.path"), .glyph = "folder"},
                      },
                  .selectedIndex = pluginSourceKindIndex(draft->kind),
                  .scale = scale,
                  .enabled = !fieldsLocked,
                  .equalSegmentWidths = true,
                  .onChange =
                      [this, draft](std::size_t index) {
                        draft->kind = index == 1 ? PluginSourceKind::Path : PluginSourceKind::Git;
                        draft->error.clear();
                        if (m_sheet != nullptr) {
                          m_sheet->rebuildBody();
                        }
                      },
                  .configure =
                      [](Segmented& seg) {
                        if (seg.focusArea() != nullptr) {
                          // Stable key so rebuildBody can restore Left/Right focus after kind changes.
                          seg.focusArea()->setTabFocusKey("plugin-source-kind");
                        }
                      },
              })
          );

          Input* nameInput = nullptr;
          addField(
              body, i18n::tr("settings.plugins.sources.name-label"),
              ui::input({
                  .out = &nameInput,
                  .value = draft->name,
                  .placeholder = i18n::tr("settings.plugins.sources.name-placeholder"),
                  .invalid = draft->nameInvalid,
                  .enabled = !nameLocked,
                  .onChange = [draft](const std::string& value) {
                    draft->name = value;
                    draft->nameInvalid = false;
                    draft->error.clear();
                  },
              })
          );

          Input* locationInput = nullptr;
          addField(
              body, i18n::tr("settings.plugins.sources.location-label"),
              ui::input({
                  .out = &locationInput,
                  .value = draft->location,
                  .placeholder = draft->kind == PluginSourceKind::Git
                      ? i18n::tr("settings.plugins.sources.location-placeholder-git")
                      : i18n::tr("settings.plugins.sources.location-placeholder-path"),
                  .invalid = draft->locationInvalid,
                  .enabled = !fieldsLocked,
                  .onChange = [draft](const std::string& value) {
                    draft->location = value;
                    draft->locationInvalid = false;
                    draft->error.clear();
                  },
              })
          );

          auto actions = ui::row({
              .align = FlexAlign::Center,
              .justify = FlexJustify::End,
              .gap = Style::spaceSm * scale,
              .fillWidth = true,
          });
          actions->addChild(
              ui::button({
                  .text = i18n::tr("common.actions.cancel"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .variant = ButtonVariant::Default,
                  .onClick = [this]() {
                    if (m_sheet != nullptr) {
                      m_sheet->close();
                    }
                  },
              })
          );
          actions->addChild(
              ui::button({
                  .text = draft->editing ? i18n::tr("settings.plugins.sources.save")
                                         : i18n::tr("settings.plugins.sources.add"),
                  .glyph = draft->editing ? "device-floppy" : "add",
                  .fontSize = Style::fontSizeCaption * scale,
                  .glyphSize = Style::fontSizeBody * scale,
                  .variant = ButtonVariant::Primary,
                  .onClick = [this, draft, nameInput, locationInput]() {
                    if (m_b.config == nullptr || m_b.pluginManager == nullptr) {
                      return;
                    }
                    draft->name = StringUtils::trim(nameInput != nullptr ? nameInput->value() : draft->name);
                    draft->location =
                        StringUtils::trim(locationInput != nullptr ? locationInput->value() : draft->location);
                    draft->nameInvalid = false;
                    draft->locationInvalid = false;
                    draft->error.clear();

                    if (!isValidPluginSourceName(draft->name)) {
                      draft->nameInvalid = true;
                      draft->error = i18n::tr("settings.plugins.sources.errors.invalid-name");
                    } else if (!draft->editing && pluginSourceNameExists(m_b.config->config(), draft->name)) {
                      draft->nameInvalid = true;
                      draft->error = i18n::tr("settings.plugins.sources.errors.duplicate-name");
                    } else if (draft->location.empty()) {
                      draft->locationInvalid = true;
                      draft->error = i18n::tr("settings.plugins.sources.errors.location-required");
                    }

                    if (!draft->error.empty()) {
                      if (m_sheet != nullptr) {
                        m_sheet->rebuildBody();
                      }
                      return;
                    }

                    m_b.pluginManager->addSource(
                        PluginSourceConfig{
                            .kind = draft->kind,
                            .name = draft->name,
                            .location = draft->location,
                            .enabled = draft->enabled,
                        }
                    );
                    if (m_b.onPluginToggled) { m_b.onPluginToggled({}, false, nullptr); };
                    markWriteSuccess(false);
                    DeferredCall::callLater([this]() {
                      if (m_sheet != nullptr) {
                        m_sheet->close();
                      }
                      if (m_b.requestRebuild) { m_b.requestRebuild(); };
                    });
                  },
              })
          );
          body.addChild(std::move(actions));
        },
        std::move(removeAction)
    );
  });
}


void HostedEditors::openPluginSettings(std::string pluginId) {
  DeferredCall::callLater([this, pluginId = std::move(pluginId)]() mutable {
    if (m_b.config == nullptr) {
      return;
    }
    const auto* manifest = scripting::PluginRegistry::instance().findManifest(pluginId);
    if (manifest == nullptr || !settings::pluginHasSettings(*manifest)) {
      return;
    }

    if (m_b.editingWidgetName != nullptr) {
      m_b.editingWidgetName->clear();
    }
    if (m_b.editingCapsuleGroupId != nullptr) {
      m_b.editingCapsuleGroupId->clear();
    }
    if (m_b.renamingWidgetName != nullptr) {
      m_b.renamingWidgetName->clear();
    }
    if (m_b.pendingDeleteWidgetName != nullptr) {
      m_b.pendingDeleteWidgetName->clear();
    }
    if (m_b.pendingDeleteWidgetSettingPath != nullptr) {
      m_b.pendingDeleteWidgetSettingPath->clear();
    }
    m_listPath.clear();

    const std::string title = manifest->name.empty() ? pluginId : manifest->name;
    openBarWidgetEditorSheet(title, [this, pluginId](Flex& body) {
      SettingsControlFactory* factory = m_b.editorFactory != nullptr ? m_b.editorFactory->get() : m_factory.get();
      if (m_b.config == nullptr || factory == nullptr) {
        return;
      }
      const auto* currentManifest = scripting::PluginRegistry::instance().findManifest(pluginId);
      if (currentManifest == nullptr) {
        return;
      }
      settings::buildPluginSettingsEditor(
          body, m_b.config->config(), *factory, pluginId, *currentManifest,
          m_b.showAdvanced ? m_b.showAdvanced() : true, uiScale()
      );
    });
  });
}


void HostedEditors::openCommunityTemplateStore() {
  if (m_b.config == nullptr) {
    return;
  }
  prepareOpen();
  if (m_sheet == nullptr) {
    return;
  }

  const float scale = uiScale();
  auto catalog = noctalia::theme::CommunityTemplateService::availableTemplates();
  std::unordered_set<std::string> selectedIds(
      m_b.config->config().theme.templates.communityIds.begin(), m_b.config->config().theme.templates.communityIds.end()
  );

  auto storeContent = std::make_shared<settings::TemplateStoreContent>(
      std::move(catalog), std::move(selectedIds), m_b.config,
      settings::TemplateStoreCallbacks{
          .setSelected = [this](std::vector<std::string> ids) {
            if (m_b.setOverride) {
              m_b.setOverride({"theme", "templates", "community_ids"}, std::move(ids));
            }
          },
          .scale = scale,
      }
  );

  storeContent->setOnRebuildNeeded([this]() {
    if (m_sheet != nullptr) {
      m_sheet->rebuildBody();
    }
  });

  m_sheet->open(
      settings::SettingsSheetRequest{
          .sheetTitle = i18n::tr("settings.templates.store.title"),
          .removeAction = nullptr,
          .createHeaderAction = nullptr,
          .populateSheetBody =
              [storeContent, this, scale](Flex& body) {
                Renderer* renderer = m_b.renderer ? m_b.renderer() : nullptr;
                if (renderer == nullptr) {
                  return;
                }
                if (m_b.config != nullptr && m_b.config->config().shell.offlineMode) {
                  body.addChild(
                      settings::makeOfflineModeNotice(
                          scale, i18n::tr("settings.window.offline-mode-notice.template-store")
                      )
                  );
                }
                storeContent->populateBody(body, *renderer);
              },
          .scale = scale,
          .minWidth = 720.0F,
          .maxWidth = 1000.0F,
          .parentFraction = 0.85F,
          .fillParentHeight = true,
          .scrollableBody = false,
          .preDispatchKeyboard =
              [storeContent, this](const KeyboardEvent& event) {
                InputArea* focused = m_sheet != nullptr ? m_sheet->focusedArea() : nullptr;
                return storeContent->handleKeyEvent(event.sym, event.modifiers, event.pressed, event.preedit, focused);
              },
      }
  );
}



  void HostedEditors::showStatus(std::string message, bool error) {
    if (m_sheet != nullptr && m_sheet->isOpen()) {
      m_sheet->setStatusMessage(std::move(message), error);
      return;
    }
    if (m_b.setHostStatus) {
      m_b.setHostStatus(std::move(message), error);
    }
  }

  void HostedEditors::markWriteError(std::string message) { showStatus(std::move(message), true); }

  void HostedEditors::markWriteSuccess(bool closeSheet) {
    if (m_sheet != nullptr && m_sheet->isOpen()) {
      m_sheet->clearStatusMessage();
    }
    if (m_b.setHostStatus) {
      m_b.setHostStatus({}, false);
    }
    if (closeSheet && m_sheet != nullptr) {
      m_sheet->close();
    }
    if (m_b.requestRebuild) {
      m_b.requestRebuild();
    }
  }

  void HostedEditors::invalidatePluginSource(const std::string& sourceName) {
    m_fileCache.invalidateSource(sourceName);
    if (m_b.pluginFileCache != nullptr) {
      m_b.pluginFileCache->invalidateSource(sourceName);
    }
  }

  void HostedEditors::openSearchPicker(SearchPickerOpenRequest request) {
    if (m_b.config == nullptr || request.options.empty()) {
      return;
    }
    prepareOpen();
    if (m_sheet == nullptr) {
      return;
    }
    const float scale = uiScale();
    auto selected = std::make_shared<std::string>(request.selectedValue);
    auto settingPath = request.settingPath;
    auto onSelect = request.onSelect;
    auto options = std::move(request.options);
    m_sheet->open(
        SettingsSheetRequest{
            .sheetTitle = std::move(request.title),
            .populateSheetBody =
                [this, options = std::move(options), selected, settingPath, onSelect, placeholder = request.placeholder,
                 emptyText = request.emptyText, scale](Flex& body) {
                  auto picker = std::make_unique<SearchPicker>();
                  std::vector<SearchPickerOption> pickerOptions;
                  pickerOptions.reserve(options.size());
                  for (const auto& opt : options) {
                    pickerOptions.push_back(
                        SearchPickerOption{
                            .value = opt.value,
                            .label = opt.label,
                            .description = opt.description,
                            .enabled = true,
                            .icon = {},
                            .preview = opt.preview,
                        }
                    );
                  }
                  picker->setOptions(std::move(pickerOptions));
                  picker->setPlaceholder(placeholder);
                  picker->setEmptyText(emptyText);
                  picker->setSelectedValue(*selected);
                  picker->setOnActivated([this, settingPath, selected, onSelect](const SearchPickerOption& option) {
                    if (option.value == *selected) {
                      close();
                      return;
                    }
                    if (onSelect) {
                      onSelect(option.value);
                    } else if (option.value.empty()) {
                      if (m_b.makeContext) {
                        auto ctx = m_b.makeContext();
                        if (ctx.clearOverride) {
                          ctx.clearOverride(settingPath);
                        }
                      }
                    } else if (m_b.setOverride) {
                      m_b.setOverride(settingPath, option.value);
                    }
                    close();
                  });
                  picker->setOnCancel([this]() { close(); });
                  picker->setFlexGrow(1.0F);
                  body.addChild(std::move(picker));
                },
            .scale = scale,
            .minWidth = 520.0F,
            .maxWidth = 720.0F,
            .parentFraction = 0.7F,
            .fillParentHeight = true,
            .scrollableBody = false,
        }
    );
  }

} // namespace settings
