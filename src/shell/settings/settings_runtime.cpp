#include "shell/settings/settings_runtime.h"

#include "calendar/calendar_service.h"
#include "i18n/i18n.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

namespace settings {
  namespace {

    template <typename Pred>
    auto insertAfter(std::vector<SettingEntry>& registry, Pred pred) {
      auto it = std::ranges::find_if(registry, pred);
      if (it != registry.end()) {
        ++it;
      }
      return it;
    }

  } // namespace

  void enrichSettingsRegistry(
      std::vector<SettingEntry>& registry, const Config& cfg, const RegistryEnvironment& env,
      const RuntimeActions& actions
  ) {
    if (actions.openCommunityTemplateStore) {
      for (auto& entry : registry) {
        if (entry.section != SettingsSection::Templates || entry.group != "community") {
          continue;
        }
        if (auto* button = std::get_if<ButtonSetting>(&entry.control)) {
          button->action = actions.openCommunityTemplateStore;
        }
      }
    }

    if (actions.calendar != nullptr
        && (actions.calendar->credentialMigrationPending()
            || actions.calendar->credentialState() != calendar::CredentialState::Ready)
        && actions.retryCalendarCredentials) {
      std::string descriptionKey = "settings.schema.services.calendar-credentials.description-error";
      switch (actions.calendar->credentialState()) {
      case calendar::CredentialState::Opening:
        descriptionKey = "settings.schema.services.calendar-credentials.description-opening";
        break;
      case calendar::CredentialState::Unavailable:
        descriptionKey = "settings.schema.services.calendar-credentials.description-unavailable";
        break;
      case calendar::CredentialState::Cancelled:
        descriptionKey = "settings.schema.services.calendar-credentials.description-cancelled";
        break;
      case calendar::CredentialState::DeniedOrLocked:
        descriptionKey = "settings.schema.services.calendar-credentials.description-locked";
        break;
      case calendar::CredentialState::BackendError:
      case calendar::CredentialState::Ready:
        break;
      }
      if (actions.calendar->credentialMigrationPending()
          && (actions.calendar->credentialState() == calendar::CredentialState::Opening
              || actions.calendar->credentialState() == calendar::CredentialState::Ready)) {
        descriptionKey = "settings.schema.services.calendar-credentials.description-migration";
      }
      auto it = insertAfter(registry, [](const SettingEntry& entry) {
        return entry.section == SettingsSection::Services && entry.group == "calendar";
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Services,
              .group = "calendar",
              .title = i18n::tr("settings.schema.services.calendar-credentials.label"),
              .subtitle = i18n::tr(descriptionKey),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.services.calendar-credentials.button"),
                      .action = actions.retryCalendarCredentials,
                      .glyph = "refresh",
                      .variant = ButtonVariant::Secondary,
                  },
              .searchText = "calendar credentials keyring secret service retry migration unlock",
          }
      );
    }

    if (actions.calendar != nullptr
        && (actions.calendar->cacheMigrationPending()
            || actions.calendar->cachePersistenceState() != CalendarService::CachePersistenceState::Ready)
        && actions.retryCalendarStorage) {
      std::string descriptionKey = "settings.schema.services.calendar-storage.description-error";
      switch (actions.calendar->cachePersistenceState()) {
      case CalendarService::CachePersistenceState::Opening:
        descriptionKey = "settings.schema.services.calendar-storage.description-opening";
        break;
      case CalendarService::CachePersistenceState::Unavailable:
        descriptionKey = "settings.schema.services.calendar-storage.description-unavailable";
        break;
      case CalendarService::CachePersistenceState::Cancelled:
        descriptionKey = "settings.schema.services.calendar-storage.description-cancelled";
        break;
      case CalendarService::CachePersistenceState::DeniedOrLocked:
        descriptionKey = "settings.schema.services.calendar-storage.description-locked";
        break;
      case CalendarService::CachePersistenceState::MissingKey:
        descriptionKey = "settings.schema.services.calendar-storage.description-missing-key";
        break;
      case CalendarService::CachePersistenceState::RecoveryRequired:
        descriptionKey = "settings.schema.services.calendar-storage.description-recovery";
        break;
      case CalendarService::CachePersistenceState::BackendError:
      case CalendarService::CachePersistenceState::Ready:
        break;
      }
      if (actions.calendar->cacheMigrationPending()
          && (actions.calendar->cachePersistenceState() == CalendarService::CachePersistenceState::Opening
              || actions.calendar->cachePersistenceState() == CalendarService::CachePersistenceState::Ready)) {
        descriptionKey = "settings.schema.services.calendar-storage.description-migration";
      }
      auto it = insertAfter(registry, [](const SettingEntry& entry) {
        return entry.section == SettingsSection::Services && entry.group == "calendar";
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Services,
              .group = "calendar",
              .title = i18n::tr("settings.schema.services.calendar-storage.label"),
              .subtitle = i18n::tr(descriptionKey),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.services.calendar-storage.button"),
                      .action = actions.retryCalendarStorage,
                      .glyph = "refresh",
                      .variant = ButtonVariant::Secondary,
                  },
              .searchText = "calendar events cache encryption storage keyring retry migration unlock",
              .visibleWhen = [](const Config& config) { return config.calendar.enabled; },
          }
      );
    }

    if (actions.clipboard != nullptr
        && (actions.clipboard->persistenceMigrationPending()
            || actions.clipboard->persistenceState() != ClipboardPersistenceState::Ready)
        && actions.retryClipboardPersistence) {
      std::string descriptionKey = "settings.schema.shell.clipboard-storage.description-error";
      switch (actions.clipboard->persistenceState()) {
      case ClipboardPersistenceState::Opening:
        descriptionKey = "settings.schema.shell.clipboard-storage.description-opening";
        break;
      case ClipboardPersistenceState::Unavailable:
        descriptionKey = "settings.schema.shell.clipboard-storage.description-unavailable";
        break;
      case ClipboardPersistenceState::Cancelled:
        descriptionKey = "settings.schema.shell.clipboard-storage.description-cancelled";
        break;
      case ClipboardPersistenceState::DeniedOrLocked:
        descriptionKey = "settings.schema.shell.clipboard-storage.description-locked";
        break;
      case ClipboardPersistenceState::MissingKey:
        descriptionKey = "settings.schema.shell.clipboard-storage.description-missing-key";
        break;
      case ClipboardPersistenceState::RecoveryRequired:
        descriptionKey = "settings.schema.shell.clipboard-storage.description-recovery";
        break;
      case ClipboardPersistenceState::BackendError:
      case ClipboardPersistenceState::Ready:
        break;
      }
      if (actions.clipboard->persistenceMigrationPending()
          && (actions.clipboard->persistenceState() == ClipboardPersistenceState::Opening
              || actions.clipboard->persistenceState() == ClipboardPersistenceState::Ready)) {
        descriptionKey = "settings.schema.shell.clipboard-storage.description-migration";
      }
      auto it = insertAfter(registry, [](const SettingEntry& entry) {
        return entry.section == SettingsSection::Shell && entry.group == "clipboard";
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Shell,
              .group = "clipboard",
              .title = i18n::tr("settings.schema.shell.clipboard-storage.label"),
              .subtitle = i18n::tr(descriptionKey),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.shell.clipboard-storage.button"),
                      .action = actions.retryClipboardPersistence,
                      .glyph = "refresh",
                  },
              .searchText = "clipboard history encryption keyring secret service retry migration unlock",
              .visibleWhen = [](const Config& config) { return config.shell.clipboardEnabled; },
          }
      );
    }

    const bool calendarStorageRecovery = actions.calendar != nullptr
        && (actions.calendar->cachePersistenceState() == CalendarService::CachePersistenceState::MissingKey
            || actions.calendar->cachePersistenceState() == CalendarService::CachePersistenceState::RecoveryRequired);
    const bool clipboardStorageRecovery = actions.clipboard != nullptr
        && (actions.clipboard->persistenceState() == ClipboardPersistenceState::MissingKey
            || actions.clipboard->persistenceState() == ClipboardPersistenceState::RecoveryRequired);
    if (!calendarStorageRecovery && !clipboardStorageRecovery && actions.pendingEncryptedStorageReset != nullptr) {
      *actions.pendingEncryptedStorageReset = false;
    }

    const auto insertStorageRecovery = [&](SettingsSection section, std::string group) {
      if (actions.resetEncryptedStorage == nullptr || actions.pendingEncryptedStorageReset == nullptr) {
        return;
      }
      auto it = insertAfter(registry, [section, &group](const SettingEntry& entry) {
        return entry.section == section && entry.group == group;
      });
      const bool pendingConfirmation = *actions.pendingEncryptedStorageReset;
      registry.insert(
          it,
          SettingEntry{
              .section = section,
              .group = std::move(group),
              .title = i18n::tr("settings.storage-recovery.label"),
              .subtitle = i18n::tr("settings.storage-recovery.description"),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr(
                          pendingConfirmation ? "settings.storage-recovery.button-confirm"
                                           : "settings.storage-recovery.button"
                      ),
                      .action =
                          [pendingConfirmation, pending = actions.pendingEncryptedStorageReset,
                           reset = actions.resetEncryptedStorage, rebuild = actions.requestRebuild]() {
                            if (pending == nullptr) {
                              return;
                            }
                            if (!pendingConfirmation) {
                              *pending = true;
                              if (rebuild) {
                                rebuild();
                              }
                              return;
                            }
                            *pending = false;
                            if (reset) {
                              reset();
                            }
                          },
                      .glyph = pendingConfirmation ? "warning" : "trash",
                      .variant = pendingConfirmation ? ButtonVariant::Destructive : ButtonVariant::Default,
                  },
              .searchText = "reset recover encrypted private storage clipboard calendar key",
          }
      );
    };

    if (calendarStorageRecovery) {
      insertStorageRecovery(SettingsSection::Services, "calendar");
    }
    if (clipboardStorageRecovery) {
      insertStorageRecovery(SettingsSection::Shell, "clipboard");
    }

    if (actions.syncGreeterAppearance && env.greeterSyncAvailable) {
      auto it = std::ranges::find_if(registry, [](const SettingEntry& e) {
        return e.section == SettingsSection::Security
            && e.group == "greeter"
            && e.path == std::vector<std::string>{"shell", "greeter_sync", "privilege_command"};
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Security,
              .group = "greeter",
              .title = i18n::tr("settings.schema.shell.sync-greeter.label"),
              .subtitle = i18n::tr("settings.schema.shell.sync-greeter.description"),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.shell.sync-greeter.button"),
                      .action = actions.syncGreeterAppearance,
                  },
              .searchText = "greeter login sync appearance wallpaper colors security",
          }
      );
    }

    if (actions.resetLauncherUsage) {
      auto it = insertAfter(registry, [](const SettingEntry& e) {
        return e.section == SettingsSection::Launcher
            && e.group == "launcher"
            && e.path == std::vector<std::string>{"shell", "launcher", "sort_by_usage"};
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Launcher,
              .group = "launcher",
              .title = i18n::tr("settings.schema.panels.launcher-reset-usage.label"),
              .subtitle = i18n::tr("settings.schema.panels.launcher-reset-usage.description"),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.panels.launcher-reset-usage.button"),
                      .action = actions.resetLauncherUsage,
                      .glyph = "refresh",
                  },
              .searchText = "launcher reset usage recently used launch count history clear",
          }
      );
    }

    if (actions.resetScreenTime) {
      auto it = insertAfter(registry, [](const SettingEntry& e) {
        return e.section == SettingsSection::System
            && e.group == "screen-time"
            && e.path == std::vector<std::string>{"shell", "screen_time_enabled"};
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::System,
              .group = "screen-time",
              .title = i18n::tr("settings.schema.shell.screen-time-reset.label"),
              .subtitle = i18n::tr("settings.schema.shell.screen-time-reset.description"),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.shell.screen-time-reset.button"),
                      .action = actions.resetScreenTime,
                      .glyph = "refresh",
                  },
              .searchText = "screen time reset usage history clear tracking",
              .visibleWhen = [](const Config& c) { return c.shell.screenTimeEnabled; },
          }
      );
    }

    if (actions.saveWallpaperPaletteAsCustom && cfg.theme.source == PaletteSource::Wallpaper) {
      auto it = insertAfter(registry, [](const SettingEntry& e) {
        return e.section == SettingsSection::Theme
            && e.group == "theme"
            && e.path == std::vector<std::string>{"theme", "wallpaper_scheme"};
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Theme,
              .group = "theme",
              .title = i18n::tr("settings.schema.appearance.export-wallpaper-palette.label"),
              .subtitle = i18n::tr("settings.schema.appearance.export-wallpaper-palette.description"),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.appearance.export-wallpaper-palette.button"),
                      .action = actions.saveWallpaperPaletteAsCustom,
                  },
              .searchText = "wallpaper palette export custom save colors theme",
          }
      );
    }

    if (actions.openDesktopWidgetEditor) {
      auto it = insertAfter(registry, [](const SettingEntry& e) {
        return e.section == SettingsSection::Desktop && e.group == "widgets";
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Desktop,
              .group = "widgets",
              .title = i18n::tr("settings.schema.desktop.widgets-editor.label"),
              .subtitle = i18n::tr("settings.schema.desktop.widgets-editor.description"),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.desktop.widgets-editor.button"),
                      .action = actions.openDesktopWidgetEditor,
                  },
              .searchText = "desktop widgets editor edit",
          }
      );
    }

    if (actions.openLockscreenWidgetEditor) {
      auto it = insertAfter(registry, [](const SettingEntry& e) {
        return e.section == SettingsSection::Security
            && e.group == "lock-screen"
            && e.path == std::vector<std::string>{"lockscreen_widgets", "enabled"};
      });
      registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Security,
              .group = "lock-screen",
              .title = i18n::tr("settings.schema.lockscreen.widgets-editor.label"),
              .subtitle = i18n::tr("settings.schema.lockscreen.widgets-editor.description"),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.lockscreen.widgets-editor.button"),
                      .action = actions.openLockscreenWidgetEditor,
                  },
              .searchText = "lockscreen widgets editor edit layout",
              .visibleWhen = [](const Config& c) { return c.lockscreen.enabled && c.lockscreenWidgets.enabled; },
          }
      );
    }

    if (actions.openCalendarAccount) {
      auto it = insertAfter(registry, [](const SettingEntry& e) {
        return e.section == SettingsSection::Services
            && e.group == "calendar"
            && e.path == std::vector<std::string>{"calendar", "refresh_minutes"};
      });
      const SettingVisibility calendarOn = [](const Config& c) { return c.calendar.enabled; };
      it = registry.insert(
          it,
          SettingEntry{
              .section = SettingsSection::Services,
              .group = "calendar",
              .title = i18n::tr("settings.schema.services.calendar-add.label"),
              .subtitle = i18n::tr("settings.schema.services.calendar-add.description"),
              .path = {},
              .control =
                  ButtonSetting{
                      .label = i18n::tr("settings.schema.services.calendar-add.button"),
                      .action = [open = actions.openCalendarAccount]() { open(std::nullopt); },
                      .glyph = "plus",
                  },
              .searchText = "calendar add account icloud caldav google ics ical subscription vdir vdirsyncer local",
          }
      );
      ++it;

      for (const CalendarConfig::Account& account : cfg.calendar.accounts) {
        if (account.type != "google" && account.type != "caldav" && account.type != "ics" && account.type != "vdir") {
          continue;
        }
        const bool credentialLocked = account.type == "google"
            && actions.calendar != nullptr
            && actions.calendar->googleAccountCredentialLocked(account.id);
        const bool reconnectRequired = account.type == "google"
            && actions.calendar != nullptr
            && actions.calendar->googleAccountNeedsReconnect(account.id);
        const std::string_view descriptionKey = credentialLocked
            ? "settings.schema.services.calendar-edit.description-locked"
            : reconnectRequired ? "settings.schema.services.calendar-edit.description-reconnect"
                               : "settings.schema.services.calendar-edit.description";
        const std::string_view buttonKey = credentialLocked ? "settings.schema.services.calendar-edit.button-retry"
            : reconnectRequired                          ? "settings.schema.services.calendar-edit.button-reconnect"
                                                          : "settings.schema.services.calendar-edit.button";
        it = registry.insert(
            it,
            SettingEntry{
                .section = SettingsSection::Services,
                .group = "calendar",
                .title = account.displayName.empty() ? account.id : account.displayName,
                .subtitle = i18n::tr(descriptionKey),
                .path = {},
                .control =
                    ButtonSetting{
                        .label = i18n::tr(buttonKey),
                        .action =
                            [open = actions.openCalendarAccount, calendar = actions.calendar, id = account.id,
                             credentialLocked]() {
                              if (credentialLocked && calendar != nullptr) {
                                calendar->requestRefresh();
                                return;
                              }
                              if (open) {
                                open(id);
                              }
                            },
                        .glyph = credentialLocked ? "key" : reconnectRequired ? "brand-google" : "edit",
                        .variant = credentialLocked ? ButtonVariant::Secondary : ButtonVariant::Default,
                    },
                .searchText =
                    "calendar account edit connect authorize caldav icloud google password ics ical subscription vdir "
                    "local "
                    + account.id,
                .visibleWhen = calendarOn,
            }
        );
        ++it;
      }
    }
  }

} // namespace settings
