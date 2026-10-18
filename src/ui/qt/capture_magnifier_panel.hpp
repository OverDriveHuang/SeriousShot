#pragma once
#include "ui/qt/analysis_readings.hpp"
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>
#include <QWidget>
namespace hdrshot {
// Transparent image region is drawn by the existing native HDR presenter.
// Text is deliberately clipped at the lower screen edge, in its fixed order.
class CaptureMagnifierPanel final : public QWidget {
public:
  explicit CaptureMagnifierPanel(QWidget *parent) : QWidget(parent) {
    setObjectName("captureMagnifierPanel");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    content_ = new QWidget(this);
    content_->setObjectName("captureMagnifierReadings");
    content_->setAutoFillBackground(true);
    header_ = new QWidget(this);
    header_->setAutoFillBackground(true);
    auto *bar = new QHBoxLayout(header_);
    bar->setContentsMargins(8, 4, 8, 4);
    coordinate_ = new QLabel(header_);
    zoom_ = new QLabel(header_);
    bar->addWidget(coordinate_);
    bar->addStretch();
    bar->addWidget(zoom_);
    refresh_palette();
    layout_ = new QVBoxLayout(content_);
    layout_->setContentsMargins(8, 6, 8, 6);
    layout_->setSpacing(3);
    hide();
  }
  static constexpr int header_height() { return 28; }
  int text_height(const MagnifierReadings &f, int panel_width = 0) const {
    const int target_width=panel_width ? panel_width : width();
    if(height_width_==target_width && height_fields_==f && height_font_==font() &&
        height_dpr_==devicePixelRatioF()) return cached_height_;
    const auto rows = magnifier_scalar_rows({}, f);
    int height = 12;
    bool preceding = false;
    for (const auto &row : rows) {
      if (preceding)
        height += row.heading ? 7 : 3; // heading separator plus layout spacing
      height += row.heading ? heading_height(row.name, panel_width) : 19;
      preceding = true;
    }
    height_width_=target_width; height_fields_=f; height_font_=font();
    height_dpr_=devicePixelRatioF(); cached_height_=height;
    return height;
  }
  void show_sample(PixelPoint point, const MagnifierReadout &value,
                   const MagnifierReadings &fields, int image_height,
                   int zoom, bool visible = true) {
    image_height_ = image_height;
    coordinate_->setText(QString("%1, %2").arg(point.x).arg(point.y));
    zoom_->setText(QString("%1×").arg(zoom));
    header_->setGeometry(0, 0, width(), header_height());
    refresh_palette();
    const auto rows = magnifier_scalar_rows(value, fields);
    QStringList structure;
    for (const auto& r : rows) structure.push_back((r.heading ? "H:" : "V:") + r.name);
    const bool rebuilt=structure != structure_;
    if (rebuilt) {
      while (auto *item = layout_->takeAt(0)) { delete item->widget(); delete item; }
      values_.clear(); headings_.clear(); structure_ = structure;
      for (const auto& r : rows) {
        if (r.heading) {
          if (layout_->count()) {
            auto *line = new QFrame(content_);
            line->setFrameShape(QFrame::HLine); line->setFixedHeight(1);
            layout_->addWidget(line);
          }
          auto *label = new QLabel(r.name, content_);
          label->setObjectName("magnifierGroupHeading"); label->setWordWrap(true);
          headings_.push_back(label); layout_->addWidget(label);
        } else {
          auto *w = new QWidget(content_); auto *h = new QHBoxLayout(w);
          h->setContentsMargins(0, 0, 0, 0); h->setSpacing(4);
          auto *label = new QLabel(r.name, w); auto *n = new QLabel(w);
          n->setObjectName("magnifierScalarValue");
          n->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
          h->addWidget(label); h->addStretch(); h->addWidget(n);
          w->setFixedHeight(19); layout_->addWidget(w); values_.push_back(n);
        }
      }
    }
    int scalar = 0;
    for (const auto& r : rows) if (!r.heading) {
      auto *label = values_[scalar++];
      label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
      label->setText(r.value);
    }
    if(rebuilt || layout_width_!=width() || layout_font_!=font() || layout_dpr_!=devicePixelRatioF()) {
      for (auto *label : headings_) {
        label->setFont(heading_font()); label->setFixedHeight(heading_height(label->text()));
      }
      layout_width_=width();layout_font_=font();layout_dpr_=devicePixelRatioF();
    }
    // Child keeps full natural height; QWidget parent clips the lower rows.
    content_->setGeometry(0, header_height() + image_height_, width(),
                          text_height(fields));
    if (visible) { show(); raise(); }
    else hide();
    ensurePolished();
    layout_->activate();
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setPen(QColor("#bbc2cc"));
    p.drawRect(0, header_height(), width() - 1, image_height_ - 1);
  }

private:
  QFont heading_font() const {
    auto f = font();
    f.setPointSizeF(std::max(9., f.pointSizeF() - 1));
    return f;
  }
  int heading_height(const QString &text, int panel_width = 0) const {
    const int available = std::max(1, (panel_width ? panel_width : width()) - 16);
    return std::max(19, QFontMetrics(heading_font())
                            .boundingRect(QRect(0, 0, available, 0),
                                          Qt::TextWordWrap, text)
                            .height());
  }
  void refresh_palette() {
    auto pal = palette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    pal.setColor(QPalette::Window,
                 dark ? QColor("#25282d") : QColor("#fafafa"));
    pal.setColor(QPalette::WindowText,
                 dark ? QColor("#e8edf2") : QColor("#20252c"));
    content_->setPalette(pal);
    header_->setPalette(pal);
  }
  QStringList structure_;
  mutable MagnifierReadings height_fields_;
  mutable QFont height_font_;
  mutable int height_width_{-1}, cached_height_{};
  mutable qreal height_dpr_{};
  QFont layout_font_;
  int layout_width_{-1};
  qreal layout_dpr_{};
  std::vector<QLabel*> values_, headings_;
  int image_height_{};
  QWidget *header_{};
  QLabel *coordinate_{};
  QLabel *zoom_{};
  QWidget *content_{};
  QVBoxLayout *layout_{};
};
} // namespace hdrshot
