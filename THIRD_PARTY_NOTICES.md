# Third-party notices

最后更新：2026-09-20 23:11:25 CST

SeriousShot currently uses the following third-party components.
Release bundles must include this notice and the referenced license texts.

This source snapshot does not bundle the downloaded Qt/codec SDKs or grant a
project-wide license. Binary distribution remains a separate packaging and
compliance step; the following list is not a substitute for the full notices
of the exact binaries shipped.

## Qt and bundled FreeType

- The current host build uses Qt 6.11.1 Widgets/Core/Gui and Qt's bundled
  FreeType. Qt provides multiple licensing options; consult the actual SDK's
  license texts and SBOM as well as [Qt licensing](https://doc.qt.io/qt-6/licensing.html).
- The development host dynamically links Qt. Distribution must include the
  applicable Qt and third-party notices and satisfy the selected license's
  obligations; dynamic linkage alone is not a compliance claim.
- FreeType is available under the FreeType License or GPLv2; retain the
  applicable license/attribution from the SDK. See [FreeType licenses](https://freetype.org/license.html).

## zlib

PNG encoding links the zlib selected by CMake (`find_package(ZLIB)`); the
Windows development setup currently uses 1.3.1, while macOS may use the system
SDK. Preserve the matching distribution's notice. See [zlib license](https://zlib.net/zlib_license.html).

## libultrahdr

- Project: [Google libultrahdr](https://github.com/google/libultrahdr)
- Version: v2.0.2, commit `e5f5a022fe96fc4dc2ee35c19f733a50df807abe`
- Copyright: 2022 The Android Open Source Project
- License: dual MIT and Apache License 2.0
- Required notice: “This product includes Gain Map technology under license by Adobe.”

The build copies libultrahdr's combined `LICENSE` and Adobe gain-map `NOTICE`
from the pinned source tree into `SeriousShot.app/Contents/Resources/licenses/`.

Local modification: `cmake/libultrahdr_p3_alternate.cmake` generates a modified
`lib/src/jpegr.cpp` and `lib/src/icc.cpp` at build time. It retains the alternate
ICC with ISO metadata (including the historical ISO+XMP path), selects upstream P3/PQ for the alternate intent, fixes an
inverted B-curve write error check, and serializes canonical Display P3 matrix
parameters with the equivalent sRGB parametric type-3 curve for the base.
The upstream descriptions and copyright are retained. No Apple ICC or
image-specific Adaptive Gain Curve is bundled. No source-pixel, gain-map or
image tone-map algorithm is changed; the PQ ICC LUT is now actually written.

Additional ICC correction (2026-09-06): in that same SHA-checked generated
`icc.cpp`, A2B0 CLUT construction uses `pqInvOetf` (PQ EOTF) instead of
`pqOetf`. It retains the subsequent ICC tone mapping, RGB-to-PCS conversion,
17-cubed grid and 16-bit serialization. This is correct PQ decoding plus the
existing SDR rendering, NOT a pure HDR/passthrough ICC. The fix is unconditional
for macOS and Windows; downloaded upstream sources remain unmodified.

Additional local modification: `cmake/libultrahdr_gainmap.cmake` removes the
HDR-only API's unconditional realtime-preset override in the same SHA-checked
generated `jpegr.cpp`. The shared adapter selects upstream BEST_QUALITY,
three-channel gain maps and both ISO 21496-1 and XMP metadata, preserving upstream two-pass
Min/Max, Gamma/Offset and tone-map mathematics. No fetched source is modified.

Additional local modification: `cmake/libultrahdr_ycbcr.cmake` generates a
modified `lib/src/dsp/arm/gainmapmath_neon.cpp`. It fixes swapped gamut cases
and uses BT.601 JPEG coefficients for Display P3, matching the scalar encoder
and decoder. SIMD and 4:4:4 remain enabled. This changes SDR base encoding,
not RGB primaries, input brightness, tone mapping, or gain-map mathematics.
The generated source retains the upstream dual-license header.

## libjpeg-turbo

- Project: [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)
- Version: 3.1.0, commit `20ade4dea9589515a69793e447a6c6220b464535`
- License: BSD-style, IJG and zlib licenses as enumerated in upstream
  `LICENSE.md`

The build copies that complete license file into the release bundle. This is a
static transitive dependency of libultrahdr.

## Compact ICC Profiles — DisplayP3-v4.icc

- Project: [Compact ICC Profiles](https://github.com/saucecontrol/Compact-ICC-Profiles)
- Commit: `bdd84663061bc4ae95ca70decff54f581e27f702`
- License: CC0 1.0 Universal
- Local notice and license: `assets/color_profiles/README.md` and
  `assets/color_profiles/CC0-1.0.txt`

## Noto Sans SC

- License: SIL Open Font License 1.1
- Local notice and license: `assets/fonts/README.md` and
  `assets/fonts/OFL.txt`
