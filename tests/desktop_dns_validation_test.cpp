#include "desktop/dns_update.h"

#include <cassert>
using helix::Json;
bool rejects(const Json& value) {
  try {
    (void)helix::validatedDnsUpdate(value);
    return false;
  } catch (const std::invalid_argument&) {
    return true;
  }
}
int main() {
  for (int profile = 0; profile < 4; ++profile) {
    auto value = helix::validatedDnsUpdate({{"profile", profile}, {"servers", Json::array()}, {"nextdns", "abc_123"}});
    assert(int(value.profile) == profile);
  }
  auto custom =
      helix::validatedDnsUpdate({{"profile", 4}, {"servers", {"1.1.1.1", "2001:4860:4860::8888#dns.google"}}});
  assert(custom.servers.size() == 2);
  for (const auto& value : std::vector<Json>{
           nullptr,
           Json::array(),
           {{"profile", -1}},
           {{"profile", 5}},
           {{"profile", true}},
           {{"profile", 4.0}},
           {{"profile", "custom"}},
           {{"profile", 4}, {"servers", Json::array()}},
           {{"profile", 4}, {"servers", "1.1.1.1"}},
           {{"profile", 4}, {"servers", {true}}},
           {{"profile", 4}, {"servers", {"not-an-address"}}},
           {{"profile", 3}, {"nextdns", "bad/id"}},
           {{"profile", 3}, {"nextdns", 12}}
       })
    assert(rejects(value));
}
