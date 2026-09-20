#include "ultra_hdr_icc_oracle.hpp"
#include <ultrahdr/icc.h>

namespace {
std::vector<std::uint8_t> generate(uhdr_color_transfer_t transfer) {
  const auto generated = ultrahdr::IccHelper::writeIccProfile(transfer, UHDR_CG_DISPLAY_P3);
  HDRSHOT_CHECK(generated != nullptr && generated->getLength() > 14U);
  const auto* bytes = static_cast<const std::uint8_t*>(generated->getData());
  return {bytes + 14, bytes + generated->getLength()};
}
void pq_a2b0_matches_eotf_and_retained_rendering() {
  const auto profile = generate(UHDR_CT_PQ);
  hdrshot::test::check_p3_pq_a2b0_values(profile);
  const auto cicp = hdrshot::test::icc_tag(profile, "cicp");
  HDRSHOT_CHECK(cicp.size() == 12U && cicp[8] == 12 && cicp[9] == 16);
}
void other_transfer_profiles_do_not_gain_pq_luts() {
  for (auto transfer : {UHDR_CT_SRGB, UHDR_CT_LINEAR}) {
    const auto profile = generate(transfer);
    HDRSHOT_CHECK(hdrshot::test::icc_tag(profile, "A2B0").empty());
    HDRSHOT_CHECK(!hdrshot::test::icc_tag(profile, "rTRC").empty());
  }
}
}
int main() {
  return hdrshot::test::run({
      {"PQ A2B0 serialized nodes: EOTF + retained tone mapping", pq_a2b0_matches_eotf_and_retained_rendering},
      {"SDR/linear profiles retain their transfer curves", other_transfer_profiles_do_not_gain_pq_luts}});
}
