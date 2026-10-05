#include "desktop/protocol.h"
#include <stdexcept>

namespace helix {
GVariant* toVariant(const Json& value) {
  if (value.is_object()) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    for (auto it = value.begin(); it != value.end(); ++it)
      g_variant_builder_add(&builder, "{sv}", it.key().c_str(), toVariant(it.value()));
    return g_variant_builder_end(&builder);
  }
  if (value.is_array()) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("av"));
    for (const auto& child : value) g_variant_builder_add(&builder, "v", toVariant(child));
    return g_variant_builder_end(&builder);
  }
  if (value.is_boolean()) return g_variant_new_boolean(value.get<bool>());
  if (value.is_number_unsigned()) return g_variant_new_uint64(value.get<std::uint64_t>());
  if (value.is_number_integer()) return g_variant_new_int64(value.get<std::int64_t>());
  if (value.is_number_float()) return g_variant_new_double(value.get<double>());
  if (value.is_string()) return g_variant_new_string(value.get_ref<const std::string&>().c_str());
  throw std::invalid_argument("Null and unsupported values cannot cross the desktop interface");
}
Json fromVariant(GVariant* value) {
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
    auto* child = g_variant_get_variant(value);
    auto result = fromVariant(child);
    g_variant_unref(child);
    return result;
  }
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARDICT)) {
    Json result = Json::object();
    GVariantIter iter;
    g_variant_iter_init(&iter, value);
    const char* key;
    GVariant* child;
    while (g_variant_iter_next(&iter, "{&sv}", &key, &child)) {
      result[key] = fromVariant(child);
      g_variant_unref(child);
    }
    return result;
  }
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ||
      g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) return g_variant_get_string(value, nullptr);
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) return bool(g_variant_get_boolean(value));
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE)) return g_variant_get_double(value);
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64)) return g_variant_get_uint64(value);
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) return g_variant_get_int64(value);
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) return g_variant_get_uint32(value);
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) return g_variant_get_int32(value);
  if (g_variant_is_container(value)) {
    Json result = Json::array();
    for (gsize i = 0; i < g_variant_n_children(value); ++i) {
      auto* child = g_variant_get_child_value(value, i);
      result.push_back(fromVariant(child));
      g_variant_unref(child);
    }
    return result;
  }
  throw std::invalid_argument("Unsupported desktop variant type");
}
std::string interfaceFor(std::string_view domain) {
  if (domain == "configuration") return configurationInterface;
  if (domain == "appearance") return appearanceInterface;
  if (domain == "audio") return audioInterface;
  if (domain == "network") return networkInterface;
  if (domain == "bluetooth") return bluetoothInterface;
  throw std::invalid_argument("Unknown desktop domain");
}
}
