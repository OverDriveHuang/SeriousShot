#include "test_support.hpp"
#include <QApplication>
#include <QColorSpace>
#include <QCursor>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QTransform>
#include <QWindow>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <iostream>
#include <array>

namespace {
using CursorInitializer = id (*)(id, SEL, NSImage*, NSPoint);
CursorInitializer original_cursor_initializer{};
unsigned bitmap_cursor_constructions{};
id observe_cursor_initializer(id self, SEL selector, NSImage* image, NSPoint hotspot) {
  HDRSHOT_CHECK(image != nil && image.size.width > 0 && image.size.height > 0);
  ++bitmap_cursor_constructions;
  return original_cursor_initializer(self, selector, image, hotspot);
}
struct CursorConstructionObserver {
  Method method = class_getInstanceMethod(NSCursor.class, @selector(initWithImage:hotSpot:));
  CursorConstructionObserver() {
    HDRSHOT_CHECK(method != nullptr);
    original_cursor_initializer = reinterpret_cast<CursorInitializer>(
        method_setImplementation(method, reinterpret_cast<IMP>(observe_cursor_initializer)));
  }
  ~CursorConstructionObserver() {
    method_setImplementation(method, reinterpret_cast<IMP>(original_cursor_initializer));
  }
};
void bridge(const QColorSpace& source_space) {
  CGImageRef cg = nullptr;
  {
    QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(191, 73, 29));
    image.setColorSpace(source_space);
    cg = image.toCGImage();
    HDRSHOT_CHECK(cg != nullptr);
  }
  // Read the returned image after the source QImage and helper temporaries died.
  HDRSHOT_CHECK(CGImageGetWidth(cg) == 16 && CGImageGetHeight(cg) == 16);
  CGColorSpaceRef space = CGImageGetColorSpace(cg);
  HDRSHOT_CHECK(space != nullptr && CGColorSpaceGetModel(space) == kCGColorSpaceModelRGB);
  CFDataRef profile = CGColorSpaceCopyICCData(space);
  HDRSHOT_CHECK(profile != nullptr);
  const QByteArray bytes(reinterpret_cast<const char*>(CFDataGetBytePtr(profile)),
                         static_cast<qsizetype>(CFDataGetLength(profile)));
  HDRSHOT_CHECK(QColorSpace::fromIccProfile(bytes) ==
      (source_space.isValid() ? source_space : QColorSpace(QColorSpace::SRgb)));
  CFRelease(profile);
  std::array<unsigned char, 16 * 16 * 4> pixels{};
  CGContextRef context = CGBitmapContextCreate(pixels.data(), 16, 16, 8, 16 * 4,
      space, static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
             static_cast<uint32_t>(kCGBitmapByteOrder32Big));
  HDRSHOT_CHECK(context != nullptr);
  CGContextDrawImage(context, CGRectMake(0, 0, 16, 16), cg);
  CGContextRelease(context);
  const auto offset = (7 * 16 + 7) * 4;
  HDRSHOT_CHECK(pixels[offset] == 191 && pixels[offset + 1] == 73 &&
                pixels[offset + 2] == 29 && pixels[offset + 3] == 255);
  CGImageRelease(cg);
}
void native_cursor(QWindow& window, const QCursor& cursor) {
  const unsigned before = bitmap_cursor_constructions;
  window.setCursor(cursor);
  QGuiApplication::setOverrideCursor(cursor);
  QApplication::processEvents();
  HDRSHOT_CHECK(QGuiApplication::overrideCursor() != nullptr);
  HDRSHOT_CHECK(NSCursor.currentCursor != nil);
  HDRSHOT_CHECK(NSCursor.currentCursor.image != nil);
  HDRSHOT_CHECK(NSCursor.currentCursor.image.size.width > 0);
  // Hidden windows do not change the desktop currentCursor. Observe the actual
  // NSCursor bitmap initializer instead, proving Cocoa convertCursor ran.
  if (cursor.shape() == Qt::ArrowCursor) HDRSHOT_CHECK(bitmap_cursor_constructions == before);
  else HDRSHOT_CHECK(bitmap_cursor_constructions > before);
  std::cout << "cursor=" << cursor.shape() << " native bitmap constructions="
            << (bitmap_cursor_constructions - before) << '\n';
  QGuiApplication::restoreOverrideCursor();
}
}
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  HDRSHOT_CHECK(QGuiApplication::platformName() == "cocoa");
  const QColorSpace srgb(QColorSpace::SRgb);
  const auto generated = QColorSpace(QColorSpace::Primaries::SRgb,
      QColorSpace::TransferFunction::Gamma, 2.17f);
  const QColorSpace custom = QColorSpace::fromIccProfile(generated.iccProfile());
  HDRSHOT_CHECK(custom.isValid() && custom != srgb);
  for (int i = 0; i < 64; ++i) { bridge(QColorSpace()); bridge(srgb); bridge(custom); }
  QWindow window;
  window.create();
  HDRSHOT_CHECK(window.handle() != nullptr);
  CursorConstructionObserver observer;
  native_cursor(window, QCursor(Qt::ArrowCursor));
  native_cursor(window, QCursor(Qt::SizeAllCursor)); // overlay and pinned-window move
  QPixmap bitmap(32, 32); bitmap.fill(Qt::transparent);
  { QPainter painter(&bitmap); painter.setPen(Qt::red); painter.drawLine(4, 4, 27, 27); }
  native_cursor(window, QCursor(bitmap.transformed(QTransform().rotate(37)), 16, 16));
  std::cout << "Qt=" << qVersion() << " Cocoa SizeAll/custom cursor; 192 default/sRGB/custom ICC/pixel bridge checks passed\n";
}
