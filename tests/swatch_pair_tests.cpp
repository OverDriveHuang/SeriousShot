#include "domain/analysis/color_difference.hpp"
#include "domain/analysis/swatch_pairs.hpp"
#include "test_support.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace {
using namespace hdrshot::analysis;

void difference_contracts() {
  HDRSHOT_CHECK_NEAR(*delta_e_itp({0, 0, 0}, {1, 0, 0}), 720, 1e-12);
  HDRSHOT_CHECK_NEAR(*delta_e_itp({0, 0, 0}, {0, -1, 0}), 720, 1e-12);
  HDRSHOT_CHECK_NEAR(*delta_e_itp({0, 0, 0}, {0, 0, -1}), 720, 1e-12);
  HDRSHOT_CHECK_NEAR(*delta_e_2000({50, 2.6772, -79.7751},
                                  {50, 0, -82.7485}),
                    2.0425, 5.1e-5);
  HDRSHOT_CHECK_NEAR(*delta_e_2000({50, 3.1571, -77.2803},
                                  {50, 0, -82.7485}),
                    2.8615, 5.1e-5);
  HDRSHOT_CHECK(!delta_e_itp({NAN, 0, 0}, {0, 0, 0}));
  HDRSHOT_CHECK(!delta_e_itp({0, INFINITY, 0}, {0, 0, 0}));
  HDRSHOT_CHECK(!delta_e_2000({50, -INFINITY, 0}, {0, 0, 0}));
  HDRSHOT_CHECK(!delta_e_2000({50, 0, 0}, {50, 0, NAN}));
}

void sharma_supplementary_vectors() {
  // G. Sharma, W. Wu, E. N. Dalal, supplementary CIEDE2000 test data,
  // University of Rochester plain-text source (34 rows):
  // https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/dataNprograms/ciede2000testdata.txt
  // Published ΔE values are rounded to four decimals.
  constexpr double v[][7] = {
      {50,2.6772,-79.7751,50,0,-82.7485,2.0425},
      {50,3.1571,-77.2803,50,0,-82.7485,2.8615},
      {50,2.8361,-74.02,50,0,-82.7485,3.4412},
      {50,-1.3802,-84.2814,50,0,-82.7485,1.0},
      {50,-1.1848,-84.8006,50,0,-82.7485,1.0},
      {50,-0.9009,-85.5211,50,0,-82.7485,1.0},
      {50,0,0,50,-1,2,2.3669},
      {50,-1,2,50,0,0,2.3669},
      {50,2.49,-0.001,50,-2.49,0.0009,7.1792},
      {50,2.49,-0.001,50,-2.49,0.001,7.1792},
      {50,2.49,-0.001,50,-2.49,0.0011,7.2195},
      {50,2.49,-0.001,50,-2.49,0.0012,7.2195},
      {50,-0.001,2.49,50,0.0009,-2.49,4.8045},
      {50,-0.001,2.49,50,0.001,-2.49,4.8045},
      {50,-0.001,2.49,50,0.0011,-2.49,4.7461},
      {50,2.5,0,50,0,-2.5,4.3065},
      {50,2.5,0,73,25,-18,27.1492},
      {50,2.5,0,61,-5,29,22.8977},
      {50,2.5,0,56,-27,-3,31.903},
      {50,2.5,0,58,24,15,19.4535},
      {50,2.5,0,50,3.1736,0.5854,1.0},
      {50,2.5,0,50,3.2972,0,1.0},
      {50,2.5,0,50,1.8634,0.5757,1.0},
      {50,2.5,0,50,3.2592,0.335,1.0},
      {60.2574,-34.0099,36.2677,60.4626,-34.1751,39.4387,1.2644},
      {63.0109,-31.0961,-5.8663,62.8187,-29.7946,-4.0864,1.263},
      {61.2901,3.7196,-5.3901,61.4292,2.248,-4.962,1.8731},
      {35.0831,-44.1164,3.7933,35.0232,-40.0716,1.5901,1.8645},
      {22.7233,20.0904,-46.694,23.0331,14.973,-42.5619,2.0373},
      {36.4612,47.858,18.3852,36.2715,50.5065,21.2231,1.4146},
      {90.8027,-2.0831,1.441,91.1528,-1.6435,0.0447,1.4441},
      {90.9257,-0.5406,-0.9208,88.6381,-0.8985,-0.7239,1.5381},
      {6.7747,-0.2908,-2.4247,5.8714,-0.0985,-2.2286,0.6377},
      {2.0776,0.0795,-1.135,0.9033,-0.0636,-0.5514,0.9082},
  };
  static_assert(std::size(v) == 34);
  for (const auto &row : v) {
    const std::array<double, 3> a{row[0], row[1], row[2]};
    const std::array<double, 3> b{row[3], row[4], row[5]};
    HDRSHOT_CHECK_NEAR(*delta_e_2000(a, b), row[6], 5.1e-5);
    HDRSHOT_CHECK_NEAR(*delta_e_2000(b, a), row[6], 5.1e-5);
  }
}

void pair_lifecycle_and_deduplication() {
  std::vector<SampleRequest> samples{{1, 2, 3, 1, false},
                                     {2, 4, 5, 3, false},
                                     {3, 6, 7, 5, false}};
  SwatchPairModel model;
  HDRSHOT_CHECK(model.add_pair(1, 2, samples) == PairAddResult::added);
  HDRSHOT_CHECK(model.add_pair(2, 1, samples) == PairAddResult::already_exists);
  HDRSHOT_CHECK(model.add_pair(1, 1, samples) == PairAddResult::invalid);
  HDRSHOT_CHECK(model.add_pair(1, 0, samples) == PairAddResult::invalid);
  HDRSHOT_CHECK(model.add_pair(1, 9, samples) == PairAddResult::invalid);
  HDRSHOT_CHECK(model.add_pair(1, 3, samples) == PairAddResult::added);
  HDRSHOT_CHECK(model.add_pair(2, 3, samples) == PairAddResult::added);
  HDRSHOT_CHECK(model.pairs().size() == 3);
  HDRSHOT_CHECK(model.remove_pair(2, 1));
  HDRSHOT_CHECK(model.pairs().size() == 2);
  HDRSHOT_CHECK(!model.remove_pair(2, 1));
  model.remove_swatch(3);
  HDRSHOT_CHECK(model.pairs().empty());
}

void pair_snapshot_identity_and_states() {
  Request request;
  request.settings.working_space = WorkingSpace::display_p3_pq;
  request.samples = {{1, 2, 3, 1, false}, {2, 4, 5, 1, false}};
  SwatchPairModel model;
  HDRSHOT_CHECK(model.add_pair(1, 2, request.samples) == PairAddResult::added);
  HDRSHOT_CHECK(model.evaluate(request, {}).front().state == PairState::pending);
  auto result = std::make_shared<ResultData>();
  result->settings = request.settings;
  for (const auto &sample : request.samples) {
    SampleResult value;
    value.request = sample;
    value.mean.valid_count = 4;
    value.mean.perceptual = sample.id == 1 ? std::array<double, 3>{.5, .1, -.2}
                                          : std::array<double, 3>{.6, .1, -.2};
    value.mean.display_rgb = {0.2f, 0.3f, 0.4f};
    result->samples.push_back(value);
  }
  auto ready = model.evaluate(request, result);
  HDRSHOT_CHECK(ready.front().state == PairState::ready);
  HDRSHOT_CHECK(ready.front().value && *ready.front().value > 0);
  result->settings.working_space = WorkingSpace::display_p3_sdr;
  HDRSHOT_CHECK(model.evaluate(request, result).front().state == PairState::pending);
  result->settings = request.settings;
  result->samples.front().mean.valid_count = 0;
  HDRSHOT_CHECK(model.evaluate(request, result).front().state == PairState::unavailable);
  result->samples.front().mean.valid_count = 4;
  result->samples.front().request.x += 1;
  HDRSHOT_CHECK(model.evaluate(request, result).front().state == PairState::pending);
  result->samples.front().request = request.samples.front();
  result->samples.front().mean.perceptual[0] =
      std::numeric_limits<double>::quiet_NaN();
  HDRSHOT_CHECK(model.evaluate(request, result).front().state == PairState::unavailable);
}
} // namespace

int main() {
  return hdrshot::test::run({
      {"shared difference contracts", difference_contracts},
      {"Sharma supplementary 34-vector CIEDE2000 data", sharma_supplementary_vectors},
      {"pair lifecycle and unordered deduplication", pair_lifecycle_and_deduplication},
      {"snapshot identity and pending/unavailable states", pair_snapshot_identity_and_states},
  });
}
