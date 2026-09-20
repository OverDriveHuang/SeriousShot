#include "domain/output/output_classifier.hpp"
#include "test_support.hpp"

namespace {

using namespace hdrshot;

RangeFitResult range_fit(const bool fits_sdr) {
  return RangeFitResult{fits_sdr, 4U, 0U, 12U, "test"};
}

void fitting_content_routes_to_wide_gamut_sdr() {
  const auto result = OutputClassifier::classify({range_fit(true)});
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().output_class == OutputClass::wide_gamut_sdr);
  HDRSHOT_CHECK(result.value().encoding_intent == EncodingIntent::wide_gamut_sdr);
  HDRSHOT_CHECK(result.value().bit_depth == 16);
}

void over_range_content_routes_to_display_p3_pq() {
  const auto result = OutputClassifier::classify({range_fit(false)});
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().output_class == OutputClass::hdr);
  HDRSHOT_CHECK(result.value().encoding_intent == EncodingIntent::hdr_pq);
}

void display_state_is_not_part_of_the_classification_request() {
  const auto request = ClassifyOutputRequest{range_fit(true)};
  const auto result = OutputClassifier::classify(request);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().encoding_intent == EncodingIntent::wide_gamut_sdr);
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"D7-SDR-001 fitting content routes to Display P3 SDR", fitting_content_routes_to_wide_gamut_sdr},
      {"D7-HDR-001 over-range content routes to Display P3 PQ", over_range_content_routes_to_display_p3_pq},
      {"D7-STATE-001 display state is absent from policy", display_state_is_not_part_of_the_classification_request},
  });
}
