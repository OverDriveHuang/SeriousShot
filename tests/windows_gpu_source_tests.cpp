#include "platform/windows/windows_gpu_source.hpp"
#include "test_support.hpp"
#include <cmath>
#include <limits>
#include <utility>
using namespace hdrshot;
namespace {
void gain_vectors_and_alpha() {
  const std::vector<std::uint16_t> raw{0x4000, 0x4000, 0x4000, 0x7e00};
  for (const auto [gain, expected] :
       {std::pair{0.0, 0.0}, {0.5, 1.0}, {1.0, 2.0}, {3.0, 6.0}}) {
    auto source = windows_normalize_scrgb_source({1, 1}, raw, 1.0F, gain);
    HDRSHOT_CHECK(source.has_value());
    HDRSHOT_CHECK(source.value()->byte_count() == 16U);
    auto region = source.value()->read_region({0, 0, 1, 1});
    HDRSHOT_CHECK(region.has_value());
    for (int c = 0; c < 3; ++c)
      HDRSHOT_CHECK_NEAR(region.value()[c], expected, 2e-6);
    HDRSHOT_CHECK_NEAR(region.value()[3], 1.0, 0);
  }
  auto white = windows_normalize_scrgb_source({1, 1}, raw, 0.4F, 0.5);
  HDRSHOT_CHECK(white.has_value());
  auto scaled = white.value()->read_region({0, 0, 1, 1});
  HDRSHOT_CHECK(scaled.has_value());
  HDRSHOT_CHECK_NEAR(scaled.value()[0], 0.4, 2e-6);
}
void invalid_source_and_gain() {
  const std::vector<std::uint16_t> raw{0x4000, 0x4000, 0x4000, 0x3c00};
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({1, 1}, raw, 1.0F, -0.01));
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({1, 1}, raw, 1.0F, 3.01));
  HDRSHOT_CHECK(!windows_normalize_scrgb_source(
      {1, 1}, raw, 1.0F, std::numeric_limits<double>::quiet_NaN()));
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({1, 1}, raw, 0.0F, 1.0));
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({2, 1}, raw, 1.0F, 1.0));
  auto invalid = raw;
  invalid[0] = 0x7c00;
  HDRSHOT_CHECK(!windows_normalize_scrgb_source({1, 1}, invalid, 1.0F, 1.0));
}
void negative_linear_input_is_preserved() {
  const std::vector<std::uint16_t> raw{0xb400, 0, 0, 0}; // -0.25 red
  auto source = windows_normalize_scrgb_source({1, 1}, raw, 1.0F, 1.0);
  HDRSHOT_CHECK(source.has_value());
  auto pixel = source.value()->read_region({0, 0, 1, 1});
  HDRSHOT_CHECK(pixel.has_value());
  HDRSHOT_CHECK_NEAR(pixel.value()[0], -0.25 * 0.82246197, 2e-6);
  HDRSHOT_CHECK_NEAR(pixel.value()[1], -0.25 * 0.03319420, 2e-6);
}
} // namespace
int main() {
  return test::run({{"GPU source gain vectors", gain_vectors_and_alpha},
                    {"GPU source input validation", invalid_source_and_gain},
                    {"GPU source retains negative linear input",
                     negative_linear_input_is_preserved}});
}
