#include "desktop_api/peer_identity.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace helix {

  std::string clientIdentity(const std::string& exePath, const std::vector<std::string>& cmdline) {
    const std::string base = std::filesystem::path(exePath).filename().string();
    static constexpr std::array<std::string_view, 7> kInterpreters{"python", "node", "bun", "deno",
                                                                   "ruby",   "perl", "bash"};
    const bool interpreter =
        std::ranges::any_of(kInterpreters, [&](std::string_view name) { return base.starts_with(name); })
        || base == "sh";
    if (interpreter) {
      // First argument that is not an option is the script (or module name for `-m`).
      for (std::size_t i = 1; i < cmdline.size(); ++i) {
        if (cmdline[i].empty() || cmdline[i].front() == '-')
          continue;
        std::error_code ec;
        const auto script = std::filesystem::weakly_canonical(cmdline[i], ec);
        return "exe:" + exePath + "#" + (ec ? cmdline[i] : script.string());
      }
    }
    return "exe:" + exePath;
  }

  std::expected<PeerIdentity, PeerFailure>
  resolvePeer(GDBusConnection* bus, const std::string& sender, PeerDetail detail) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionCredentials",
        g_variant_new("(s)", sender.c_str()), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, &error
    );
    if (!reply) {
      const std::string message = error ? error->message : "unknown error";
      if (error)
        g_error_free(error);
      return std::unexpected(PeerFailure{false, "could not identify the caller: " + message});
    }
    GVariant* dict = g_variant_get_child_value(reply, 0);
    guint32 pid = 0, uid = 0;
    const bool havePid = g_variant_lookup(dict, "ProcessID", "u", &pid);
    const bool haveUid = g_variant_lookup(dict, "UnixUserID", "u", &uid);
    g_variant_unref(dict);
    g_variant_unref(reply);
    if (!havePid || !haveUid)
      return std::unexpected(PeerFailure{false, "the bus did not report the caller's process"});
    if (uid != geteuid())
      return std::unexpected(PeerFailure{true, "callers must run as the same user"});
    if (detail == PeerDetail::ProcessOnly)
      return PeerIdentity{"pid:" + std::to_string(pid), {}};

    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
    if (ec)
      return std::unexpected(PeerFailure{false, "could not read the caller's executable"});
    std::vector<std::string> cmdline;
    if (std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary); in) {
      for (std::string part; std::getline(in, part, '\0');)
        cmdline.push_back(part);
    }
    return PeerIdentity{clientIdentity(exe.string(), cmdline), exe.string()};
  }

} // namespace helix
