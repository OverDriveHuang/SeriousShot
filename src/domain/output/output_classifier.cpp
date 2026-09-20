#include "domain/output/output_classifier.hpp"

namespace hdrshot {

Result<OutputPlan, Error> OutputClassifier::classify(const ClassifyOutputRequest& request) {
  if (request.range_fit.fits_sdr) {
    return Result<OutputPlan, Error>::success(OutputPlan{
        OutputClass::wide_gamut_sdr,
        EncodingIntent::wide_gamut_sdr,
        16,
        "source_visible_range_fits_display_p3_sdr",
    });
  }
  return Result<OutputPlan, Error>::success(OutputPlan{
      OutputClass::hdr,
      EncodingIntent::hdr_pq,
      16,
      "source_visible_range_requires_display_p3_pq",
  });
}

}  // namespace hdrshot
