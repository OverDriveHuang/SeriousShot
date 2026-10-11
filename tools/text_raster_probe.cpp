#include "platform/freetype/freetype_text_rasterizer_port.hpp"
#include "ports/annotation_font_contract.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QPainter>
#include <QLineEdit>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H
#include <iostream>
#include <array>
#include <vector>

// Standalone diagnostic: compare production masks, FreeType hinting modes,
// and the real Qt input widget. Run from the project root with an output path.
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (argc < 2) return 1;
  const char* path = "assets/fonts/NotoSansSC-VF.ttf";
  const int id = QFontDatabase::addApplicationFont(path);
  const auto family = QFontDatabase::applicationFontFamilies(id).first();
  std::cout << "Qt family=" << family.toStdString() << '\n';
  FT_Library library{}; FT_Face face{};
  if (FT_Init_FreeType(&library) || FT_New_Face(library, path, 0, &face)) return 2;
  FT_Fixed weight = 450 * 65536;
  if (FT_Set_Var_Design_Coordinates(face, 1, &weight)) return 3;
  int major, minor, patch; FT_Library_Version(library, &major, &minor, &patch);
  std::cout << "FreeType=" << major << '.' << minor << '.' << patch << '\n';
  FT_ULong prep_length=0;
  FT_Load_Sfnt_Table(face, FT_MAKE_TAG('p','r','e','p'), 0, nullptr, &prep_length);
  std::cout << "prep table bytes=" << prep_length << '\n';
  // Causal isolation on single glyphs: bypass composition, GPU and all resampling.
  for (const int audit_weight : {400,450}) {
  FT_Fixed audit_coordinate=audit_weight*65536;
  if (FT_Set_Var_Design_Coordinates(face,1,&audit_coordinate)) return 8;
  for (int size : {12,15,16,18,20,22,24,28,32,40,48}) {
    FT_Set_Pixel_Sizes(face,0,size);
    for (auto c : QString::fromUtf8("在里")) {
      std::array<QImage,4> masks;
      const int flags[] = {FT_LOAD_DEFAULT, FT_LOAD_FORCE_AUTOHINT, FT_LOAD_NO_AUTOHINT, FT_LOAD_NO_HINTING|FT_LOAD_NO_AUTOHINT};
      for(int m=0;m<4;++m) {
        QImage mask(80,80,QImage::Format_Grayscale8); mask.fill(0);
        if(FT_Load_Char(face,c.unicode(),flags[m]) || FT_Render_Glyph(face->glyph,FT_RENDER_MODE_NORMAL)) return 6;
        auto g=face->glyph;
        for(unsigned y=0;y<g->bitmap.rows;++y) for(unsigned x=0;x<g->bitmap.width;++x) {
          const int xx=8+g->bitmap_left+x, yy=60-g->bitmap_top+y;
          if(xx>=0&&xx<80&&yy>=0&&yy<80) mask.scanLine(yy)[xx]=g->bitmap.buffer[y*g->bitmap.pitch+x];
        }
        masks[m]=mask;
      }
      std::cout << "isolation weight=" << audit_weight << " size=" << size << " char=" << static_cast<unsigned>(c.unicode())
                << " default_eq_forced_auto=" << (masks[0]==masks[1])
                << " noauto_eq_nohint=" << (masks[2]==masks[3])
                << " default_eq_noauto=" << (masks[0]==masks[2]) << '\n';
      if (size==24 && c.unicode()==0x5728) {
        const int x=8+static_cast<int>(size*0.8);
        for(int m : {0,2}) {
          int bottom=79; while(bottom>=0 && masks[m].constScanLine(bottom)[x]==0) --bottom;
          std::cout << "bottom24 weight=" << audit_weight << " mode=" << m << " coverage=";
          for(int y=bottom-4;y<=bottom;++y) std::cout << static_cast<int>(masks[m].constScanLine(y)[x]) << ':';
          std::cout << '\n';
        }
      }
    }
  }
  }
  if (FT_Set_Var_Design_Coordinates(face,1,&weight)) return 9;
  for (int size : {12,16,20,22,24,28,32,40,48}) {
    FT_Set_Pixel_Sizes(face,0,size);
    int fractional_advances=0;
    for (auto c : QString::fromUtf8("在里 土王三 目重画田 HDR Agj 0123")) {
      if (FT_Load_Char(face,c.unicode(),FT_LOAD_NO_AUTOHINT)) return 10;
      if (face->glyph->advance.x % 64 != 0) ++fractional_advances;
    }
    std::cout << "metrics size=" << size << " height26_6=" << face->size->metrics.height
              << " ascender26_6=" << face->size->metrics.ascender
              << " descender26_6=" << face->size->metrics.descender
              << " fractional_advances=" << fractional_advances << '\n';
  }
  hdrshot::FreeTypeTextRasterizerPort production(path, hdrshot::annotation_font::identity);
  std::cout << "Production identity=" << hdrshot::annotation_font::identity << '\n';
  const QString sample = QString::fromUtf8("不在里 土王三");
  const int sizes[] = {12, 16, 20, 22, 24, 28, 32, 40, 48};
  const char* labels[] = {"Production no-auto 450", "FT no-hint 450", "FT auto light 450", "FT target light 450", "Qt prior weight450/default", "Qt axis450/default", "Qt weight400/default", "FT baseline default450", "Qt fixed axis450/no-hint"};
  QImage sheet(3330, 9 * 270, QImage::Format_RGB32); sheet.fill(Qt::white);
  QPainter out(&sheet);
  for (int row = 0; row < 9; ++row) {
    const int size = sizes[row];
    FT_Set_Pixel_Sizes(face, 0, size);
    for (int mode = 0; mode < 9; ++mode) {
      QImage tile(360, 58, QImage::Format_RGB32); tile.fill(Qt::white);
      if (mode == 0) {
        auto mask = production.rasterize({hdrshot::ObjectId{1}, sample.toStdString(), {360,58}, static_cast<uint16_t>(size), 1.0});
        if (!mask) return 4;
        for (int y=0;y<58;++y) for(int x=0;x<360;++x) {
          int v=255-mask.value().coverage_u8[y*360+x]; tile.setPixel(x,y,qRgb(v,v,v));
        }
      } else if (mode < 4 || mode == 7) {
        int x=0;
        const int baseline = std::max(0,(58-static_cast<int>(face->size->metrics.height/64))/2)+static_cast<int>(face->size->metrics.ascender/64);
        for (auto c : sample) {
          const int flags = mode == 7 ? FT_LOAD_DEFAULT : (mode == 1 ? FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT : (mode == 2 ? FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT : FT_LOAD_TARGET_LIGHT));
          if (FT_Load_Char(face,c.unicode(),flags) || FT_Render_Glyph(face->glyph,FT_RENDER_MODE_NORMAL)) return 5;
          auto g=face->glyph;
          for(unsigned y=0;y<g->bitmap.rows;++y) for(unsigned xx=0;xx<g->bitmap.width;++xx) {
            int tx=x+g->bitmap_left+xx, ty=baseline-g->bitmap_top+y;
            if(tx>=0&&tx<360&&ty>=0&&ty<58) { int v=255-g->bitmap.buffer[y*g->bitmap.pitch+xx]; tile.setPixel(tx,ty,qRgb(v,v,v)); }
          }
          x+=static_cast<int>(g->advance.x/64);
        }
      } else {
        QLineEdit input; input.resize(360,58); input.setFrame(false);
        input.setStyleSheet("QLineEdit { color: black; background: white; border: none; padding: 0px; }");
        QFont font(family); font.setPixelSize(size); font.setWeight(static_cast<QFont::Weight>(450));
        font.setHintingPreference(QFont::PreferDefaultHinting);
        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(QFont::PreferAntialias|QFont::NoFontMerging));
        if (mode == 5 || mode == 8) font.setVariableAxis("wght",450.0f);
        if (mode == 8) font.setHintingPreference(QFont::PreferNoHinting);
        if (mode == 6) font.setWeight(QFont::Normal);
        if (row == 0) { QFontInfo info(font); std::cout << labels[mode] << " actual=" << info.family().toStdString() << '/' << info.styleName().toStdString() << " weight=" << info.weight() << '\n'; }
        input.setFont(font); input.setText(sample); input.deselect(); input.setCursorPosition(0);
        input.render(&tile);
      }
      if (mode < 4 || mode == 7) {
        // A column through the lower horizontal of the second character (在),
        // to the right of its vertical stem; report the last occupied rows.
        const int x=size+static_cast<int>(size*0.8);
        int bottom=57; while(bottom>=0 && qRed(tile.pixel(x,bottom))==255) --bottom;
        std::cout << size << ',' << labels[mode] << ",x=" << x << ",bottom=" << bottom << ",coverage=";
        for (int y=std::max(0,bottom-4);y<=bottom;++y) std::cout << 255-qRed(tile.pixel(x,y)) << ':';
        std::cout << '\n';
      }
      tile.save(QString::fromLocal8Bit(argv[1])+QString("/%1_%2.png").arg(size).arg(mode));
      const int left=mode*370, top=row*270;
      out.setPen(Qt::black); out.drawText(left+6,top+18,QString("%1 px / %2").arg(size).arg(labels[mode]));
      out.drawImage(QPoint(left+6,top+28),tile);
      out.drawImage(QRect(left+6,top+90,300,174),tile,QRect(0,0,100,58));
    }
  }
  out.end(); sheet.save(QString::fromLocal8Bit(argv[1])+"/comparison.png");
  for (int size : sizes) {
    const QImage normal(QString::fromLocal8Bit(argv[1])+QString("/%1_4.png").arg(size));
    const QImage axis(QString::fromLocal8Bit(argv[1])+QString("/%1_5.png").arg(size));
    const QImage regular(QString::fromLocal8Bit(argv[1])+QString("/%1_6.png").arg(size));
    const QImage fixed(QString::fromLocal8Bit(argv[1])+QString("/%1_8.png").arg(size));
    std::cout << "Qt size=" << size << " requested450_eq_400=" << (normal==regular)
              << " requested450_eq_axis450=" << (normal==axis)
              << " axis450_default_eq_nohint=" << (axis==fixed) << '\n';
  }
  const auto scale_one = production.rasterize({hdrshot::ObjectId{1}, "在里", {120,58}, 24, 1.0});
  const auto scale_two = production.rasterize({hdrshot::ObjectId{1}, "在里", {120,58}, 12, 2.0});
  if (!scale_one || !scale_two) return 7;
  std::cout << "24pt_1x_eq_12pt_2x=" << (scale_one.value().coverage_u8 == scale_two.value().coverage_u8) << '\n';
  QImage focus(1110,340,QImage::Format_RGB32); focus.fill(Qt::white);
  QPainter detail(&focus);
  const int selected[] = {7,0,8};
  for (int i=0;i<3;++i) {
    QImage tile(QString::fromLocal8Bit(argv[1])+QString("/24_%1.png").arg(selected[i]));
    detail.drawText(i*370+12,24,QString("24 physical px / %1").arg(labels[selected[i]]));
    detail.drawImage(QPoint(i*370+12,40),tile);
    detail.drawImage(QRect(i*370+12,112,350,210),tile,QRect(0,12,70,42));
  }
  detail.end(); focus.save(QString::fromLocal8Bit(argv[1])+"/focus_24px.png");
  FT_Done_Face(face); FT_Done_FreeType(library);
}
