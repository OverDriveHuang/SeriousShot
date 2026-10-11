#pragma once
#include "domain/analysis/defaults.hpp"
#include "domain/capture/magnifier_readout.hpp"
#include <QStringList>
#include <vector>
namespace hdrshot {
inline const QStringList &magnifier_reading_names() {
  static const QStringList n{"源线性 RGB / EDR · P3",
                             "Y′R′G′B′ / P3-sRGB（SDR）",
                             "Y′R′G′B′ / P3-PQ",
                             "线性 YRGB / nit · P3",
                             "L*a*b* · D65（SDR）",
                             "ITP",
                             "ICtCp",
                             "Hue / Chroma · a*b*（SDR）",
                             "Hue / Chroma · TP"};
  return n;
}
inline const QStringList &analyzer_reading_names() {
  static const QStringList n{"源线性 RGB / EDR · P3",   "工作 R′G′B′ / code",
                             "线性 YRGB / nit",         "Y′",
                             "L*a*b* · D65 / ITP",      "L*a*b* · D65 / ICtCp",
                             "Hue / Chroma · a*b* / TP"};
  return n;
}
inline QString reading_number(double value, int precision = 4,
                              bool valid = true) {
  if (!valid || !std::isfinite(value))
    return "--";
  auto s = QString::number(value, 'f', precision);
  return s.size() > 12 ? QString::number(value, 'e', 4) : s;
}
struct AnalysisScalarRow {
  QString name, value;
  bool heading{};
};
inline std::vector<AnalysisScalarRow>
magnifier_scalar_rows(const MagnifierReadout &r,
                      const MagnifierReadings &fields) {
  std::vector<AnalysisScalarRow> rows;
  auto add = [&](QString name, double v, bool ok, int precision = 4) {
    rows.push_back({name, reading_number(v, precision, r.valid && ok)});
  };
  auto triple = [&](QString prefix, const analysis::Rgb &v,
                    QStringList components, bool ok, int precision = 4) {
    for (int i = 0; i < 3; ++i)
      add(prefix + components[i], v[i], ok, precision);
  };
  const auto &f = fields.selected;
  auto group = [&](int i) {
    rows.push_back({magnifier_reading_names()[i], {}, true});
  };
  if (f[0]) {
    group(0);
    triple("", r.source, {"R", "G", "B"}, true);
  }
  if (f[1]) {
    group(1);
    add("Y′", r.srgb_y, r.sdr);
    triple("", r.srgb, {"R′", "G′", "B′"}, r.sdr);
  }
  if (f[2]) {
    group(2);
    add("Y′", r.pq_y, true);
    triple("", r.pq, {"R′", "G′", "B′"}, true);
  }
  if (f[3]) {
    group(3);
    add("Y", r.y_nits, true, 2);
    triple("", r.nits, {"R", "G", "B"}, true, 2);
  }
  if (f[4]) {
    group(4);
    triple("", r.lab, {"L*", "a*", "b*"}, r.sdr, 2);
  }
  if (f[5]) {
    group(5);
    triple("", r.itp, {"I", "T", "P"}, r.itp_valid);
  }
  if (f[6]) {
    group(6);
    triple("", {r.itp[0], 2 * r.itp[1], r.itp[2]}, {"I", "Ct", "Cp"},
           r.itp_valid);
  }
  auto hc = [&](QString plane, std::optional<double> hue, double chroma,
                bool ok) {
    rows.push_back({"Hue " + plane + " · °",
                    r.valid && ok
                        ? (hue ? reading_number(*hue, 1) : QString("—"))
                        : QString("--")});
    add("Chroma " + plane, chroma, ok);
  };
  if (f[7])
    hc("a*b*", r.lab_hue, r.lab_chroma, r.sdr);
  if (f[8])
    hc("TP", r.tp_hue, r.tp_chroma, r.itp_valid);
  return rows;
}
inline QStringList analyzer_reading_rows(const analysis::Readout &r,
                                         const AnalyzerReadings &fields,
                                         analysis::WorkingSpace space,
                                         bool fallback = false) {
  QStringList rows;
  const auto &f = fields.selected;
  const bool hdr = analysis::is_hdr(space);
  auto num = [&](double v, int precision = 4) {
    return reading_number(v, precision, r.valid_count > 0);
  };
  auto triple = [&](const analysis::Rgb &v, int precision = 4) {
    return num(v[0], precision) + " / " + num(v[1], precision) + " / " +
           num(v[2], precision);
  };
  if (f[0])
    rows << "源线性 RGB / EDR · P3   " + triple(r.source_rgb_edr);
  if (f[1])
    rows << QString("工作 R′G′B′ · %1   ")
                    .arg(analysis_math_double::working_transfer(
                             unsigned(space)) == 1
                             ? "PQ"
                             : "sRGB") +
                triple(r.signal_rgb);
  if (f[2] || (fallback &&
               std::none_of(f.begin(), f.end(), [](bool b) { return b; }))) {
    rows << "Y · nit   " + num(r.y_nits, 2);
    rows << "R / G / B · nit   " + triple(r.work_rgb_nits, 2);
  }
  if (f[3])
    rows << "Y′   " + num(r.encoded_luma);
  if (!hdr && (f[4] || f[5]))
    rows << "L*a*b* · D65   " + triple(r.perceptual, 2);
  if (hdr && f[4])
    rows << "ITP I / T / P   " + triple(r.perceptual);
  if (hdr && f[5])
    rows << "ICtCp I / Ct / Cp   " +
                triple({r.perceptual[0], 2 * r.perceptual[1], r.perceptual[2]});
  if (f[6])
    rows << QString("Hue / Chroma · %1   ").arg(hdr ? "TP" : "a*b*") +
                (r.valid_count ? (r.hue_degrees ? num(*r.hue_degrees, 1) + "°"
                                                : QString("—"))
                               : QString("--")) +
                " / " + num(r.chroma);
  return rows;
}
} // namespace hdrshot
