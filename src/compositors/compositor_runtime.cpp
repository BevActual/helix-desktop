#include "compositors/compositor_runtime.h"

#include "compositors/niri/niri_runtime.h"

namespace compositors {

  CompositorRuntimeRegistry::CompositorRuntimeRegistry() : m_niri(std::make_unique<niri::NiriRuntime>()) {}

  CompositorRuntimeRegistry::~CompositorRuntimeRegistry() = default;

  niri::NiriRuntime& CompositorRuntimeRegistry::niri() noexcept { return *m_niri; }

  const niri::NiriRuntime& CompositorRuntimeRegistry::niri() const noexcept { return *m_niri; }

} // namespace compositors
