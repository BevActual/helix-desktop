#pragma once

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>

namespace helix::test {

  using nlohmann::json;

  // A validator for exactly the JSON Schema subset the generator emits.
  inline bool valid(const json& schema, const json& value) {
    if (schema.is_boolean())
      return schema.get<bool>();
    if (schema.contains("oneOf")) {
      int matches = 0;
      for (const auto& alternative : schema.at("oneOf"))
        matches += valid(alternative, value) ? 1 : 0;
      if (matches != 1)
        return false;
    }
    if (schema.contains("const") && value != schema.at("const"))
      return false;
    if (schema.contains("enum") && std::ranges::find(schema.at("enum"), value) == schema.at("enum").end())
      return false;
    if (schema.contains("type")) {
      const auto type = schema.at("type").get<std::string>();
      if ((type == "string" && !value.is_string())
          || (type == "array" && !value.is_array())
          || (type == "object" && !value.is_object()))
        return false;
    }
    if (value.is_string()
        && schema.contains("maxLength")
        && value.get<std::string>().size() > schema.at("maxLength").get<std::size_t>())
      return false;
    if (value.is_string()
        && schema.contains("minLength")
        && value.get<std::string>().size() < schema.at("minLength").get<std::size_t>())
      return false;
    if (value.is_object()) {
      const auto properties = schema.value("properties", json::object());
      for (const auto& [key, child] : value.items()) {
        if (properties.contains(key)) {
          if (!valid(properties.at(key), child))
            return false;
        } else if (schema.value("additionalProperties", true) == false)
          return false;
      }
      for (const auto& key : schema.value("required", json::array()))
        if (!value.contains(key.get<std::string>()))
          return false;
    }
    if (value.is_array()) {
      if (schema.contains("minItems") && value.size() < schema.at("minItems").get<std::size_t>())
        return false;
      if (schema.contains("maxItems") && value.size() > schema.at("maxItems").get<std::size_t>())
        return false;
      const auto prefix = schema.value("prefixItems", json::array());
      for (std::size_t i = 0; i < value.size(); ++i) {
        if (i < prefix.size()) {
          if (!valid(prefix[i], value[i]))
            return false;
        } else if (schema.contains("items") && !valid(schema.at("items"), value[i]))
          return false;
      }
    }
    return true;
  }

} // namespace helix::test
