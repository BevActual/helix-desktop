#include "shell/control_center/plugin_control_center_tab.h"

#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

namespace {

  constexpr Logger kLog("plugin-control-center-tab");
  constexpr int kDefaultUpdateIntervalMs = 1000;

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
      return {};
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
  }

} // namespace

PluginControlCenterTab::PluginControlCenterTab(scripting::PluginRuntimeContext context)
    : m_entryId(std::move(context.entryId)), m_sourcePath(std::move(context.sourcePath)),
      m_pluginDir(std::move(context.pluginDir)), m_scriptApi(context.scriptApi), m_settings(std::move(context.settings)),
      m_fileWatcher(context.fileWatcher), m_httpClient(context.httpClient), m_clipboard(context.clipboard),
      m_platform(context.platform) {
  scripting::PluginIpcRouter::instance().registerEndpoint(this);
}

PluginControlCenterTab::~PluginControlCenterTab() {
  scripting::PluginIpcRouter::instance().unregisterEndpoint(this);
  if (m_alive) {
    *m_alive = false;
  }
  teardownScriptWatch();
  if (m_runtime != nullptr) {
    if (m_runtimeSubscription != 0) {
      m_runtime->unsubscribe(m_runtimeSubscription);
    }
    m_runtime->stop();
  }
}

std::unique_ptr<Flex> PluginControlCenterTab::create() {
  auto flex = ui::column({
      .out = &m_flex,
      .align = FlexAlign::Stretch,
      .flexGrow = 1.0F,
  });

  m_reconciler.setCallbackSink([this](const ui::UiTreeReconciler::ControlCallback& callback) {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueCallStrings(
          callback.fn, callback.arg1, callback.arg2, makeScriptSnapshot(), callback.coalesce, callback.coalesceKey
      );
    }
  });
  m_reconciler.setPathResolver([this](const std::string& path) { return resolvePluginPath(path); });

  if (m_runtime == nullptr) {
    startScript();
  } else if (m_tree.has_value()) {
    m_treeDirty = true;
  }
  startUpdateTimer();
  setupScriptWatch();
  return flex;
}

void PluginControlCenterTab::startScript() {
  if (m_sourcePath.empty()) {
    kLog.warn("plugin control-center tab '{}': no source path", m_entryId);
    return;
  }
  std::string source = readFile(m_sourcePath);
  if (source.empty()) {
    kLog.warn("plugin control-center tab '{}': failed to read '{}'", m_entryId, m_sourcePath.string());
    return;
  }

  m_runtime = std::make_shared<scripting::ScriptRuntime>(
      m_entryId, m_settings, m_scriptApi, m_pluginDir, m_httpClient, m_clipboard
  );

  auto alive = std::weak_ptr<bool>(m_alive);
  m_runtimeSubscription = m_runtime->subscribe([this, alive](scripting::ScriptResult result) {
    auto token = alive.lock();
    if (token == nullptr || !*token) {
      return;
    }
    if (result.modulePathsKnown) {
      m_scriptWatcher.setModulePaths(result.modulePaths);
    }
    handleScriptResult(std::move(result));
  });

  m_runtime->start(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
}

void PluginControlCenterTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_runtime == nullptr) {
    return;
  }
  if (active) {
    (void)m_runtime->enqueueCallStrings("onOpen", m_entryId, {}, makeScriptSnapshot());
  } else {
    (void)m_runtime->enqueueCall("onClose", makeScriptSnapshot());
  }
}

void PluginControlCenterTab::onClose() {
  m_updateTimer.stop();
  teardownScriptWatch();
  if (m_active && m_runtime != nullptr) {
    (void)m_runtime->enqueueCall("onClose", makeScriptSnapshot());
  }
  m_active = false;
  m_flex = nullptr;
}

void PluginControlCenterTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_flex == nullptr) {
    return;
  }
  if (m_tree.has_value()) {
    m_reconciler.setScale(contentScale());
    (void)m_reconciler.reconcile(*m_flex, *m_tree, renderer);
    m_treeDirty = false;
  }
  m_flex->setSize(contentWidth, bodyHeight);
  m_flex->layout(renderer);
}

void PluginControlCenterTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_wantsSecondTicks && m_runtime != nullptr) {
    (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
  }
}

void PluginControlCenterTab::onFrameTick(float deltaMs) {
  if (m_runtime == nullptr || !m_needsFrameTick || !m_active) {
    return;
  }
  (void)m_runtime->enqueueCallStrings(
      "onFrameTick", std::format("{:.3F}", deltaMs), {}, makeScriptSnapshot(), /*coalesce=*/true
  );
}

PluginControlCenterTab::DispatchResult PluginControlCenterTab::dispatchIpc(
    std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot
) {
  (void)snapshot;
  if (m_runtime == nullptr) {
    return DispatchResult::MissingHost;
  }
  if (m_hasOnIpcKnown && !m_hasOnIpc) {
    return DispatchResult::MissingCallback;
  }
  if (!m_runtime->enqueueCallStrings("onIpc", std::string(event), std::string(payload), makeScriptSnapshot())) {
    return DispatchResult::Failed;
  }
  return DispatchResult::Handled;
}

void PluginControlCenterTab::handleScriptResult(scripting::ScriptResult result) {
  if (result.hasOnIpcKnown) {
    m_hasOnIpc = result.hasOnIpc;
    m_hasOnIpcKnown = true;
  }

  if (result.unhealthy) {
    m_updateTimer.stop();
    m_needsFrameTick = false;
    kLog.warn("plugin control-center tab '{}' disabled after repeated timeouts", m_entryId);
  }

  const auto& patch = result.patch;
  if (patch.wantsSecondTicks.has_value()) {
    m_wantsSecondTicks = *patch.wantsSecondTicks;
  }
  if (patch.needsFrameTick.has_value()) {
    m_needsFrameTick = *patch.needsFrameTick;
  }
  if (patch.updateIntervalMs.has_value()) {
    m_updateIntervalMs = std::max(16, *patch.updateIntervalMs);
    if (m_flex != nullptr) {
      startUpdateTimer();
    }
  }
  if (patch.uiTree.has_value() && (!m_tree.has_value() || *patch.uiTree != *m_tree)) {
    m_tree = *patch.uiTree;
    m_treeDirty = true;
    requestHostRefresh();
  }
}

scripting::ScriptSnapshot PluginControlCenterTab::makeScriptSnapshot() const {
  return scripting::ScriptSnapshot{
      .isVertical = false,
      .outputName = {},
      .barName = {},
      .focusedOutputName = focusedOutputName(),
  };
}

std::string PluginControlCenterTab::focusedOutputName() const {
  if (m_platform == nullptr) {
    return {};
  }
  wl_output* output = m_platform->preferredInteractiveOutput();
  const auto* info = m_platform->findOutputByWl(output);
  return info != nullptr ? info->connectorName : std::string{};
}

std::string PluginControlCenterTab::resolvePluginPath(const std::string& path) const {
  if (path.empty()) {
    return {};
  }
  if (path[0] == '~') {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      return std::string(home) + path.substr(1);
    }
    return path;
  }
  if (path[0] == '/') {
    return path;
  }
  return (m_pluginDir / path).string();
}

void PluginControlCenterTab::startUpdateTimer() {
  m_updateTimer.startRepeating(std::chrono::milliseconds(m_updateIntervalMs), [this] {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
    }
  });
}

void PluginControlCenterTab::setupScriptWatch() {
  m_scriptWatcher.start(m_fileWatcher, m_sourcePath, [this] { reloadScript(); });
}

void PluginControlCenterTab::teardownScriptWatch() { m_scriptWatcher.stop(); }

void PluginControlCenterTab::reloadScript() {
  std::string source = readFile(m_sourcePath);
  auto name = m_sourcePath.filename().string();
  if (source.empty() || m_runtime == nullptr) {
    kLog.warn("hot reload: failed to reload '{}'", name);
    notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    return;
  }

  m_wantsSecondTicks = false;
  m_needsFrameTick = false;
  m_updateIntervalMs = kDefaultUpdateIntervalMs;
  m_hasOnIpc = false;
  m_hasOnIpcKnown = false;

  m_runtime->reload(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
  startUpdateTimer();
  requestHostRefresh();
  kLog.info("hot reload: reloaded '{}'", name);
  notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
}

void PluginControlCenterTab::requestHostRefresh() {
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestRedraw();
}
