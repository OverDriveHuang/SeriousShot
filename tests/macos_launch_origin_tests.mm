#include "platform/macos/macos_launch_origin.hpp"
#include <QApplication>
#include <QTimer>
#include <iostream>
int main(int argc,char**argv) {
  hdrshot::MacLaunchOriginObserver origin;
  QApplication app(argc,argv);
  app.setQuitOnLastWindowClosed(false);
  int delivered{};
  bool early{};
  bool entered{};
  origin.when_ready([&](hdrshot::MacLaunchOrigin value) {
    ++delivered; early=!entered;
    std::cout << "nativeLaunchOrigin=" << (value==hdrshot::MacLaunchOrigin::manual?"manual":"login") << '\n';
    // An observer registered after synchronous capture consumes the frozen
    // result only once; it cannot repeat a settings presentation.
    origin.when_ready([&](auto){++delivered;});
    app.quit();
  });
  QTimer::singleShot(3000,&app,[&]{app.exit(2);});
  entered=true;
  const int result=app.exec();
  if(result || delivered!=1 || early) {
    std::cerr << "launch barrier failed delivered="<<delivered<<" early="<<early<<'\n';return 1;
  }
  return 0;
}
