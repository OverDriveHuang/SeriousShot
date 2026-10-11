#pragma once
#include "domain/analysis/defaults.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
namespace hdrshot {
template <std::size_t N>
QJsonArray readings_json(const std::array<bool, N> &flags) {
  QJsonArray a;
  for (bool b : flags)
    a.append(b);
  return a;
}
template <std::size_t N>
void readings_from_json(const QJsonValue &v, std::array<bool, N> &flags) {
  const auto a = v.toArray();
  if (a.size() != int(N))
    return;
  for (int i = 0; i < int(N); ++i)
    if (!a[i].isBool())
      return;
  for (int i = 0; i < int(N); ++i)
    flags[i] = a[i].toBool();
}
inline QString analysis_defaults_json(const AnalysisDefaults &d) {
  return QString::fromUtf8(
      QJsonDocument(
          QJsonObject{{"version", 2},
                      {"enabled", d.magnifier_enabled},
                      {"zoom", d.magnifier_zoom},
                      {"hdrSpace", int(d.hdr_space)},
                      {"sdrSpace", int(d.sdr_space)},
                      {"white", d.reference_white_nits},
                      {"blur", d.blur_sigma_px},
                      {"sampleSize", d.sample_side},
                      {"magnifier", readings_json(d.magnifier.selected)},
                      {"analyzer", readings_json(d.analyzer.selected)}})
          .toJson(QJsonDocument::Compact));
}
inline AnalysisDefaults analysis_defaults_from_json(const QString &json,
                                                    const QString & = {}) {
  AnalysisDefaults d;
  const auto o = QJsonDocument::fromJson(json.toUtf8()).object();
  // Neither legacy window usage nor unpublished four-bank drafts are defaults.
  if (o.value("version").toInt() != 2)
    return d;
  d.magnifier_enabled = o.value("enabled").toBool(true);
  d.magnifier_zoom = o.value("zoom").toInt(8);
  d.hdr_space = analysis::WorkingSpace(o.value("hdrSpace").toInt(2));
  d.sdr_space = analysis::WorkingSpace(o.value("sdrSpace").toInt(1));
  d.reference_white_nits = o.value("white").toDouble(203);
  d.blur_sigma_px = o.value("blur").toDouble(0);
  d.sample_side = o.value("sampleSize").toInt(1);
  readings_from_json(o.value("magnifier"), d.magnifier.selected);
  readings_from_json(o.value("analyzer"), d.analyzer.selected);
  const AnalysisDefaults base;
  const std::array zooms{4, 8, 12, 16};
  const std::array sides{1, 3, 5, 11, 31, 101};
  const std::array sigmas{0., .5, 1., 2., 4., 8.};
  if (std::find(zooms.begin(), zooms.end(), d.magnifier_zoom) == zooms.end())
    d.magnifier_zoom = 8;
  if (std::find(sides.begin(), sides.end(), d.sample_side) == sides.end())
    d.sample_side = 1;
  if (std::find(sigmas.begin(), sigmas.end(), d.blur_sigma_px) == sigmas.end())
    d.blur_sigma_px = 0;
  if (d.reference_white_nits != 100 && d.reference_white_nits != 203)
    d.reference_white_nits = 203;
  if (d.hdr_space != base.hdr_space &&
      d.hdr_space != analysis::WorkingSpace::bt2020_pq)
    d.hdr_space = base.hdr_space;
  if (d.sdr_space != base.sdr_space &&
      d.sdr_space != analysis::WorkingSpace::srgb_sdr)
    d.sdr_space = base.sdr_space;
  return d;
}
} // namespace hdrshot
