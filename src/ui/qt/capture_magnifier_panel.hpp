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
  int text_height(const MagnifierReadings &f) const {
    return int(magnifier_scalar_rows({}, f).size()) * 22 + 12;
  }
  void show_sample(PixelPoint point, const MagnifierReadout &value,
                   const MagnifierReadings &fields, int image_height,
                   int zoom) {
    image_height_ = image_height;
    coordinate_->setText(QString("%1, %2").arg(point.x).arg(point.y));
    zoom_->setText(QString("%1×").arg(zoom));
    header_->setGeometry(0, 0, width(), header_height());
    refresh_palette();
    while (auto *item = layout_->takeAt(0)) {
      delete item->widget();
      delete item;
    }
    auto row = [&](const QString &name, const QString &value) {
      auto *w = new QWidget(content_);
      auto *h = new QHBoxLayout(w);
      h->setContentsMargins(0, 0, 0, 0);
      h->setSpacing(4);
      auto *label = new QLabel(name, w);
      auto *n = new QLabel(value, w);
      n->setObjectName("magnifierScalarValue");
      n->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
      n->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      h->addWidget(label);
      h->addStretch();
      h->addWidget(n);
      w->setFixedHeight(19);
      layout_->addWidget(w);
    };
    for (const auto &r : magnifier_scalar_rows(value, fields)) {
      if (r.heading) {
        if (layout_->count()) {
          auto *line = new QFrame(content_);
          line->setFrameShape(QFrame::HLine);
          line->setFixedHeight(1);
          layout_->addWidget(line);
        }
        auto *label = new QLabel(r.name, content_);
        label->setWordWrap(true);
        label->setFixedHeight(19);
        auto font = label->font();
        font.setPointSizeF(std::max(9., font.pointSizeF() - 1));
        label->setFont(font);
        layout_->addWidget(label);
      } else
        row(r.name, r.value);
    }
    // Child keeps full natural height; QWidget parent clips the lower rows.
    content_->setGeometry(0, header_height() + image_height_, width(),
                          text_height(fields));
    show();
    raise();
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setPen(QColor("#bbc2cc"));
    p.drawRect(0, header_height(), width() - 1, image_height_ - 1);
  }

private:
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
  int image_height_{};
  QWidget *header_{};
  QLabel *coordinate_{};
  QLabel *zoom_{};
  QWidget *content_{};
  QVBoxLayout *layout_{};
};
} // namespace hdrshot
