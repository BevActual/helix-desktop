#include "core/build_info.h"

#include "helix_git_revision.h"

namespace helix::build_info {

  std::string_view version() noexcept { return HELIX_VERSION; }

  std::string_view revision() noexcept { return HELIX_GIT_REVISION; }

  std::string displayVersion() {
    std::string label = "v";
    label += version();

    constexpr std::string_view unknownRevision = "unknown";
    const std::string_view rev = revision();
    if (!rev.empty() && rev != unknownRevision) {
      label += " (";
      label += rev;
      label += ')';
    }
    return label;
  }

} // namespace helix::build_info
