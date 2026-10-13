#pragma once

#include <memory>

namespace compositors {

  namespace niri {
    class NiriRuntime;
  } // namespace niri

  class CompositorRuntimeRegistry {
  public:
    CompositorRuntimeRegistry();
    ~CompositorRuntimeRegistry();

    CompositorRuntimeRegistry(const CompositorRuntimeRegistry&) = delete;
    CompositorRuntimeRegistry& operator=(const CompositorRuntimeRegistry&) = delete;

    [[nodiscard]] niri::NiriRuntime& niri() noexcept;
    [[nodiscard]] const niri::NiriRuntime& niri() const noexcept;

  private:
    std::unique_ptr<niri::NiriRuntime> m_niri;
  };

} // namespace compositors
