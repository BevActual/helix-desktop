#pragma once

#include <functional>

class ConfigService;
class IpcService;
class LockScreen;
class SessionActionRunner;

void registerSessionIpc(
    IpcService& ipc, SessionActionRunner& runner, LockScreen& lockScreen, ConfigService& config,
    std::function<bool()> keepSessionOnLidClose = {}
);
