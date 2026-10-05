#pragma once
#include "desktop/protocol.h"
#include "dbus/network/dns_override.h"
#include <stdexcept>
namespace helix {
inline DnsOverride validatedDnsUpdate(const Json& settings) {
  if (!settings.is_object() || !settings.contains("profile") || !settings.at("profile").is_number_integer())
    throw std::invalid_argument("Choose a DNS profile");
  const auto profile = settings.at("profile").get<std::int64_t>();
  if (profile < 0 || profile > 4) throw std::invalid_argument("Unknown DNS profile");
  DnsOverride value; value.profile = DnsProfile(profile);
  if (settings.contains("servers")) {
    if (!settings.at("servers").is_array()) throw std::invalid_argument("DNS servers must be a list of addresses");
    for (const auto& server : settings.at("servers")) {
      if (!server.is_string()) throw std::invalid_argument("DNS servers must be text addresses");
      value.servers.push_back(server.get<std::string>());
    }
  }
  if (settings.contains("nextdns")) {
    if (!settings.at("nextdns").is_string()) throw std::invalid_argument("NextDNS ID must be text");
    value.nextDnsId = settings.at("nextdns").get<std::string>();
  }
  if (value.profile == DnsProfile::Custom) {
    if (value.servers.empty()) throw std::invalid_argument("Enter at least one DNS server address");
    for (const auto& server : value.servers) if (!dns_override::parseServer(server))
      throw std::invalid_argument("Enter valid IPv4 or IPv6 DNS addresses");
  }
  if (value.profile == DnsProfile::NextDns && !dns_override::validNextDnsId(value.nextDnsId))
    throw std::invalid_argument("NextDNS ID may contain only letters, numbers, hyphens and underscores");
  return value;
}
}
