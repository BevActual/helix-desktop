#include "theme/hook_runner.h"

#include "core/log.h"
#include "core/process/process.h"
#include "util/string_utils.h"

#include <utility>

namespace helix::theme {

  namespace {
    constexpr Logger kLog("hook_runner");
    // Hook output is only read back for the failure warning.
    constexpr std::size_t kMaxHookOutputBytes = 8 * 1024;

    [[nodiscard]] std::string compactHookError(const process::RunResult& result) {
      std::string text = result.err.empty() ? result.out : result.err;
      if (const auto pos = text.find('\n'); pos != std::string::npos) {
        text.resize(pos);
      }
      text = StringUtils::trim(text);
      if (text.empty()) {
        if (result.timedOut) {
          return "timed out";
        }
        return "exit " + std::to_string(result.exitCode);
      }
      constexpr std::size_t kMax = 160;
      if (text.size() > kMax) {
        text.resize(kMax);
      }
      return text;
    }

  } // namespace

  HookRunner::HookRunner(std::size_t maxConcurrent) : m_state(std::make_shared<State>()) {
    m_state->maxConcurrent = maxConcurrent > 0 ? maxConcurrent : kDefaultMaxConcurrent;
  }

  HookRunner::~HookRunner() {
    requestShutdown();
    std::unique_lock lock(m_state->mutex);
    // Hooks that already started own the shared state; wait them out instead of
    // killing a command halfway through rewriting an application's config.
    m_state->idleCv.wait(lock, [this]() { return m_state->running == 0; });
  }

  void HookRunner::requestShutdown() {
    std::scoped_lock lock(m_state->mutex);
    m_state->shutdown = true;
    m_state->queue.clear();
    // Releases waitIdle() callers: the queued backlog is gone and running hooks
    // are awaited by the destructor, not by whoever was draining.
    m_state->idleCv.notify_all();
  }

  void HookRunner::enqueue(std::string command, std::uint64_t generation, std::string templateId) {
    if (command.empty()) {
      return;
    }
    {
      std::scoped_lock lock(m_state->mutex);
      if (m_state->shutdown || generation < m_state->currentGeneration) {
        return;
      }
      m_state->queue.emplace_back(QueuedHook{std::move(command), std::move(templateId), generation});
    }
    pump(m_state);
  }

  void HookRunner::setOnFailure(std::function<void(std::string templateId, std::string error)> callback) {
    std::scoped_lock lock(m_state->mutex);
    m_state->onFailure = std::move(callback);
  }

  void HookRunner::invalidateBefore(std::uint64_t generation) {
    std::scoped_lock lock(m_state->mutex);
    if (generation <= m_state->currentGeneration) {
      return;
    }
    m_state->currentGeneration = generation;
    std::erase_if(m_state->queue, [generation](const QueuedHook& hook) { return hook.generation < generation; });
    if (m_state->queue.empty() && m_state->running == 0) {
      m_state->idleCv.notify_all();
    }
  }

  void HookRunner::waitIdle() {
    std::unique_lock lock(m_state->mutex);
    m_state->idleCv.wait(lock, [this]() {
      return m_state->shutdown || (m_state->queue.empty() && m_state->running == 0);
    });
  }

  std::size_t HookRunner::pendingCount() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->queue.size() + m_state->running;
  }

  void HookRunner::pump(const std::shared_ptr<State>& state) {
    for (;;) {
      QueuedHook hook;
      {
        std::scoped_lock lock(state->mutex);
        while (!state->shutdown && state->running < state->maxConcurrent && !state->queue.empty()) {
          QueuedHook next = std::move(state->queue.front());
          state->queue.pop_front();
          if (next.generation < state->currentGeneration) {
            continue;
          }
          hook = std::move(next);
          ++state->running;
          break;
        }

        if (hook.command.empty()) {
          if (state->queue.empty() && state->running == 0) {
            state->idleCv.notify_all();
          }
          return;
        }
      }

      if (!launch(state, std::move(hook))) {
        std::scoped_lock lock(state->mutex);
        --state->running;
      }
    }
  }

  bool HookRunner::launch(const std::shared_ptr<State>& state, QueuedHook hook) {
    process::RunCallbacks callbacks;
    callbacks.onExit = [state, command = hook.command, templateId = hook.templateId,
                        generation = hook.generation](process::RunResult result) {
      if (!result) {
        kLog.warn("hook failed with exit code {}: {} (command: {})", result.exitCode, result.err, command);
        if (!templateId.empty()) {
          std::function<void(std::string, std::string)> handler;
          {
            std::scoped_lock lock(state->mutex);
            if (!state->shutdown && generation >= state->currentGeneration && state->onFailure) {
              handler = state->onFailure;
            }
          }
          if (handler) {
            handler(templateId, compactHookError(result));
          }
        }
      }
      {
        std::scoped_lock lock(state->mutex);
        --state->running;
      }
      pump(state);
    };

    process::RunOptions options;
    options.maxOutputBytes = kMaxHookOutputBytes;
    if (process::runAsync(hook.command, std::move(callbacks), options)) {
      return true;
    }
    kLog.warn("failed to start hook (command: {})", hook.command);
    if (!hook.templateId.empty()) {
      std::function<void(std::string, std::string)> handler;
      {
        std::scoped_lock lock(state->mutex);
        if (!state->shutdown && hook.generation >= state->currentGeneration && state->onFailure) {
          handler = state->onFailure;
        }
      }
      if (handler) {
        handler(std::move(hook.templateId), "failed to start");
      }
    }
    return false;
  }

} // namespace helix::theme
