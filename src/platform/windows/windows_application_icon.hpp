#pragma once
#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace hdrshot {
// Match the Mac tray's camera-photo meaning. Supply a deterministic vector
// fallback because Windows usually has no camera-photo icon theme.
inline QIcon windows_application_icon() {
  QIcon fallback;
  for(int size:{16,20,24,32,48,64,128,256}) {
    QPixmap pixmap(size,size);pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);
    p.scale(size/24.0,size/24.0);
    p.setPen(QPen(QColor("#f8fafc"),1.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    p.setBrush(QColor("#334155"));
    p.drawRoundedRect(QRectF(2,6,20,15),3,3);
    p.drawRoundedRect(QRectF(7,3,10,5),1.5,1.5);
    p.setPen(Qt::NoPen);p.drawRect(QRectF(7,6,10,3));
    p.setPen(QPen(QColor("#f8fafc"),1.7));p.drawEllipse(QPointF(12,13),4,4);
    p.setPen(Qt::NoPen);p.setBrush(QColor("#f8fafc"));p.drawEllipse(QPointF(19,9),0.8,0.8);
    p.end();fallback.addPixmap(pixmap);
  }
  return QIcon::fromTheme(QStringLiteral("camera-photo"),fallback);
}
}
