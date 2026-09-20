#include "platform/windows/windows_application_icon.hpp"
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <array>

// Build-time serialization of the same vector icon used by the resident app.
// Windows Explorer reads RT_GROUP_ICON rather than QApplication::windowIcon.
int main(int argc,char** argv) {
  QApplication app(argc,argv);
  if(argc!=2) return 2;
  const auto icon=hdrshot::windows_application_icon();
  constexpr std::array<int,8> sizes{16,20,24,32,48,64,128,256};
  std::array<QByteArray,sizes.size()> images;
  for(std::size_t i=0;i<sizes.size();++i) {
    QBuffer buffer(&images[i]);buffer.open(QIODevice::WriteOnly);
    if(!icon.pixmap(sizes[i],sizes[i]).toImage().save(&buffer,"PNG")) return 1;
  }
  QFile file(QString::fromLocal8Bit(argv[1]));
  if(!file.open(QIODevice::WriteOnly)) return 1;
  QDataStream stream(&file);stream.setByteOrder(QDataStream::LittleEndian);
  stream<<quint16(0)<<quint16(1)<<quint16(sizes.size());
  quint32 offset=6+16*sizes.size();
  for(std::size_t i=0;i<sizes.size();++i) {
    stream<<quint8(sizes[i]%256)<<quint8(sizes[i]%256)<<quint8(0)<<quint8(0)
          <<quint16(1)<<quint16(32)<<quint32(images[i].size())<<offset;
    offset+=quint32(images[i].size());
  }
  for(const auto& image:images) if(file.write(image)!=image.size()) return 1;
  return stream.status()==QDataStream::Ok && file.flush()?0:1;
}
