// Offline mesh exporter. The color math is the same FP32 definition used by
// the CPU/Metal analyzer, NOT a separate Python copy of its matrices/transfer.
#include "domain/analysis/math.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  if (argc != 3) return 1;
  const unsigned space = unsigned(std::atoi(argv[1]));
  const int n = std::atoi(argv[2]);
  if (space > 3 || n < 2 || n > 1025) return 2;
  using namespace hdrshot::analysis_math;
  for (int axis = 0; axis < 3; ++axis)
    for (float edge : {0.f, 1.f})
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
          float rgb[3]{};
          rgb[axis] = edge;
          int index = 0;
          for (int k = 0; k < 3; ++k)
            if (k != axis) rgb[k] = float(index++ ? j : i) / float(n - 1);
          auto v = a3(rgb[0], rgb[1], rgb[2]);
          if (space >= 2) v = a3(pq_decode(v.x), pq_decode(v.y), pq_decode(v.z));
          const auto plane = space >= 2
              ? itp_from_lms(lms_from_rgb(v, working_gamut(space)))
              : lab_from_xyz(xyz_from_rgb(v, working_gamut(space)));
          const std::array<float, 2> point{plane.y, plane.z};
          if (std::fwrite(point.data(), sizeof(float), 2, stdout) != 2) return 3;
        }
  return 0;
}
