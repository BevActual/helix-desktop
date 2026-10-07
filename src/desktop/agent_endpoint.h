#pragma once

#include "desktop/agent_broker.h"
#include "desktop_api/protocol.h"

#include <gio/gio.h>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace helix::agent {

// GDBus face of org.helix.Desktop.Agent1. Policy and confirmation logic live in AgentBroker; this class resolves
// who is calling, enforces the confirmer and manager roles, and turns broker completions into signals.
// If the risk policy cannot be loaded every method fails closed with the reason.
class AgentEndpoint {
public:
  AgentEndpoint(GDBusConnection* bus, std::string objectPath, std::string interfaceName,
                std::function<std::string()> desktopOwner);

  void method(const std::string& sender, const std::string& name, GVariant* parameters, GDBusMethodInvocation* invocation);
  // org.helix.Desktop.AgentActivity1: only the registered desktop may call these.
  void activity(const std::string& sender, const std::string& name, GVariant* parameters, GDBusMethodInvocation* invocation);
  void ownerGone(const std::string& name);
  void tick();
  bool available() const { return broker_ != nullptr; }

private:
  struct Peer {
    std::string client;  // grant key, from the verified executable (see clientIdentity)
    std::string exe;
    std::string label;   // self-declared, display only
  };
  struct OperationOwner {
    std::string sender;
    std::string client;
  };

  const Peer& peer(const std::string& sender);
  void emit(const char* signal, GVariant* value, const std::string& destination);
  void emitActivity(const char* signal, GVariant* value);
  void complete(const std::string& sender, std::shared_ptr<std::uint64_t> operation, bool ok, std::string result);
  std::uint64_t track(const std::string& sender, const Peer& who, std::function<std::uint64_t(Done)> start);
  Json readAudit(std::uint64_t since, std::uint64_t limit) const;
  void requireDesktop(const std::string& sender) const;

  GDBusConnection* bus_;
  std::string objectPath_, interface_;
  std::function<std::string()> desktopOwner_;
  std::string unavailable_;
  std::unique_ptr<GrantStore> grants_;
  std::unique_ptr<AgentBroker> broker_;
  std::filesystem::path auditFile_;
  std::map<std::string, Peer> peers_;
  std::map<std::uint64_t, OperationOwner> operations_;
  std::string confirmer_, manager_;
};

}  // namespace helix::agent
