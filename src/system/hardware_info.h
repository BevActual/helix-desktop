#pragma once

#include <optional>
#include <string>
#include <vector>

[[nodiscard]] std::string cpuModelName();
[[nodiscard]] std::string gpuLabel();
[[nodiscard]] std::string motherboardLabel();
[[nodiscard]] std::string memoryTotalLabel();
[[nodiscard]] std::string compositorLabel();

// Mount points of physical (block-device backed) filesystems, deduped by backing device and sorted
// so the root filesystem comes first. Excludes pseudo filesystems, snap/loop images, boot and EFI.
[[nodiscard]] std::vector<std::string> physicalDiskMountPoints();

// "total GB (NN%)" for the filesystem containing the given mount point.
[[nodiscard]] std::string diskUsageLabel(const std::string& mountPoint);

// Used fraction [0, 1] of the filesystem containing the given mount point; nullopt when unknown.
[[nodiscard]] std::optional<float> diskUsageFraction(const std::string& mountPoint);
