#include "domain/analysis/swatch_pairs.hpp"

#include "domain/analysis/color_difference.hpp"
#include "domain/analysis/math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrshot::analysis {

PairAddResult SwatchPairModel::add_pair(
    std::uint64_t source_id, std::uint64_t target_id,
    const std::vector<SampleRequest> &fixed_samples) {
  if (source_id == 0 || target_id == 0 || source_id == target_id ||
      !has_fixed(fixed_samples, source_id) ||
      !has_fixed(fixed_samples, target_id))
    return PairAddResult::invalid;
  const auto key = [](std::uint64_t a, std::uint64_t b) {
    return std::pair{std::min(a, b), std::max(a, b)};
  };
  const auto wanted = key(source_id, target_id);
  const auto found = std::find_if(pairs_.begin(), pairs_.end(), [&](const auto &p) {
    return key(p.source_id, p.target_id) == wanted;
  });
  if (found != pairs_.end())
    return PairAddResult::already_exists;
  pairs_.push_back({source_id, target_id});
  return PairAddResult::added;
}

bool SwatchPairModel::remove_pair(std::uint64_t a, std::uint64_t b) {
  const auto old = pairs_.size();
  pairs_.erase(std::remove_if(pairs_.begin(), pairs_.end(), [a, b](const auto &p) {
                return (p.source_id == a && p.target_id == b) ||
                       (p.source_id == b && p.target_id == a);
              }),
              pairs_.end());
  return pairs_.size() != old;
}

void SwatchPairModel::remove_swatch(std::uint64_t id) {
  pairs_.erase(std::remove_if(pairs_.begin(), pairs_.end(), [id](const auto &p) {
                return p.source_id == id || p.target_id == id;
              }),
              pairs_.end());
}

bool SwatchPairModel::has_fixed(const std::vector<SampleRequest> &samples,
                                std::uint64_t id) {
  return find_fixed(samples, id) != nullptr;
}

const SampleRequest *SwatchPairModel::find_fixed(
    const std::vector<SampleRequest> &samples, std::uint64_t id) {
  const auto it = std::find_if(samples.begin(), samples.end(),
                               [id](const auto &sample) {
                                 return sample.id == id;
                               });
  return it == samples.end() ? nullptr : &*it;
}

const SampleResult *SwatchPairModel::find_matching_result(
    const ResultData &snapshot, const SampleRequest &request) {
  const auto it = std::find_if(snapshot.samples.begin(), snapshot.samples.end(),
                               [&](const auto &sample) {
                                 const auto &r = sample.request;
                                 return r.id == request.id && r.x == request.x &&
                                        r.y == request.y && r.side == request.side &&
                                        r.respect_mask == request.respect_mask;
                               });
  return it == snapshot.samples.end() ? nullptr : &*it;
}

bool SwatchPairModel::finite_readout(const Readout &readout) {
  return std::all_of(readout.perceptual.begin(), readout.perceptual.end(),
                     [](double value) { return std::isfinite(value); }) &&
         std::all_of(readout.display_rgb.begin(), readout.display_rgb.end(),
                     [](float value) { return std::isfinite(value); });
}

PairEvaluation SwatchPairModel::evaluate_pair(
    const SwatchPair &pair, const Request &current_request,
    const ResultRef &snapshot) const {
  PairEvaluation output;
  output.pair = pair;
  output.metric = is_hdr(current_request.settings.working_space)
                      ? PairMetric::itp
                      : PairMetric::lab_d65;
  const auto *source_request = find_fixed(current_request.samples, pair.source_id);
  const auto *target_request = find_fixed(current_request.samples, pair.target_id);
  if (!source_request || !target_request || !snapshot ||
      snapshot->settings != current_request.settings) {
    output.state = PairState::pending;
    return output;
  }
  const auto *source = find_matching_result(*snapshot, *source_request);
  const auto *target = find_matching_result(*snapshot, *target_request);
  if (!source || !target) {
    output.state = PairState::pending;
    return output;
  }
  if (source->mean.valid_count == 0 || target->mean.valid_count == 0 ||
      !finite_readout(source->mean) || !finite_readout(target->mean)) {
    output.state = PairState::unavailable;
    return output;
  }
  output.source_color = source->mean.display_rgb;
  output.target_color = target->mean.display_rgb;
  output.value = output.metric == PairMetric::itp
                     ? delta_e_itp(source->mean.perceptual, target->mean.perceptual)
                     : delta_e_2000(source->mean.perceptual, target->mean.perceptual);
  if (!output.value) {
    output.state = PairState::unavailable;
    output.source_color.reset();
    output.target_color.reset();
  } else {
    output.state = PairState::ready;
  }
  return output;
}

std::vector<PairEvaluation> SwatchPairModel::evaluate(
    const Request &current_request, const ResultRef &snapshot) const {
  std::vector<PairEvaluation> values;
  values.reserve(pairs_.size());
  for (const auto &pair : pairs_)
    values.push_back(evaluate_pair(pair, current_request, snapshot));
  return values;
}

} // namespace hdrshot::analysis
