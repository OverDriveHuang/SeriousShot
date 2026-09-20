#pragma once

#include "core/error.hpp"
#include "core/result.hpp"
#include "domain/output/range_fit.hpp"

#include <cstdint>

namespace hdrshot {

enum class OutputClass : std::uint8_t { sdr, wide_gamut_sdr, hdr };
enum class EncodingIntent : std::uint8_t { sdr, wide_gamut_sdr, hdr_pq };

struct ClassifyOutputRequest {
  // The injected source interpreter has already normalized its platform's
  // source encoding into this EDR-relative range result.
  RangeFitResult range_fit;
};

struct OutputPlan {
  OutputClass output_class{OutputClass::sdr};
  EncodingIntent encoding_intent{EncodingIntent::sdr};
  std::uint8_t bit_depth{16};
  const char* rationale_code{"sdr_srgb"};
};

class OutputClassifier {
 public:
  [[nodiscard]] static Result<OutputPlan, Error> classify(const ClassifyOutputRequest& request);
};

}  // namespace hdrshot
