#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTimer>
#include <cstdio>
#include <functional>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QNetworkAccessManager manager;
  QJsonArray replies;
  QStringList urls{
      "https://raw.githubusercontent.com/OverDriveHuang/SeriousShot/main/updates/stable.json",
      "https://api.github.com/repos/OverDriveHuang/SeriousShot/releases/tags/v0.1.4"};
  int index = 0;
  std::function<void()> next;
  next = [&] {
    if (index >= urls.size()) {
      std::printf(
          "%s\n",
          QJsonDocument(
              QJsonObject{
                  {"tls", QSslSocket::supportsSsl()},
                  {"backend", QSslSocket::activeBackend()},
                  {"requests", replies},
                  {"scope", "read-only transport; no signed catalog or remote update acceptance"}})
              .toJson()
              .constData());
      app.quit();
      return;
    }
    const auto url = urls[index++];
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", "SeriousShot-isolated-readonly-probe");
    request.setTransferTimeout(30000);
    auto *reply = manager.get(request);
    QTimer::singleShot(30000, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &app, [&, reply, url] {
      const auto bytes = reply->readAll();
      const auto payload = QJsonDocument::fromJson(bytes).object();
      replies.push_back(QJsonObject{
          {"url", url},
          {"status", reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()},
          {"error", int(reply->error())},
          {"bytes", bytes.size()},
          {"etag", QString::fromLatin1(reply->rawHeader("ETag"))},
          {"release_id", payload["id"]},
          {"draft", payload["draft"]},
          {"prerelease", payload["prerelease"]}});
      reply->deleteLater();
      next();
    });
  };
  QTimer::singleShot(0, &app, next);
  return app.exec();
}
