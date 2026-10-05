#include "wayland/wayland_workspaces.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_runtime.h"
#include "compositors/ext_workspace/ext_workspace_backend.h"
#include "compositors/output_backend.h"
#include "core/log.h"

#include <string>

namespace {

  constexpr Logger kLog("workspace");

} // namespace

WaylandWorkspaces::WaylandWorkspaces(compositors::CompositorRuntimeRegistry& /*runtimeRegistry*/) {
  auto extBackend = std::make_unique<ExtWorkspaceBackend>();
  m_extWorkspaceBinder = extBackend.get();
  m_extBackend = extBackend.get();
  m_backends.push_back(std::move(extBackend));
}

WaylandWorkspaces::~WaylandWorkspaces() = default;

void WaylandWorkspaces::bindExtWorkspace(ext_workspace_manager_v1* manager) {
  if (m_extWorkspaceBinder != nullptr) {
    m_extWorkspaceBinder->bindExtWorkspace(manager);
  }
}

void WaylandWorkspaces::setOutputNameResolver(std::function<std::string(wl_output*)> resolver) {
  for (auto* backend : m_outputNameResolvers) {
    if (backend != nullptr) {
      backend->setOutputNameResolver(resolver);
    }
  }
}

void WaylandWorkspaces::initialize() {
  if (m_extBackend != nullptr && m_extBackend->isAvailable()) {
    setActiveBackend(m_extBackend);
    return;
  }
  setActiveBackend(nullptr);
}

void WaylandWorkspaces::onOutputAdded(wl_output* output) {
  for (auto* backend : m_outputObservers) {
    if (backend != nullptr) {
      backend->onOutputAdded(output);
    }
  }
}

void WaylandWorkspaces::onOutputRemoved(wl_output* output) {
  for (auto* backend : m_outputObservers) {
    if (backend != nullptr) {
      backend->onOutputRemoved(output);
    }
  }
}

void WaylandWorkspaces::setChangeCallback(ChangeCallback callback) {
  m_changeCallback = std::move(callback);
  for (const auto& backend : m_backends) {
    if (backend != nullptr) {
      backend->setChangeCallback([this]() { notifyChanged(); });
    }
  }
}

void WaylandWorkspaces::activate(const std::string& id) {
  if (m_activeBackend != nullptr) {
    m_activeBackend->activate(id);
  }
}

void WaylandWorkspaces::activateForOutput(wl_output* output, const std::string& id) {
  if (m_activeBackend != nullptr) {
    m_activeBackend->activateForOutput(output, id);
  }
}

void WaylandWorkspaces::activateForOutput(wl_output* output, const Workspace& workspace) {
  if (m_activeBackend != nullptr) {
    m_activeBackend->activateForOutput(output, workspace);
  }
}

void WaylandWorkspaces::cleanup() {
  for (const auto& backend : m_backends) {
    if (backend != nullptr) {
      backend->cleanup();
    }
  }
  m_activeBackend = nullptr;
}

int WaylandWorkspaces::pollFd() const noexcept { return m_activeBackend != nullptr ? m_activeBackend->pollFd() : -1; }

short WaylandWorkspaces::pollEvents() const noexcept {
  return m_activeBackend != nullptr ? m_activeBackend->pollEvents() : static_cast<short>(POLLIN);
}

int WaylandWorkspaces::pollTimeoutMs() const noexcept {
  return m_activeBackend != nullptr ? m_activeBackend->pollTimeoutMs() : -1;
}

void WaylandWorkspaces::dispatchPoll(short revents) {
  if (m_activeBackend != nullptr) {
    m_activeBackend->dispatchPoll(revents);
  }
}

const char* WaylandWorkspaces::backendName() const noexcept {
  return m_activeBackend != nullptr ? m_activeBackend->backendName() : "none";
}

std::unordered_map<std::string, std::vector<std::string>>
WaylandWorkspaces::appIdsByWorkspace(wl_output* output) const {
  return m_activeBackend != nullptr ? m_activeBackend->appIdsByWorkspace(output)
                                    : std::unordered_map<std::string, std::vector<std::string>>{};
}

TaskbarAssignmentMode WaylandWorkspaces::taskbarAssignmentMode() const noexcept {
  return m_activeBackend != nullptr ? m_activeBackend->taskbarAssignmentMode() : TaskbarAssignmentMode::Generic;
}

std::unordered_map<std::uintptr_t, WorkspaceWindow>
WaylandWorkspaces::assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* output) const {
  return m_activeBackend != nullptr ? m_activeBackend->assignTaskbarWindows(windows, output)
                                    : std::unordered_map<std::uintptr_t, WorkspaceWindow>{};
}

std::vector<WorkspaceWindow> WaylandWorkspaces::workspaceWindows(wl_output* output) const {
  return m_activeBackend != nullptr ? m_activeBackend->workspaceWindows(output) : std::vector<WorkspaceWindow>{};
}

void WaylandWorkspaces::focusWindow(const std::string& windowId) const {
  if (m_activeBackend != nullptr) {
    m_activeBackend->focusWindow(windowId);
  }
}

std::optional<std::string> WaylandWorkspaces::focusedWindowId() const { return std::nullopt; }

std::vector<Workspace> WaylandWorkspaces::all() const {
  return m_activeBackend != nullptr ? m_activeBackend->all() : std::vector<Workspace>{};
}

std::vector<Workspace> WaylandWorkspaces::forOutput(wl_output* output) const {
  return m_activeBackend != nullptr ? m_activeBackend->forOutput(output) : std::vector<Workspace>{};
}

void WaylandWorkspaces::setActiveBackend(WorkspaceBackend* backend) {
  m_activeBackend = backend;
  kLog.info("workspace backend={}", backendName());
}

void WaylandWorkspaces::notifyChanged() const {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
