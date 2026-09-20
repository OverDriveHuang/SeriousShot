#include "ui/qt/analyzer_scope_plot.hpp"
#include "domain/analysis/math.hpp"
#include "domain/analysis/gamut_boundary.hpp"
#include "test_support.hpp"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QWheelEvent>
#include <algorithm>
#include <limits>

namespace {
using namespace hdrshot;
void events() { QApplication::processEvents(); }
analysis::ResultRef wave_result() {
  auto r = std::make_shared<analysis::ResultData>();
  r->settings.working_space = analysis::WorkingSpace::display_p3_sdr;
  r->valid_count = 128;
  auto &g = r->waveform[0];
  g.width = 128;
  g.height = 128;
  g.maximum = 1;
  g.counts.resize(128 * 128);
  for (unsigned x = 0; x < 128; ++x)
    g.counts[38 * 128 + x] = 1;
  return r;
}
void show(AnalyzerScopePlot &p, analysis::ResultRef r) {
  p.resize(720, 400);
  p.show();
  p.set_options(r->scopes);
  p.set_result(r);
  p.set_pending(false);
  events();
}
QImage render(AnalyzerScopePlot &p) {
  events();
  return p.grab().toImage().convertToFormat(QImage::Format_ARGB32);
}
double trace_y(AnalyzerScopePlot &p) {
  auto image = render(p);
  const double dpr = p.devicePixelRatioF();
  const auto rect = p.plot_rect();
  const int x = int(rect.center().x() * dpr);
  double sum = 0, weight = 0;
  for (int y = int(rect.top() * dpr); y < int(rect.bottom() * dpr); ++y) {
    const QColor c = image.pixelColor(x, y);
    if (c.red() > 100 && c.green() > 100 && c.blue() > 100) {
      sum += double(y) * c.red();
      weight += c.red();
    }
  }
  HDRSHOT_CHECK(weight > 0);
  return sum / weight / dpr;
}
void tick_collision() {
  std::vector<analysis::AxisTick> ticks{
      {50, .5, "50%", true, false}, {50.04, .501, "50.04", false, false},
      {40, .25, "40", false, false}, {60, .75, "60", false, false}};
  auto selected = analyzer_axis_label_ticks(ticks, 300, 14);
  HDRSHOT_CHECK(selected.size() == 3);
  HDRSHOT_CHECK(std::any_of(selected.begin(), selected.end(),
                            [](auto &t) { return t.label == "50%"; }));
  HDRSHOT_CHECK(std::none_of(selected.begin(), selected.end(),
                             [](auto &t) { return t.label == "50.04"; }));
  for (double height : {30., 50., 100., 300.}) {
    auto labels = analyzer_axis_label_ticks(ticks, height, 14);
    for (std::size_t i = 0; i < labels.size(); ++i)
      for (std::size_t j = i + 1; j < labels.size(); ++j)
        HDRSHOT_CHECK(std::abs(labels[i].position - labels[j].position) * height >= 17.);
  }
}
void pending_keeps_trace() {
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::waveform);
  show(p, wave_result());
  auto before = render(p);
  p.set_pending(true);
  HDRSHOT_CHECK(render(p) == before);
}
void full_domain_pan_and_zoom() {
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::waveform);
  auto result = wave_result();
  show(p, result);
  const auto rect = p.plot_rect();
  const double old_y = trace_y(p);
  auto options = result->scopes;
  options.amplitude_view = {2., .1};
  p.set_options(options);
  const double expected = rect.bottom() -
      ((rect.bottom() - old_y) / rect.height() - .1) * 2. * rect.height();
  HDRSHOT_CHECK_NEAR(trace_y(p), expected, 2.);
  options.amplitude_view.pan = .2;
  p.set_options(options);
  HDRSHOT_CHECK_NEAR(trace_y(p), expected + .2 * rect.height(), 2.);
  HDRSHOT_CHECK(result->scopes.amplitude_view.zoom == 1.);
}
void trackpad_pan_and_native_pinch() {
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::waveform);
  auto result = wave_result();
  show(p, result);
  auto options = result->scopes;
  options.amplitude_view = {3., .2};
  p.set_options(options);
  auto observed = options;
  p.options_changed = [&](const auto &next) { observed = next; };
  QPointF local = p.plot_rect().center();
  QWheelEvent scroll(local, QPointF(p.mapToGlobal(local.toPoint())), QPoint(0, 15),
                      QPoint(), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
  QApplication::sendEvent(&p, &scroll);
  HDRSHOT_CHECK_NEAR(observed.amplitude_view.zoom, 3., 1e-9);
  HDRSHOT_CHECK(observed.amplitude_view.pan > .2);
  QNativeGestureEvent pinch(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(),
                            2, local, local, local, .1, QPointF(), 1);
  QApplication::sendEvent(&p, &pinch);
  HDRSHOT_CHECK_NEAR(observed.amplitude_view.zoom, 3.3, 1e-9);
  QWheelEvent notch(local, local, QPoint(), QPoint(0, 120), Qt::NoButton,
                     Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&p, &notch);
  HDRSHOT_CHECK(observed.amplitude_view.zoom > 3.3);
}
void gain_only_changes_scope_presentation() {
  auto r = std::make_shared<analysis::ResultData>();
  r->settings.working_space = analysis::WorkingSpace::display_p3_pq;
  r->valid_count = 1025;
  r->vector_grid_x_extent = 1.;
  r->vector_grid_y_extent = 1.;
  auto &g = r->vectorscope;
  g.width = g.height = 100;
  g.counts.resize(10000);
  g.counts[60 * 100 + 55] = 1;
  g.counts[90 * 100 + 90] = 1024;
  g.maximum = 1024;
  for (auto &wave : r->waveform) wave = g;
  for (bool detail : {false, true}) {
    if (detail) {
      r->waveform_detail = r->waveform;
      r->vectorscope_detail = g;
      r->vector_detail_x_extent = r->vector_detail_y_extent = 1.;
    }
    for (auto kind : {AnalyzerScopePlot::Kind::vectorscope, AnalyzerScopePlot::Kind::waveform})
      for (auto mode : {analysis::WaveMode::intensity, analysis::WaveMode::rgb,
                        analysis::WaveMode::parade_rgb, analysis::WaveMode::parade_intensity_rgb}) {
        AnalyzerScopePlot p(kind);
        r->scopes.wave_mode = mode;
        show(p, r);
        const auto baseline = render(p);
        p.set_display_gain(.1);
        auto dim = render(p);
        p.set_display_gain(3.);
        auto bright = render(p);
        HDRSHOT_CHECK(dim != bright);
        HDRSHOT_CHECK(g.counts[60 * 100 + 55] == 1);
        HDRSHOT_CHECK(g.maximum == 1024);
        p.set_display_gain(.09);
        HDRSHOT_CHECK(p.display_gain() == 3.);
        p.set_display_gain(std::numeric_limits<double>::quiet_NaN());
        HDRSHOT_CHECK(p.display_gain() == 3.);
        p.set_display_gain(1.);
        HDRSHOT_CHECK(render(p) == baseline);
      }
  }
  for (auto &h : r->histograms) {
    h.counts.resize(8);
    h.counts[2] = 100;
    h.maximum = 100;
  }
  AnalyzerScopePlot h(AnalyzerScopePlot::Kind::histogram);
  show(h, r);
  const auto histogram = render(h);
  h.set_display_gain(.1);
  HDRSHOT_CHECK(render(h) == histogram);
  h.set_display_gain(3.);
  HDRSHOT_CHECK(render(h) == histogram);
}
void vector_reference_lines_stop_at_nominal_domain() {
  const auto dir = qEnvironmentVariable("HDRSHOT_ANALYZER_SCOPE_ARTIFACTS");
  if (!dir.isEmpty()) HDRSHOT_CHECK(QDir().mkpath(dir));
  for (auto mode : {analysis::VectorMode::perceptual, analysis::VectorMode::ycbcr}) {
    auto r = std::make_shared<analysis::ResultData>();
    r->settings.working_space = analysis::WorkingSpace::display_p3_pq;
    r->scopes.vector_mode = mode;
    r->valid_count = 1;
    AnalyzerScopePlot p(AnalyzerScopePlot::Kind::vectorscope);
    show(p, r);
    for (double zoom : {.5, 1., 2.}) {
      auto options = r->scopes;
      options.vector_zoom = zoom;
      p.set_options(options);
      p.resize(2085, 340);
      const auto rect = p.plot_rect();
      const auto c = analysis::vector_calibration(r->settings, mode,
          std::uint32_t(rect.width()), std::uint32_t(rect.height()), zoom);
      HDRSHOT_CHECK(c.reference_extent.has_value());
      const auto boundary = analysis::project_vector(*c.reference_extent, c, zoom);
      const double half_w = (boundary[0] - .5) * rect.width();
      const double half_h = (boundary[1] - .5) * rect.height();
      const auto img = render(p);
      const auto dpr = p.devicePixelRatioF();
      // Empty pixels beyond the nominal axes, including no skin-line or tick
      // continuation. Allow room for labels anchored to the actual endpoints.
      for (int y = int(rect.top() + 1); y < int(rect.bottom() - 1); ++y)
        for (int x = int(rect.left() + 1); x < int(rect.right() - 1); ++x) {
          if (std::abs(x - rect.center().x()) <= half_w + 80 &&
              std::abs(y - rect.center().y()) <= half_h + 40) continue;
          const auto color = img.pixelColor(int(x * dpr), int(y * dpr));
          HDRSHOT_CHECK(color.red() <= 6 && color.green() <= 6 && color.blue() <= 6);
        }
      if (!dir.isEmpty()) {
        const auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
        HDRSHOT_CHECK(img.save(dir + "/" + stamp + (mode == analysis::VectorMode::ycbcr ? "_ycbcr_" : "_itp_") + QString::number(zoom) + "x.png"));
      }
    }
  }
}
void full_columns_remain_visible_after_resample() {
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::waveform);
  show(p, wave_result());
  for (int width : {511, 720, 1001}) {
    p.resize(width, 400);
    const double y = trace_y(p);
    const auto img = render(p);
    const auto r = p.plot_rect();
    const double dpr = p.devicePixelRatioF();
    for (int x = int((r.left() + 2) * dpr); x < int((r.right() - 2) * dpr); ++x) {
      int peak = 0;
      for (int dy = -2; dy <= 2; ++dy)
        peak = std::max(peak, img.pixelColor(x, int(y * dpr) + dy).red());
      HDRSHOT_CHECK(peak > 100);
    }
  }
}
void gamut_outline_uses_current_space_and_trace_transform() {
  const auto dir = qEnvironmentVariable("HDRSHOT_ANALYZER_SCOPE_ARTIFACTS");
  if (!dir.isEmpty()) HDRSHOT_CHECK(QDir().mkpath(dir));
  std::array<QImage, 4> images;
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::vectorscope);
  auto r = std::make_shared<analysis::ResultData>();
  r->valid_count = 1;
  show(p, r);
  for (unsigned space = 0; space < 4; ++space) {
    // Fresh immutable result on the SAME widget tests invalidation on switching.
    auto next = std::make_shared<analysis::ResultData>(*r);
    next->settings.working_space = analysis::WorkingSpace(space);
    p.set_result(next);
    for (auto size : {QSize(720, 480), QSize(1350, 260), QSize(340, 760)})
      for (double zoom : {.5, 1., 2., 8.}) {
        p.resize(size);
        auto options = next->scopes;
        options.vector_zoom = zoom;
        p.set_options(options);
        const auto image = render(p);
        const auto rect = p.plot_rect();
        const auto dpr = p.devicePixelRatioF();
        const auto c = analysis::vector_calibration(next->settings,
            analysis::VectorMode::perceptual, std::uint32_t(rect.width() * dpr),
            std::uint32_t(rect.height() * dpr), zoom);
        int visible = 0, hits = 0;
        const auto boundary = analysis::perceptual_gamut_boundary(next->settings.working_space);
        for (std::size_t i = 0; i < boundary.size(); i += 7) {
          const auto point = analysis::project_vector({boundary[i][0], boundary[i][1]}, c, zoom);
          QPointF pt(rect.x() + rect.width() * point[0], rect.bottom() - rect.height() * point[1]);
          if (!rect.adjusted(4, 4, -4, -4).contains(pt)) continue;
          // Exclude calibration axes and text; test the actual curved outline.
          if (std::abs(pt.x() - rect.center().x()) < 45 ||
              std::abs(pt.y() - rect.center().y()) < 28) continue;
          ++visible;
          bool lit = false;
          for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x) {
              auto pixel = image.pixelColor(int((pt.x() + x) * dpr), int((pt.y() + y) * dpr));
              lit |= pixel.red() > 35 && std::abs(pixel.red() - pixel.blue()) < 12;
            }
          hits += lit ? 1 : 0;
        }
        if (visible > 10) HDRSHOT_CHECK(hits > visible / 3); // dashed gaps retained
        if (size == QSize(720, 480) && zoom == 1.) {
          HDRSHOT_CHECK(visible > 30);
          images[space] = image;
        }
        if (!dir.isEmpty() && (zoom <= 2. || size == QSize(720, 480))) {
          const auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
          HDRSHOT_CHECK(image.save(dir + "/" + stamp + "_gamut_" + QString::number(space) +
              "_" + QString::number(size.width()) + "x" + QString::number(size.height()) +
              "_" + QString::number(zoom) + "x.png"));
        }
        p.set_display_gain(3.);
        HDRSHOT_CHECK(render(p) == image); // Gain cannot alter empty-scope guides.
        auto white = std::make_shared<analysis::ResultData>(*next);
        white->settings.reference_white_nits = 100;
        p.set_result(white);
        HDRSHOT_CHECK(render(p) == image);
        p.set_result(next);
        p.set_display_gain(1.);
      }
  }
  HDRSHOT_CHECK(images[0] != images[1]);
  HDRSHOT_CHECK(images[2] != images[3]);
}
void uneven_column_coverage_is_display_normalized() {
  auto r = std::make_shared<analysis::ResultData>(*wave_result());
  auto &g = r->waveform[0];
  g.maximum = 2;
  g.maximum_density = 1;
  g.column_coverage.resize(g.width);
  for (unsigned x = 0; x < g.width; ++x) {
    const unsigned columns = 1 + x % 2;
    g.column_coverage[x] = float(columns);
    g.counts[38 * g.width + x] = columns;
  }
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::waveform);
  show(p, r);
  const auto normalized = render(p);
  p.set_result(wave_result());
  HDRSHOT_CHECK(render(p) == normalized);
  HDRSHOT_CHECK(g.counts[38 * g.width] == 1 && g.counts[38 * g.width + 1] == 2);
}
void detail_tile_replaces_base_and_moves_with_domain() {
  auto r = std::make_shared<analysis::ResultData>(*wave_result());
  auto &fine = r->waveform_detail[0];
  fine.width = fine.height = 128;
  fine.maximum = 1;
  fine.counts.resize(128 * 128);
  for (unsigned x = 0; x < fine.width; ++x)
    fine.counts[70 * fine.width + x] = 1;
  r->waveform_detail_view = {8, .25};
  r->scopes.amplitude_view = r->waveform_detail_view;
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::waveform);
  show(p, r);
  const auto rect = p.plot_rect();
  const double expected = rect.bottom() - (70.5 / 128.) * rect.height();
  HDRSHOT_CHECK_NEAR(trace_y(p), expected, 2.);
  // Same detail tile remains usable immediately after a pan. No new result.
  auto options = r->scopes;
  options.amplitude_view.pan += .02;
  p.set_options(options);
  HDRSHOT_CHECK_NEAR(trace_y(p), expected + .02 * 8 * rect.height(), 2.);
  // Fine-only completion refreshes its image even when base stats are shared.
  auto refreshed = std::make_shared<analysis::ResultData>(*r);
  refreshed->waveform_detail[0].counts.clear();
  refreshed->waveform_detail[0].counts.resize(128 * 128);
  for (unsigned x = 0; x < 128; ++x)
    refreshed->waveform_detail[0].counts[90 * 128 + x] = 1;
  p.set_result(refreshed, false);
  HDRSHOT_CHECK_NEAR(trace_y(p), rect.bottom() - (90.5 / 128. - .16) * rect.height(), 2.);
}
void capped_vector_detail_covers_real_viewport() {
  const auto solid = [](std::array<float, 3> color) {
    analysis::DensityGrid grid;
    grid.width = grid.height = 8;
    grid.maximum = 1;
    grid.counts.resize(64);
    grid.color_sums.resize(64);
    for (std::size_t i = 0; i < 64; ++i) {
      grid.counts[i] = 1;
      grid.color_sums[i] = color;
    }
    return grid;
  };
  for (auto space : {analysis::WorkingSpace::display_p3_pq,
                     analysis::WorkingSpace::display_p3_sdr})
    for (auto mode : {analysis::VectorMode::perceptual, analysis::VectorMode::ycbcr})
      for (auto size : {QSize(2400, 300), QSize(300, 2400), QSize(2400, 1600)}) {
        auto result = std::make_shared<analysis::ResultData>();
        result->settings.working_space = space;
        result->scopes.vector_mode = mode;
        result->scopes.vector_zoom = 20.;
        result->valid_count = 64;
        result->vectorscope = solid({1, 0, 0}); // Red means coarse data leaked through.
        result->vectorscope_detail = solid({0, 1, 0});
        result->vector_grid_x_extent = result->vector_grid_y_extent = 1000.;
        AnalyzerScopePlot plot(AnalyzerScopePlot::Kind::vectorscope);
        show(plot, result);
        plot.resize(size);
        events();
        const auto rect = plot.plot_rect();
        const double dpr = plot.devicePixelRatioF();
        analysis::Request request;
        request.settings = result->settings;
        request.scopes = result->scopes;
        request.scopes.vector_viewport_width = std::uint32_t(std::ceil(rect.width() * dpr));
        request.scopes.vector_viewport_height = std::uint32_t(std::ceil(rect.height() * dpr));
        request.scopes.vector_detail_width = std::min(2048u, request.scopes.vector_viewport_width);
        request.scopes.vector_detail_height = std::min(2048u, request.scopes.vector_viewport_height);
        HDRSHOT_CHECK(request.scopes.vector_viewport_width > 2048 ||
                      request.scopes.vector_viewport_height > 2048);
        const auto extents = analysis::vector_detail_extents(request);
        result->vector_detail_x_extent = extents[0];
        result->vector_detail_y_extent = extents[1];
        plot.set_result(result);
        const auto red_fraction = [&] {
          const auto img = render(plot);
          int red = 0, total = 0;
          for (int y = 5; y < int(rect.height()) - 5; y += 7)
            for (int x = 5; x < int(rect.width()) - 5; x += 7) {
              const auto c = img.pixelColor(int((rect.left()+x)*dpr),
                                            int((rect.top()+y)*dpr));
              red += c.red() > 200 && c.green() < 30;
              ++total;
            }
          return double(red) / total;
        };
        HDRSHOT_CHECK(red_fraction() < .001);
        // Negative control: the original independently capped aspect must fail
        // this pixel oracle, proving it detects partially replaced textures.
        auto broken = std::make_shared<analysis::ResultData>(*result);
        request.scopes.vector_viewport_width = request.scopes.vector_viewport_height = 0;
        const auto old = analysis::vector_detail_extents(request);
        broken->vector_detail_x_extent = old[0];
        broken->vector_detail_y_extent = old[1];
        plot.set_result(broken);
        HDRSHOT_CHECK(red_fraction() > .01);
      }
}
void vector_trackpad_pan_keeps_tiles_and_fit() {
  for (double zoom : {4., 5., 10., 20.}) {
    auto r = std::make_shared<analysis::ResultData>();
    r->valid_count = 64;
    r->settings.working_space = analysis::WorkingSpace::display_p3_pq;
    r->scopes.vector_zoom = zoom;
    r->vector_grid_x_extent = r->vector_grid_y_extent = 100;
    auto &base = r->vectorscope;
    base.width = base.height = 8; base.maximum = 1;
    base.counts.resize(64); base.color_sums.resize(64);
    for (unsigned i = 0; i < 64; ++i) {base.counts[i] = 1; base.color_sums[i] = {1, 0, 0};}
    r->vectorscope_detail = base;
    for (auto &c : r->vectorscope_detail.color_sums) c = {0, 1, 0};
    AnalyzerScopePlot p(AnalyzerScopePlot::Kind::vectorscope);
    show(p, r);
    const auto rect = p.plot_rect();
    const auto calibration = analysis::vector_calibration(r->settings, r->scopes.vector_mode,
        unsigned(rect.width()), unsigned(rect.height()), zoom);
    r->vector_detail_x_extent = calibration.x_extent / zoom;
    r->vector_detail_y_extent = calibration.y_extent / zoom;
    p.set_result(r);
    auto observed = r->scopes;
    p.options_changed = [&](const auto &o) { observed = o; };
    const QPointF local = rect.center();
    const QPoint delta(55, -30);
    QWheelEvent scroll(local, local, delta, {}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(&p, &scroll);
    HDRSHOT_CHECK(observed.vector_zoom == zoom);
    HDRSHOT_CHECK_NEAR(observed.vector_pan[0], -55 * 2 * calibration.x_extent / (rect.width() * zoom), 1e-12);
    HDRSHOT_CHECK_NEAR(observed.vector_pan[1], -30 * 2 * calibration.y_extent / (rect.height() * zoom), 1e-12);
    const auto pixel = [&](QPointF pt) {const auto image = render(p); return image.pixelColor(int(pt.x()*p.devicePixelRatioF()), int(pt.y()*p.devicePixelRatioF()));};
    // Retained detail moves by the gesture; newly exposed left strip uses base.
    const QPointF strip(rect.left()+20, rect.top()+rect.height()*.65);
    HDRSHOT_CHECK(pixel(strip).red() > pixel(strip).green()+100);
    HDRSHOT_CHECK(pixel(local+QPointF(70, 50)).green() > 150);
    auto refined = std::make_shared<analysis::ResultData>(*r);
    refined->vector_detail_center = observed.vector_pan;
    p.set_result(refined, false);
    HDRSHOT_CHECK(pixel(strip).green() > pixel(strip).red()+100);
    // Native pan and wheel translation share exactly the same domain mapping.
    QNativeGestureEvent native(Qt::PanNativeGesture, QPointingDevice::primaryPointingDevice(),
        2, local, local, local, 0, QPointF(-55, 30), 1);
    QApplication::sendEvent(&p, &native);
    HDRSHOT_CHECK_NEAR(observed.vector_pan[0], 0, 1e-12);
    HDRSHOT_CHECK_NEAR(observed.vector_pan[1], 0, 1e-12);
    p.fit();
    HDRSHOT_CHECK(observed.vector_zoom == 1.);
    HDRSHOT_CHECK((observed.vector_pan == std::array<double, 2>{}));
  }
}
void artifact() {
  const auto dir = qEnvironmentVariable("HDRSHOT_ANALYZER_SCOPE_ARTIFACTS");
  if (dir.isEmpty()) return;
  HDRSHOT_CHECK(QDir().mkpath(dir));
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::waveform);
  auto r = std::make_shared<analysis::ResultData>(*wave_result());
  r->waveform[0].counts.clear();
  r->waveform[0].counts.resize(128 * 128);
  for (unsigned x = 0; x < 128; ++x)
    r->waveform[0].counts[(56 + x / 16) * 128 + x] = 1;
  show(p, r);
  auto options = r->scopes;
  options.amplitude_view = {4.962, .365};
  p.set_options(options);
  p.set_pending(true);
  const auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
  HDRSHOT_CHECK(render(p).save(dir + "/" + stamp + "_wave_zoom_ticks.png"));
}
void picker_distribution_is_bounded_and_clears() {
  auto r = std::make_shared<analysis::ResultData>(*wave_result());
  auto &hist = r->histograms[0]; hist.counts.resize(1024); hist.maximum = 100;
  for (unsigned i=100;i<800;++i) hist.counts[i] = 40;
  analysis::SampleResult sample;
  sample.request = {0, 100, 100, 101, true}; sample.mean.valid_count=10201;
  sample.mean_position.histogram={.85,.85,.85};
  for (unsigned i=0;i<10201;++i) {
    analysis::ProjectedSample point;
    const double value=.2+.4*double(i%101)/100.;
    point.histogram={value,value,value}; point.hue_valid=true;
    sample.distribution.push_back(point);
  }
  r->samples={sample};
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::histogram); show(p,r);
  p.set_transient_hidden(true); const auto base=render(p);
  p.set_transient_hidden(false); const auto picked=render(p);
  const auto rect=p.plot_rect(); const double dpr=p.devicePixelRatioF();
  unsigned cyan=0;
  for(int y=int(rect.top()*dpr);y<int(rect.bottom()*dpr);++y)
    for(int x=int((rect.left()+.15*rect.width())*dpr);x<int((rect.left()+.7*rect.width())*dpr);++x) {
      if(y<int((rect.bottom()-25)*dpr)) HDRSHOT_CHECK(picked.pixel(x,y)==base.pixel(x,y));
      const auto c=picked.pixelColor(x,y);
      if(c.blue()>c.red()+50&&c.green()>c.red()+50)++cyan;
    }
  HDRSHOT_CHECK(cyan>50);
  p.set_transient_hidden(true); HDRSHOT_CHECK(render(p)==base);
  p.set_transient_hidden(false);
  auto mean_only=std::make_shared<analysis::ResultData>(*r);
  mean_only->samples[0].distribution={}; p.set_result(mean_only,false);
  HDRSHOT_CHECK(render(p)!=picked);
  // New no-hover result cannot leave a previous distribution behind.
  auto cleared=std::make_shared<analysis::ResultData>(*r); cleared->samples.clear();
  p.set_result(cleared,false); HDRSHOT_CHECK(render(p)==base);
}
void picker_histogram_rgb_overlap_and_hue_exclusion() {
  auto r=std::make_shared<analysis::ResultData>(*wave_result());
  for(auto &hist:r->histograms){hist.counts.resize(8192);hist.maximum=1;}
  r->scopes.histogram_mode=analysis::HistogramMode::rgb;
  analysis::SampleResult sample;sample.request={0,50,50,101,true};sample.mean.valid_count=10201;
  sample.mean_position.histogram={.9,.9,.9};
  for(unsigned i=0;i<10201;++i){analysis::ProjectedSample point;
    const double v=.2+.4*double(i)/10201;point.histogram={v,v,v};point.hue_valid=false;
    sample.distribution.push_back(point);}
  r->samples={sample};
  AnalyzerScopePlot p(AnalyzerScopePlot::Kind::histogram);show(p,r);
  p.set_transient_hidden(true);const auto background=render(p);p.set_transient_hidden(false);
  auto image=render(p);const auto rect=p.plot_rect();const auto dpr=p.devicePixelRatioF();
  unsigned neutral=0;
  for(int y=int((rect.bottom()-20)*dpr);y<int(rect.bottom()*dpr);++y)
    for(int x=int((rect.x()+.22*rect.width())*dpr);x<int((rect.x()+.58*rect.width())*dpr);++x){
      const auto c=image.pixelColor(x,y);
      if(c.red()>100&&image.pixel(x,y)!=background.pixel(x,y)){HDRSHOT_CHECK(std::abs(c.red()-c.green())<2);HDRSHOT_CHECK(std::abs(c.red()-c.blue())<2);++neutral;}}
  HDRSHOT_CHECK(neutral>50);
  auto o=r->scopes;o.histogram_mode=analysis::HistogramMode::hue;p.set_options(o);
  const auto invalid=render(p);
  auto mean=std::make_shared<analysis::ResultData>(*r);mean->samples[0].distribution.clear();
  p.set_result(mean,false);HDRSHOT_CHECK(render(p)==invalid);
  // A region already exists: zoom/pan uses its existing domain projection.
  p.set_result(r,false);o.histogram_mode=analysis::HistogramMode::rgb;o.histogram_view={2.,.1};p.set_options(o);
  const auto zoomed=render(p);HDRSHOT_CHECK(zoomed!=image);
  o.histogram_view={};p.set_options(o);HDRSHOT_CHECK(render(p)==image);
}
void picker_colored_gain_is_four_times_user_gain_without_overdraw() {
  for(auto kind:{AnalyzerScopePlot::Kind::waveform,AnalyzerScopePlot::Kind::vectorscope})
    for(bool fine:{false,true}) {
      auto r=std::make_shared<analysis::ResultData>();
      r->valid_count=1000001; r->settings.working_space=analysis::WorkingSpace::display_p3_pq;
      r->scopes.colorize=true; r->scopes.wave_mode=analysis::WaveMode::intensity;
      r->vector_grid_x_extent=r->vector_grid_y_extent=1.;
      auto &grid=kind==AnalyzerScopePlot::Kind::waveform?r->waveform[0]:r->vectorscope;
      grid.width=grid.height=100;grid.maximum=1000000;
      grid.counts.resize(10000);grid.color_sums.resize(10000);
      grid.counts[60*100+55]=1;grid.color_sums[60*100+55]={.6f,.2f,.1f};
      grid.counts[80*100+80]=1000000;grid.color_sums[80*100+80]={600000,200000,100000};
      if(fine){r->waveform_detail=r->waveform;r->vectorscope_detail=r->vectorscope;r->vector_detail_x_extent=r->vector_detail_y_extent=1.;}
      analysis::SampleResult sample;sample.mean.valid_count=10201;
      sample.request={0,55,60,101,true};
      sample.mean_position.source_x=.2;sample.mean_position.intensity=.2;sample.mean_position.vector={-.2,-.2};
      analysis::ProjectedSample point;point.source_x=.555;point.intensity=.605;point.vector={.11,.21};
      sample.distribution.push_back(point); r->samples={sample};
      AnalyzerScopePlot p(kind);show(p,r);
      const auto rect=p.plot_rect(); const auto dpr=p.devicePixelRatioF();
      QPointF at;
      if(kind==AnalyzerScopePlot::Kind::waveform)at={rect.x()+.555*rect.width(),rect.bottom()-.605*rect.height()};
      else {const auto c=analysis::vector_calibration(r->settings,r->scopes.vector_mode,unsigned(std::ceil(rect.width()*dpr)),unsigned(std::ceil(rect.height()*dpr)),1.);
        const auto pos=analysis::project_vector(point.vector,c,1.);at={rect.x()+pos[0]*rect.width(),rect.bottom()-pos[1]*rect.height()};}
      const auto read=[&](const QImage &image){return image.pixelColor(int(at.x()*dpr),int(at.y()*dpr));};
      // Below saturation, the multiplier must be four, not two or G+4G.
      p.set_display_gain(.1);p.set_transient_hidden(true);const auto reference=read(render(p));
      HDRSHOT_CHECK(reference.red()>0);
      p.set_display_gain(.5);const auto low=read(render(p));
      p.set_transient_hidden(false);const auto four=read(render(p));
      // Vector base blends over #040506; the occupied tile is replaced over
      // black. Remove that background contribution from this pixel oracle.
      const int background = kind == AnalyzerScopePlot::Kind::vectorscope ? 4 : 0;
      HDRSHOT_CHECK_NEAR(double(four.red()) / (low.red()-background), 4., .6);
      p.set_display_gain(2.);p.set_transient_hidden(true);const auto base=read(render(p));
      p.set_transient_hidden(false);const auto highlighted=render(p);const auto bright=read(highlighted);
      HDRSHOT_CHECK(bright.red()>base.red()*1.65);
      HDRSHOT_CHECK_NEAR(double(bright.red())/std::max(1,bright.green()),3.,.4);
      HDRSHOT_CHECK_NEAR(double(bright.green())/std::max(1,bright.blue()),2.,.4);
      HDRSHOT_CHECK(p.display_gain()==2.);
      // Duplicating the same occupied pixel does not increase opacity or white out.
      auto duplicates=std::make_shared<analysis::ResultData>(*r);
      for(int i=1;i<10201;++i)duplicates->samples[0].distribution.push_back(point);
      p.set_result(duplicates,false);HDRSHOT_CHECK(render(p)==highlighted);
      // 2 -> 8 / 3 -> 12 saturate display alpha, not at the input limit 3.
      p.set_display_gain(3.);const auto twelve=read(render(p));
      HDRSHOT_CHECK(twelve.red()>=bright.red());
      HDRSHOT_CHECK(p.display_gain()==3.);
      p.set_display_gain(2.);HDRSHOT_CHECK(render(p)==highlighted);
      if(kind==AnalyzerScopePlot::Kind::waveform){auto o=r->scopes;o.colorize=false;p.set_options(o);const auto mono=read(render(p));HDRSHOT_CHECK(mono.blue()>mono.red());HDRSHOT_CHECK(mono.green()>mono.red());}
      const auto dir=qEnvironmentVariable("HDRSHOT_ANALYZER_SCOPE_ARTIFACTS");
      if(!dir.isEmpty()){QDir().mkpath(dir);render(p).save(dir+QString("/picker_%1_%2.png").arg(int(kind)).arg(fine));}
    }
}
} // namespace
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  app.setFont(QFont("Arial", 10));
  return hdrshot::test::run({
      {"major ticks suppress colliding subdivisions", tick_collision},
      {"pending keeps the existing trace", pending_keeps_trace},
      {"full domain texture pans and zooms without a new result", full_domain_pan_and_zoom},
      {"trackpad translation and actual pinch are distinct", trackpad_pan_and_native_pinch},
      {"vectorscope high-zoom pan retains old detail then replaces at its new center", vector_trackpad_pan_keeps_tiles_and_fit},
      {"shared gain changes wave/parade/vector base and detail, not histogram", gain_only_changes_scope_presentation},
      {"vector references stop at nominal axes even in a wide zoomed-out view", vector_reference_lines_stop_at_nominal_domain},
      {"uniform columns have no resampling gaps", full_columns_remain_visible_after_resample},
      {"current gamut follows trace transform and survives view-only changes", gamut_outline_uses_current_space_and_trace_transform},
      {"nonintegral source-column footprints do not create stripes", uneven_column_coverage_is_display_normalized},
      {"detail cache replaces and transforms without duplicate traces", detail_tile_replaces_base_and_moves_with_domain},
      {"capped vector detail covers the real viewport with a broken-aspect negative control", capped_vector_detail_covers_real_viewport},
      {"picker histogram is a bounded strip and clears without stale samples", picker_distribution_is_bounded_and_clears},
      {"picker histogram RGB overlap stays neutral and invalid hues are excluded", picker_histogram_rgb_overlap_and_hue_exclusion},
      {"picker reuses original colors at 4x user gain without duplicate opacity", picker_colored_gain_is_four_times_user_gain_without_overdraw},
      {"optional scoped visual artifact", artifact}});
}
