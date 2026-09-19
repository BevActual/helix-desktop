#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class SystemBus;
class IpcService;

struct PrinterInfo {
  std::string name;
  std::string info;
  std::string location;
  std::string state;
  bool isDefault = false;
  bool enabled = true;
  int jobCount = 0;

  bool operator==(const PrinterInfo&) const = default;
};

struct PrinterSnapshot {
  bool installed = false;
  bool running = false;
  bool busy = false;
  std::vector<PrinterInfo> printers;
  std::string error;

  bool operator==(const PrinterSnapshot&) const = default;
};

class PrinterService {
public:
  using ChangeCallback = std::function<void()>;

  explicit PrinterService(SystemBus* bus);
  ~PrinterService();

  PrinterService(const PrinterService&) = delete;
  PrinterService& operator=(const PrinterService&) = delete;

  [[nodiscard]] const PrinterSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool running() const noexcept { return m_snapshot.running; }

  void setChangeCallback(ChangeCallback callback);
  void start();
  void refresh();
  void tryStart();
  void setWantFastPoll(bool enabled);
  void setDefault(const std::string& name);
  void setEnabled(const std::string& name, bool enabled);
  void cancelJobs(const std::string& name);
  void printFile(const std::string& name, const std::string& path);

  void registerIpc(IpcService& ipc);

private:
  struct Impl;

  void notifyChanged();
  void schedulePoll();
  void finishRefresh(PrinterSnapshot next);

  SystemBus* m_bus = nullptr;
  PrinterSnapshot m_snapshot;
  ChangeCallback m_changeCallback;
  Timer m_pollTimer;
  bool m_fastPoll = false;
  // refresh() runs CUPS queries on a worker thread; these are only touched on the main thread.
  bool m_refreshInflight = false;
  bool m_refreshQueued = false;
  std::unique_ptr<Impl> m_impl;
};
