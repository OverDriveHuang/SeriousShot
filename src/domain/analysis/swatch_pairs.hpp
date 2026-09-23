#pragma once

#include "domain/analysis/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace hdrshot::analysis {

struct SwatchPair {
  std::uint64_t source_id{};
  std::uint64_t target_id{};
  friend bool operator==(const SwatchPair &, const SwatchPair &) = default;
};

enum class PairAddResult { added, already_exists, invalid };
enum class PairState { ready, pending, unavailable };
enum class PairMetric { itp, lab_d65 };

struct PairEvaluation {
  SwatchPair pair;
  PairMetric metric{PairMetric::itp};
  PairState state{PairState::pending};
  std::optional<double> value;
  std::optional<std::array<float, 3>> source_color;
  std::optional<std::array<float, 3>> target_color;
};

class SwatchPairModel final {
public:
  PairAddResult add_pair(std::uint64_t source_id, std::uint64_t target_id,
                         const std::vector<SampleRequest> &fixed_samples);
  bool remove_pair(std::uint64_t a, std::uint64_t b);
  void remove_swatch(std::uint64_t id);
  void clear() noexcept { pairs_.clear(); }
  [[nodiscard]] const std::vector<SwatchPair> &pairs() const noexcept {
    return pairs_;
  }

  [[nodiscard]] PairEvaluation evaluate_pair(const SwatchPair &pair,
                                             const Request &current_request,
                                             const ResultRef &snapshot) const;
  [[nodiscard]] std::vector<PairEvaluation>
  evaluate(const Request &current_request, const ResultRef &snapshot) const;

private:
  [[nodiscard]] static bool has_fixed(const std::vector<SampleRequest> &samples,
                                      std::uint64_t id);
  [[nodiscard]] static const SampleRequest *find_fixed(
      const std::vector<SampleRequest> &samples, std::uint64_t id);
  [[nodiscard]] static const SampleResult *find_matching_result(
      const ResultData &snapshot, const SampleRequest &request);
  static bool finite_readout(const Readout &readout);
  std::vector<SwatchPair> pairs_;
};

} // namespace hdrshot::analysis
