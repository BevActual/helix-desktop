#include "dbus/cups/printer_service.h"

#include "cli/schema_msg.h"
#include "core/log.h"
#include "core/process/process.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"

#include <cups/cups.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("printers");

  const sdbus::ServiceName kPkHelperName{"org.opensuse.CupsPkHelper.Mechanism"};
  const sdbus::ObjectPath kPkHelperPath{"/org/opensuse/CupsPkHelper/Mechanism"};
  const sdbus::ServiceName kSystemdName{"org.freedesktop.systemd1"};
  const sdbus::ObjectPath kSystemdPath{"/org/freedesktop/systemd1"};

  constexpr auto kPkHelperInterface = "org.opensuse.CupsPkHelper.Mechanism";
  constexpr auto kSystemdManager = "org.freedesktop.systemd1.Manager";

  [[nodiscard]] bool cupsInstalled() {
    return process::commandExists("lpstat") || process::commandExists("cupsd") || access("/run/cups/cups.sock", F_OK) == 0;
  }

  [[nodiscard]] bool cupsReachable() {
    http_t* http = httpConnect2(
        cupsServer(), ippPort(), nullptr, AF_UNSPEC, cupsEncryption(), 1, 400, nullptr
    );
    if (http == nullptr) {
      return false;
    }
    httpClose(http);
    return true;
  }

  [[nodiscard]] std::string printerStateLabel(int state, bool enabled) {
    if (!enabled || state == IPP_PSTATE_STOPPED) {
      return "paused";
    }
    if (state == IPP_PSTATE_PROCESSING) {
      return "printing";
    }
    return "idle";
  }

} // namespace

struct PrinterService::Impl {
  PrinterService& self;
  SystemBus* bus = nullptr;
  std::shared_ptr<void> lifetime = std::make_shared<int>(0);
  std::unique_ptr<sdbus::IProxy> inflight;

  explicit Impl(PrinterService& s, SystemBus* b) : self(s), bus(b) {}

  [[nodiscard]] std::weak_ptr<void> token() const { return lifetime; }
};

PrinterService::PrinterService(SystemBus* bus) : m_bus(bus), m_impl(std::make_unique<Impl>(*this, bus)) {}

PrinterService::~PrinterService() {
  m_pollTimer.stop();
  m_impl->lifetime.reset();
}

void PrinterService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void PrinterService::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void PrinterService::schedulePoll() {
  m_pollTimer.stop();
  const auto seconds = m_fastPoll && m_snapshot.running ? 3 : 12;
  m_pollTimer.start(std::chrono::seconds(seconds), [this]() { refresh(); });
}

void PrinterService::start() {
  refresh();
  schedulePoll();
}

void PrinterService::setWantFastPoll(bool enabled) {
  if (m_fastPoll == enabled) {
    return;
  }
  m_fastPoll = enabled;
  schedulePoll();
  if (enabled) {
    refresh();
  }
}

void PrinterService::finishRefresh(PrinterSnapshot next) {
  next.busy = m_snapshot.busy;
  if (next == m_snapshot) {
    return;
  }
  m_snapshot = std::move(next);
  notifyChanged();
  schedulePoll();
}

void PrinterService::refresh() {
  PrinterSnapshot next;
  next.installed = cupsInstalled();
  next.running = next.installed && cupsReachable();
  if (!next.running) {
    finishRefresh(std::move(next));
    return;
  }

  cups_dest_t* dests = nullptr;
  const int count = cupsGetDests(&dests);
  if (count < 0) {
    next.error = i18n::tr("control-center.printers.status-failed");
    finishRefresh(std::move(next));
    return;
  }

  next.printers.reserve(static_cast<std::size_t>(std::max(count, 0)));
  for (int i = 0; i < count; ++i) {
    const cups_dest_t& dest = dests[i];
    if (dest.name == nullptr || dest.name[0] == '\0') {
      continue;
    }
    PrinterInfo printer;
    printer.name = dest.name;
    const char* info = cupsGetOption("printer-info", dest.num_options, dest.options);
    printer.info = info != nullptr ? info : "";
    const char* location = cupsGetOption("printer-location", dest.num_options, dest.options);
    printer.location = location != nullptr ? location : "";
    printer.isDefault = dest.is_default != 0;
    const char* accepting = cupsGetOption("printer-is-accepting-jobs", dest.num_options, dest.options);
    const int state = cupsGetIntegerOption("printer-state", dest.num_options, dest.options);
    printer.enabled = accepting == nullptr || std::string_view(accepting) != "false";
    if (state == IPP_PSTATE_STOPPED) {
      printer.enabled = false;
    }
    printer.state = printerStateLabel(state, printer.enabled);

    cups_job_t* jobs = nullptr;
    const int jobCount = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, dest.name, 0, CUPS_WHICHJOBS_ACTIVE);
    printer.jobCount = std::max(jobCount, 0);
    if (jobCount > 0) {
      cupsFreeJobs(jobCount, jobs);
    }
    next.printers.push_back(std::move(printer));
  }
  cupsFreeDests(count, dests);
  finishRefresh(std::move(next));
}

void PrinterService::tryStart() {
  if (m_snapshot.running) {
    refresh();
    return;
  }
  if (m_bus == nullptr) {
    m_snapshot.error = i18n::tr("control-center.printers.write-failed");
    notifyChanged();
    return;
  }
  m_snapshot.busy = true;
  notifyChanged();
  const std::weak_ptr<void> lifetime = m_impl->token();
  try {
    m_impl->inflight = sdbus::createProxy(m_bus->connection(), kSystemdName, kSystemdPath);
    m_impl->inflight->callMethodAsync("StartUnit")
        .onInterface(kSystemdManager)
        .withArguments(std::string{"cups.service"}, std::string{"replace"})
        .uponReplyInvoke([this, lifetime](std::optional<sdbus::Error> err, sdbus::ObjectPath /*job*/) {
          if (lifetime.expired()) {
            return;
          }
          m_snapshot.busy = false;
          if (err.has_value()) {
            kLog.warn("start cups failed: {}", err->what());
            m_snapshot.error = i18n::tr("control-center.printers.write-failed");
            notifyChanged();
          }
          refresh();
        });
  } catch (const sdbus::Error& e) {
    m_snapshot.busy = false;
    kLog.warn("start cups dispatch failed: {}", e.what());
    m_snapshot.error = i18n::tr("control-center.printers.write-failed");
    notifyChanged();
  }
}

void PrinterService::setDefault(const std::string& name) {
  cups_dest_t* dests = nullptr;
  const int count = cupsGetDests(&dests);
  cups_dest_t* dest = cupsGetDest(name.c_str(), nullptr, count, dests);
  if (dest == nullptr) {
    cupsFreeDests(count, dests);
    m_snapshot.error = i18n::tr("control-center.printers.write-failed");
    notifyChanged();
    return;
  }
  for (int i = 0; i < count; ++i) {
    dests[i].is_default = 0;
  }
  dest->is_default = 1;
  if (cupsSetDests2(CUPS_HTTP_DEFAULT, count, dests) != 0) {
    m_snapshot.error = i18n::tr("control-center.printers.write-failed");
  }
  cupsFreeDests(count, dests);
  refresh();
}

void PrinterService::setEnabled(const std::string& name, bool enabled) {
  if (m_bus == nullptr) {
    m_snapshot.error = i18n::tr("control-center.printers.write-failed");
    notifyChanged();
    return;
  }
  const std::weak_ptr<void> lifetime = m_impl->token();
  try {
    auto proxy = sdbus::createProxy(m_bus->connection(), kPkHelperName, kPkHelperPath);
    m_impl->inflight = std::move(proxy);
    m_impl->inflight->callMethodAsync("PrinterSetEnabled")
        .onInterface(kPkHelperInterface)
        .withArguments(name, enabled)
        .uponReplyInvoke([this, lifetime](std::optional<sdbus::Error> err) {
          if (lifetime.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("PrinterSetEnabled failed: {}", err->what());
            m_snapshot.error = i18n::tr("control-center.printers.write-failed");
            notifyChanged();
          }
          refresh();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("PrinterSetEnabled dispatch failed: {}", e.what());
    m_snapshot.error = i18n::tr("control-center.printers.write-failed");
    notifyChanged();
  }
}

void PrinterService::cancelJobs(const std::string& name) {
  cups_job_t* jobs = nullptr;
  const int jobCount = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, name.c_str(), 1, CUPS_WHICHJOBS_ACTIVE);
  for (int i = 0; i < jobCount; ++i) {
    (void)cupsCancelJob2(CUPS_HTTP_DEFAULT, name.c_str(), jobs[i].id, 0);
  }
  if (jobCount > 0) {
    cupsFreeJobs(jobCount, jobs);
  }
  if (jobCount <= 0 && m_bus != nullptr) {
    const std::weak_ptr<void> lifetime = m_impl->token();
    try {
      auto proxy = sdbus::createProxy(m_bus->connection(), kPkHelperName, kPkHelperPath);
      m_impl->inflight = std::move(proxy);
      m_impl->inflight->callMethodAsync("JobCancelAll")
          .onInterface(kPkHelperInterface)
          .withArguments(name)
          .uponReplyInvoke([this, lifetime](std::optional<sdbus::Error> err) {
            if (lifetime.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.warn("JobCancelAll failed: {}", err->what());
              m_snapshot.error = i18n::tr("control-center.printers.write-failed");
              notifyChanged();
            }
            refresh();
          });
      return;
    } catch (const sdbus::Error& e) {
      kLog.warn("JobCancelAll dispatch failed: {}", e.what());
    }
  }
  refresh();
}

void PrinterService::printFile(const std::string& name, const std::string& path) {
  const int job = cupsPrintFile(name.c_str(), path.c_str(), path.c_str(), 0, nullptr);
  if (job <= 0) {
    kLog.warn("cupsPrintFile failed: {}", cupsLastErrorString());
    m_snapshot.error = i18n::tr("control-center.printers.print-failed");
    notifyChanged();
    return;
  }
  refresh();
}

void PrinterService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::printersStatus, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: printers-status takes no arguments\n";
    }
    if (!m_snapshot.installed) {
      return "not-installed\n";
    }
    if (!m_snapshot.running) {
      return m_snapshot.busy ? "starting\n" : "stopped\n";
    }
    std::string def = "-";
    for (const auto& printer : m_snapshot.printers) {
      if (printer.isDefault) {
        def = printer.name;
        break;
      }
    }
    return std::format("running printers={} default={}\n", m_snapshot.printers.size(), def);
  });
  ipc.bind(noctalia::cli::msg::printersStart, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: printers-start takes no arguments\n";
    }
    if (m_snapshot.running) {
      return "ok\n";
    }
    tryStart();
    return "ok\n";
  });
}
