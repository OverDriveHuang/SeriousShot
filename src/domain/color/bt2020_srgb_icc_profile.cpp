#include "domain/color/bt2020_srgb_icc_profile.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace hdrshot {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t signature(
    const char a,
    const char b,
    const char c,
    const char d) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
      static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

void append_u16(Bytes& bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_u32(Bytes& bytes, const std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void patch_u16(Bytes& bytes, const std::size_t offset, const std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  bytes[offset + 1U] = static_cast<std::uint8_t>(value & 0xFFU);
}

void patch_u32(Bytes& bytes, const std::size_t offset, const std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
  bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  bytes[offset + 3U] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::uint32_t s15_fixed16(const double value) {
  const auto fixed = static_cast<std::int32_t>(std::llround(value * 65536.0));
  return static_cast<std::uint32_t>(fixed);
}

void pad_four(Bytes& bytes) {
  while (bytes.size() % 4U != 0U) {
    bytes.push_back(0U);
  }
}

Bytes make_mluc(const std::string_view text) {
  Bytes bytes;
  append_u32(bytes, signature('m', 'l', 'u', 'c'));
  append_u32(bytes, 0U);
  append_u32(bytes, 1U);
  append_u32(bytes, 12U);
  append_u16(bytes, 0x656EU);  // en
  append_u16(bytes, 0x5553U);  // US
  append_u32(bytes, static_cast<std::uint32_t>(text.size() * 2U));
  append_u32(bytes, 28U);
  for (const char character : text) {
    append_u16(
        bytes,
        static_cast<std::uint16_t>(static_cast<unsigned char>(character)));
  }
  pad_four(bytes);
  return bytes;
}

Bytes make_xyz(const double x, const double y, const double z) {
  Bytes bytes;
  append_u32(bytes, signature('X', 'Y', 'Z', ' '));
  append_u32(bytes, 0U);
  append_u32(bytes, s15_fixed16(x));
  append_u32(bytes, s15_fixed16(y));
  append_u32(bytes, s15_fixed16(z));
  return bytes;
}

Bytes make_srgb_trc() {
  Bytes bytes;
  append_u32(bytes, signature('p', 'a', 'r', 'a'));
  append_u32(bytes, 0U);
  append_u16(bytes, 3U);
  append_u16(bytes, 0U);
  // ICC parametricCurveType 3 is the sRGB decoding curve:
  // (aX+b)^g for X >= d, cX otherwise.
  for (const double value : std::array{
           2.4,
           1.0 / 1.055,
           0.055 / 1.055,
           1.0 / 12.92,
           0.04045}) {
    append_u32(bytes, s15_fixed16(value));
  }
  return bytes;
}

Bytes make_chad() {
  Bytes bytes;
  append_u32(bytes, signature('s', 'f', '3', '2'));
  append_u32(bytes, 0U);
  // Bradford D65-to-D50 chromatic adaptation matrix.
  for (const double value : std::array{
           1.047882080078125, 0.022918701171875, -0.050201416015625,
           0.0295867919921875, 0.990478515625, -0.017059326171875,
           -0.0092315673828125, 0.01507568359375, 0.751678466796875}) {
    append_u32(bytes, s15_fixed16(value));
  }
  return bytes;
}

struct TagEntry {
  std::uint32_t tag_signature{};
  std::size_t payload_index{};
};

Bytes make_profile(const bool display_p3) {
  const std::array payloads{
      make_mluc(display_p3 ? "HDRShot Display P3" : "HDRShot BT.2020 sRGB"),
      make_mluc(display_p3
          ? "Generated from public Display P3 colorimetry"
          : "Generated from public BT.2020 and sRGB colorimetry"),
      make_xyz(0.9642, 1.0, 0.8249),
      display_p3
          ? make_xyz(0.5151214599609375, 0.2411956787109375, -0.0010528564453125)
          : make_xyz(0.6734771728515625, 0.2790374755859375, -0.0019378662109375),
      display_p3
          ? make_xyz(0.2919769287109375, 0.6922454833984375, 0.0418853759765625)
          : make_xyz(0.1656646728515625, 0.6753387451171875, 0.0299835205078125),
      display_p3
          ? make_xyz(0.1571044921875, 0.0665740966796875, 0.7840728759765625)
          : make_xyz(0.1250457763671875, 0.0456085205078125, 0.796844482421875),
      make_srgb_trc(),
      make_chad(),
  };
  constexpr std::array entries{
      TagEntry{signature('d', 'e', 's', 'c'), 0U},
      TagEntry{signature('c', 'p', 'r', 't'), 1U},
      TagEntry{signature('w', 't', 'p', 't'), 2U},
      TagEntry{signature('r', 'X', 'Y', 'Z'), 3U},
      TagEntry{signature('g', 'X', 'Y', 'Z'), 4U},
      TagEntry{signature('b', 'X', 'Y', 'Z'), 5U},
      TagEntry{signature('r', 'T', 'R', 'C'), 6U},
      TagEntry{signature('g', 'T', 'R', 'C'), 6U},
      TagEntry{signature('b', 'T', 'R', 'C'), 6U},
      TagEntry{signature('c', 'h', 'a', 'd'), 7U},
  };

  Bytes profile(128U + 4U + entries.size() * 12U, 0U);
  patch_u32(profile, 4U, signature('H', 'D', 'S', 'H'));
  patch_u32(profile, 8U, 0x04300000U);
  patch_u32(profile, 12U, signature('m', 'n', 't', 'r'));
  patch_u32(profile, 16U, signature('R', 'G', 'B', ' '));
  patch_u32(profile, 20U, signature('X', 'Y', 'Z', ' '));
  patch_u16(profile, 24U, 2026U);
  patch_u16(profile, 26U, 8U);
  patch_u16(profile, 28U, 29U);
  patch_u32(profile, 36U, signature('a', 'c', 's', 'p'));
  patch_u32(profile, 64U, 1U);  // media-relative colorimetric
  patch_u32(profile, 68U, s15_fixed16(0.9642));
  patch_u32(profile, 72U, s15_fixed16(1.0));
  patch_u32(profile, 76U, s15_fixed16(0.8249));
  patch_u32(profile, 80U, signature('H', 'D', 'S', 'H'));
  patch_u32(profile, 128U, static_cast<std::uint32_t>(entries.size()));

  std::array<std::uint32_t, payloads.size()> offsets{};
  for (std::size_t index = 0; index < payloads.size(); ++index) {
    offsets[index] = static_cast<std::uint32_t>(profile.size());
    profile.insert(profile.end(), payloads[index].begin(), payloads[index].end());
    pad_four(profile);
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto entry_offset = 132U + index * 12U;
    const auto payload_index = entries[index].payload_index;
    patch_u32(profile, entry_offset, entries[index].tag_signature);
    patch_u32(profile, entry_offset + 4U, offsets[payload_index]);
    patch_u32(
        profile,
        entry_offset + 8U,
        static_cast<std::uint32_t>(payloads[payload_index].size()));
  }
  patch_u32(profile, 0U, static_cast<std::uint32_t>(profile.size()));
  return profile;
}

}  // namespace

std::span<const std::uint8_t> Bt2020SrgbIccProfile::bytes() {
  static const auto profile = make_profile(false);
  return profile;
}

}  // namespace hdrshot
