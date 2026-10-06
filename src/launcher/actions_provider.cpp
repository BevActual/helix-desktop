#include "launcher/actions_provider.h"

#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"

ActionsProvider::ActionsProvider(desktop_actions::Services services) : m_services(services) {}

std::string ActionsProvider::displayName() const { return i18n::tr("launcher.providers.actions.title"); }

std::vector<LauncherResult> ActionsProvider::query(std::string_view text) const {
  const auto actions = desktop_actions::query(text, m_services);
  std::vector<LauncherResult> results;
  results.reserve(actions.size());
  for (const auto& action : actions) {
    LauncherResult result;
    result.id = action.id;
    result.title = action.title;
    result.subtitle = action.subtitle;
    result.glyphName = action.glyph;
    results.push_back(std::move(result));
  }
  return results;
}

bool ActionsProvider::activate(const LauncherResult& result) {
  if (!result.providerId.empty() && result.providerId != id()) {
    return false;
  }
  if (result.id.empty()) {
    return false;
  }

  desktop_actions::Request request;
  request.id = result.id;
  if (m_services.panels != nullptr && m_services.panels->isOpenPanel("launcher")) {
    request.output = m_services.panels->attachedPanelOutput();
    request.sourceBarName = m_services.panels->attachedSourceBarName();
  }
  desktop_actions::schedule(m_services, std::move(request));
  return true;
}
