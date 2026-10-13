#pragma once

#include "util/string_utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <cstdint>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <vector>

enum class DnsProfile : std::uint8_t {
  Automatic = 0,
  Google = 1,
  Cloudflare = 2,
  NextDns = 3,
  Custom = 4,
};

struct DnsOverride {
  DnsProfile profile = DnsProfile::Automatic;
  std::vector<std::string> servers;
  std::string nextDnsId;
};

struct ParsedDnsServer {
  std::string host;
  std::string sni;
  bool ipv6 = false;
};

namespace dns_override {

  inline constexpr std::array<std::string_view, 2> kGoogleV4{"8.8.8.8", "8.8.4.4"};
  inline constexpr std::array<std::string_view, 2> kGoogleV6{"2001:4860:4860::8888", "2001:4860:4860::8844"};
  inline constexpr std::array<std::string_view, 2> kCloudflareV4{"1.1.1.1", "1.0.0.1"};
  inline constexpr std::array<std::string_view, 2> kCloudflareV6{"2606:4700:4700::1111", "2606:4700:4700::1001"};
  inline constexpr std::array<std::string_view, 2> kNextDnsV4{"45.90.28.0", "45.90.30.0"};
  inline constexpr std::array<std::string_view, 2> kNextDnsV6{"2a07:a8c0::", "2a07:a8c1::"};
  inline constexpr std::string_view kNextDnsSniSuffix = ".dns.nextdns.io";

  [[nodiscard]] inline std::string_view hostPart(std::string_view server) {
    const auto hash = server.find('#');
    return hash == std::string_view::npos ? server : server.substr(0, hash);
  }

  [[nodiscard]] inline std::string_view sniPart(std::string_view server) {
    const auto hash = server.find('#');
    return hash == std::string_view::npos ? std::string_view{} : server.substr(hash + 1);
  }

  [[nodiscard]] inline std::optional<ParsedDnsServer> parseServer(std::string_view token) {
    const std::string trimmed = StringUtils::trim(token);
    if (trimmed.empty()) {
      return std::nullopt;
    }
    const std::string_view host = hostPart(trimmed);
    if (host.empty()) {
      return std::nullopt;
    }
    const std::string hostStr(host);
    in_addr v4{};
    in6_addr v6{};
    ParsedDnsServer parsed;
    parsed.sni = std::string(sniPart(trimmed));
    if (inet_pton(AF_INET, hostStr.c_str(), &v4) == 1) {
      parsed.host = hostStr;
      parsed.ipv6 = false;
      return parsed;
    }
    if (inet_pton(AF_INET6, hostStr.c_str(), &v6) == 1) {
      parsed.host = hostStr;
      parsed.ipv6 = true;
      return parsed;
    }
    return std::nullopt;
  }

  [[nodiscard]] inline std::vector<std::string> splitServerList(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
      const std::string trimmed = StringUtils::trim(current);
      if (!trimmed.empty()) {
        tokens.push_back(trimmed);
      }
      current.clear();
    };
    for (const char c : text) {
      if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n') {
        flush();
      } else {
        current.push_back(c);
      }
    }
    flush();
    return tokens;
  }

  [[nodiscard]] inline bool parseServerList(std::string_view text, std::vector<ParsedDnsServer>& out) {
    out.clear();
    const auto tokens = splitServerList(text);
    if (tokens.empty()) {
      return false;
    }
    out.reserve(tokens.size());
    for (const auto& token : tokens) {
      auto parsed = parseServer(token);
      if (!parsed.has_value()) {
        out.clear();
        return false;
      }
      out.push_back(std::move(*parsed));
    }
    return !out.empty();
  }

  [[nodiscard]] inline std::string formatServer(const ParsedDnsServer& server) {
    if (server.sni.empty()) {
      return server.host;
    }
    return server.host + "#" + server.sni;
  }

  [[nodiscard]] inline std::string nextDnsSni(std::string_view id) {
    const std::string trimmed = StringUtils::trim(id);
    if (trimmed.empty()) {
      return {};
    }
    return trimmed + std::string(kNextDnsSniSuffix);
  }

  [[nodiscard]] inline std::string extractNextDnsId(std::string_view sni) {
    const std::string lower = StringUtils::toLower(sni);
    if (!lower.ends_with(kNextDnsSniSuffix)) {
      return {};
    }
    return std::string(sni.substr(0, sni.size() - kNextDnsSniSuffix.size()));
  }

  template <std::size_t N>
  [[nodiscard]] inline std::vector<std::string>
  withOptionalSni(const std::array<std::string_view, N>& hosts, std::string_view sni) {
    std::vector<std::string> out;
    out.reserve(N);
    for (const auto host : hosts) {
      if (sni.empty()) {
        out.emplace_back(host);
      } else {
        out.emplace_back(std::string(host) + "#" + std::string(sni));
      }
    }
    return out;
  }

  [[nodiscard]] inline std::vector<std::string>
  joinFamilies(const std::vector<std::string>& v4, const std::vector<std::string>& v6) {
    std::vector<std::string> out = v4;
    out.insert(out.end(), v6.begin(), v6.end());
    return out;
  }

  [[nodiscard]] inline std::vector<std::string> serversFor(DnsProfile profile, std::string_view nextDnsId) {
    switch (profile) {
    case DnsProfile::Google:
      return joinFamilies(withOptionalSni(kGoogleV4, {}), withOptionalSni(kGoogleV6, {}));
    case DnsProfile::Cloudflare:
      return joinFamilies(withOptionalSni(kCloudflareV4, {}), withOptionalSni(kCloudflareV6, {}));
    case DnsProfile::NextDns:
      return joinFamilies(
          withOptionalSni(kNextDnsV4, nextDnsSni(nextDnsId)), withOptionalSni(kNextDnsV6, nextDnsSni(nextDnsId))
      );
    case DnsProfile::Automatic:
    case DnsProfile::Custom:
      break;
    }
    return {};
  }

  [[nodiscard]] inline std::string normalizedHost(std::string_view server) {
    const auto parsed = parseServer(server);
    return parsed.has_value() ? StringUtils::toLower(parsed->host) : StringUtils::toLower(hostPart(server));
  }

  template <std::size_t Nv4, std::size_t Nv6>
  [[nodiscard]] inline bool hostsMatchPreset(
      const std::vector<std::string>& servers, const std::array<std::string_view, Nv4>& v4,
      const std::array<std::string_view, Nv6>& v6
  ) {
    if (servers.empty()) {
      return false;
    }
    for (const auto& server : servers) {
      const std::string host = normalizedHost(server);
      const auto matches = [&](std::string_view candidate) { return host == StringUtils::toLower(candidate); };
      if (std::ranges::any_of(v4, matches) || std::ranges::any_of(v6, matches)) {
        continue;
      }
      return false;
    }
    return true;
  }

  [[nodiscard]] inline DnsOverride classify(bool ignoreAutoDns, const std::vector<std::string>& servers) {
    DnsOverride out;
    out.servers = servers;
    if (!ignoreAutoDns || servers.empty()) {
      out.profile = DnsProfile::Automatic;
      return out;
    }
    std::string nextId;
    for (const auto& server : servers) {
      const auto parsed = parseServer(server);
      if (!parsed.has_value()) {
        continue;
      }
      const std::string id = extractNextDnsId(parsed->sni);
      if (!id.empty()) {
        nextId = id;
        break;
      }
    }
    if (!nextId.empty() || hostsMatchPreset(servers, kNextDnsV4, kNextDnsV6)) {
      out.profile = DnsProfile::NextDns;
      out.nextDnsId = std::move(nextId);
      return out;
    }
    if (hostsMatchPreset(servers, kGoogleV4, kGoogleV6)) {
      out.profile = DnsProfile::Google;
      return out;
    }
    if (hostsMatchPreset(servers, kCloudflareV4, kCloudflareV6)) {
      out.profile = DnsProfile::Cloudflare;
      return out;
    }
    out.profile = DnsProfile::Custom;
    return out;
  }

  [[nodiscard]] inline bool validNextDnsId(std::string_view id) {
    const std::string trimmed = StringUtils::trim(id);
    if (trimmed.empty() || trimmed.size() > 64) {
      return trimmed.empty();
    }
    return std::ranges::all_of(trimmed, [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_'; });
  }

} // namespace dns_override
