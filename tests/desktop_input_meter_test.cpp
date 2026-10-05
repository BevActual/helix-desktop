#include "desktop/input_meter.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
int main() {
  assert(helix::inputPeak({}) == 0);
  const float samples[] = {0, -0.25F, 0.75F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  std::array<std::byte, sizeof(samples) + 2> bytes{};
  std::memcpy(bytes.data() + 1, samples, sizeof(samples));
  assert(helix::inputPeak({bytes.data() + 1, sizeof(samples) + 1}) == 0.75);
  const float clipped[] = {-2, 1.5F};
  assert(helix::inputPeak(std::as_bytes(std::span(clipped))) == 1);
}
