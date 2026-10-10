#include "config/widgets/widget_settings_registry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace settings {
  namespace schema = helix::config::schema;

  namespace {

    constexpr std::array kBuiltInTypes{
        std::string_view{"active_window"},
        std::string_view{"audio_visualizer"},
        std::string_view{"battery"},
        std::string_view{"bluetooth"},
        std::string_view{"brightness"},
        std::string_view{"caffeine"},
        std::string_view{"clipboard"},
        std::string_view{"clock"},
        std::string_view{"control-center"},
        std::string_view{"custom_button"},
        std::string_view{"keyboard_layout"},
        std::string_view{"launcher"},
        std::string_view{"lock_keys"},
        std::string_view{"media"},
        std::string_view{"network"},
        std::string_view{"nightlight"},
        std::string_view{"notifications"},
        std::string_view{"power_profile"},
        std::string_view{"privacy"},
        std::string_view{"screenshot"},
        std::string_view{"session"},
        std::string_view{"settings"},
        std::string_view{"spacer"},
        std::string_view{"sysmon"},
        std::string_view{"taskbar"},
        std::string_view{"test"},
        std::string_view{"text"},
        std::string_view{"theme_mode"},
        std::string_view{"tray"},
        std::string_view{"volume"},
        std::string_view{"wallpaper"},
        std::string_view{"workspaces"},
    };

    const std::vector<WidgetTypeSpec> kWidgetTypeSpecs = [] {
      std::vector<WidgetTypeSpec> out;
      out.reserve(kBuiltInTypes.size());
      for (const auto type : kBuiltInTypes) {
        out.push_back(WidgetTypeSpec{.type = type, .labelKey = type, .glyph = {}});
      }
      return out;
    }();

  } // namespace

  schema::WidgetSettingType schemaTypeForControl(WidgetControlKind control) {
    switch (control) {
    case WidgetControlKind::Bool:
      return schema::WidgetSettingType::Bool;
    case WidgetControlKind::Int:
      return schema::WidgetSettingType::Int;
    case WidgetControlKind::Double:
      return schema::WidgetSettingType::Double;
    case WidgetControlKind::OptionalDouble:
      return schema::WidgetSettingType::OptionalDouble;
    case WidgetControlKind::String:
    case WidgetControlKind::File:
    case WidgetControlKind::Folder:
    case WidgetControlKind::Glyph:
      return schema::WidgetSettingType::String;
    case WidgetControlKind::StringList:
      return schema::WidgetSettingType::StringList;
    case WidgetControlKind::StringMap:
      return schema::WidgetSettingType::StringMap;
    case WidgetControlKind::Select:
      return schema::WidgetSettingType::Enum;
    case WidgetControlKind::ColorSpec:
      return schema::WidgetSettingType::Color;
    }
    return schema::WidgetSettingType::String;
  }

  const std::vector<WidgetTypeSpec>& widgetTypeSpecs() { return kWidgetTypeSpecs; }

  bool isBuiltInWidgetType(std::string_view type) {
    return std::ranges::find(kBuiltInTypes, type) != kBuiltInTypes.end();
  }

  bool widgetTypeRequiresNamedConfig(std::string_view type) {
    return type == "custom_button" || type == "spacer" || type == "text";
  }

  std::string widgetTypeForReference(const Config& cfg, std::string_view name) {
    if (const auto it = cfg.widgets.find(std::string(name)); it != cfg.widgets.end() && !it->second.type.empty()) {
      return it->second.type;
    }
    if (isBuiltInWidgetType(name)) {
      return std::string(name);
    }
    return {};
  }

  std::string titleFromWidgetKey(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    bool upperNext = true;
    for (const char c : key) {
      if (c == '_' || c == '-') {
        out.push_back(' ');
        upperNext = true;
      } else if (upperNext) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        upperNext = false;
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  WidgetReferenceInfo
  widgetReferenceInfo(const Config& cfg, std::string_view name, bool /*includeManifestVersion*/) {
    WidgetReferenceInfo info;
    info.title = titleFromWidgetKey(name);
    const std::string type = widgetTypeForReference(cfg, name);
    if (isBuiltInWidgetType(type.empty() ? name : std::string_view(type))) {
      info.kind = WidgetReferenceKind::BuiltIn;
      info.detail = type.empty() ? std::string(name) : type;
    } else if (cfg.widgets.contains(std::string(name))) {
      info.kind = WidgetReferenceKind::Named;
      info.detail = type;
    } else {
      info.kind = WidgetReferenceKind::Unknown;
    }
    return info;
  }

  std::vector<WidgetPickerEntry> widgetPickerEntries(const Config& cfg) {
    std::vector<WidgetPickerEntry> entries;
    for (const auto& spec : kWidgetTypeSpecs) {
      entries.push_back(WidgetPickerEntry{
          .value = std::string(spec.type),
          .label = std::string(spec.type),
          .description = {},
          .icon = std::string(spec.glyph),
          .kind = WidgetReferenceKind::BuiltIn,
      });
    }
    for (const auto& [name, widget] : cfg.widgets) {
      if (isBuiltInWidgetType(name)) {
        continue;
      }
      entries.push_back(WidgetPickerEntry{
          .value = name,
          .label = titleFromWidgetKey(name),
          .description = widget.type,
          .icon = {},
          .kind = WidgetReferenceKind::Named,
      });
    }
    return entries;
  }

  std::vector<WidgetSettingSpec>
  commonWidgetSettingSpecs(std::string_view /*shellFontFamily*/, bool /*populateFontCatalogs*/) {
    return {};
  }

  std::vector<WidgetSettingSpec> widgetSettingSpecs(
      std::string_view /*type*/, const WidgetConfig* /*config*/, std::string_view /*shellFontFamily*/,
      bool /*populateFontCatalogs*/
  ) {
    return {};
  }

  bool widgetSettingIsVisible(
      const Config& /*config*/, std::string_view /*widgetName*/, const WidgetSettingSpec& /*spec*/,
      const std::vector<WidgetSettingSpec>& /*allSpecs*/, const WidgetSettingCapabilities& /*capabilities*/
  ) {
    return true;
  }

  helix::config::schema::WidgetSettingSchema widgetSettingSchema(std::string_view /*type*/) { return {}; }

  helix::config::schema::WidgetSettingSchema
  widgetSettingSchema(std::string_view /*type*/, const WidgetConfig* /*config*/) {
    return {};
  }

  std::optional<std::string> validateWidgetSemantics(std::string_view /*type*/, const WidgetConfig* /*config*/) {
    return std::nullopt;
  }

  std::optional<helix::config::schema::WidgetSettingField>
  findWidgetSettingField(std::string_view /*widgetType*/, std::string_view /*settingKey*/) {
    return std::nullopt;
  }

  std::optional<WidgetSettingSpec>
  findWidgetSettingSpec(std::string_view /*widgetType*/, std::string_view /*settingKey*/) {
    return std::nullopt;
  }

  std::optional<WidgetSettingSpec> findWidgetSettingSpec(
      std::string_view /*widgetType*/, std::string_view /*settingKey*/, const WidgetConfig* /*config*/
  ) {
    return std::nullopt;
  }

  bool configOverrideValueMatchesWidgetSetting(
      const ConfigOverrideValue& /*overrideValue*/, const WidgetSettingValue& /*settingValue*/
  ) {
    return false;
  }

  bool widgetOverrideValueMatchesRegistryDefault(
      std::string_view /*widgetType*/, std::string_view /*settingKey*/, const ConfigOverrideValue& /*overrideValue*/
  ) {
    return false;
  }

  bool widgetSettingOverrideIsEffective(
      std::string_view /*widgetName*/, std::string_view /*settingKey*/, const Config& /*withOverride*/,
      const Config& /*withoutOverride*/
  ) {
    return true;
  }

} // namespace settings
