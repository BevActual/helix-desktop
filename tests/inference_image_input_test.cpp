// Pictures a client sends are untrusted: they are sniffed, bounded and decoded before an engine sees them.
#include "inference/image_input.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

  int failures = 0;
  void check(bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  }

  const std::vector<std::uint8_t> kSmallPng = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
                                               0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
                                               0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24,
                                               0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63,
                                               0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x19, 0x18, 0xfe, 0x83, 0x08,
                                               0x06, 0x00, 0x3f, 0xd2, 0x05, 0xfb, 0xdc, 0x02, 0xa3, 0xd4, 0x00,
                                               0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
  const std::vector<std::uint8_t> kHugeHeaderPng = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
                                                    0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0xea, 0x60, 0x00, 0x00,
                                                    0xea, 0x60, 0x08, 0x06, 0x00, 0x00, 0x00, 0x80, 0xd2, 0x75, 0x42};

} // namespace

int main() {
  using namespace helix::inference;

  const auto small = decodeImageInput(kSmallPng.data(), kSmallPng.size());
  check(small.has_value(), "a small PNG decodes");
  if (small) {
    check(small->width == 2 && small->height == 2, "dimensions are kept");
    check(small->rgb.size() == 12, "RGB, three bytes a pixel");
    check(small->rgb[0] == 255 && small->rgb[1] == 0 && small->rgb[2] == 0, "red stays red");
    check(small->rgb[9] == 255 && small->rgb[10] == 255 && small->rgb[11] == 255, "a transparent pixel becomes white");
  }

  const std::string text = "<svg onload=alert(1)/>";
  check(
      !decodeImageInput(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()).has_value(),
      "text is not a picture"
  );
  check(!decodeImageInput(nullptr, 0).has_value(), "nothing is not a picture");
  check(
      !decodeImageInput(kHugeHeaderPng.data(), kHugeHeaderPng.size()).has_value(),
      "a picture that claims 60000x60000 is refused"
  );
  std::vector<std::uint8_t> truncated(kSmallPng.begin(), kSmallPng.begin() + 30);
  check(!decodeImageInput(truncated.data(), truncated.size()).has_value(), "a truncated picture is refused");

  ImageInput big;
  big.width = 3000;
  big.height = 1500;
  big.rgb.assign(static_cast<std::size_t>(big.width) * big.height * 3, 100);
  const auto scaled = scaleDown(big, 1500);
  check(scaled.width == 1500 && scaled.height == 750, "scaling keeps the aspect ratio");
  check(scaled.rgb.size() == 1500U * 750 * 3 && scaled.rgb[0] == 100, "scaling averages pixels");
  const auto same = scaleDown(small ? *small : big, 4000);
  check(same.width == (small ? 2U : 3000U), "a small picture is not scaled");

  if (failures)
    return 1;
  std::puts("ok: image input");
  return 0;
}
