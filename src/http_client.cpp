#include "http_client.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace steamidra {

QByteArray httpGet(const QString& url, int timeoutMs, const QMap<QString, QString>& headers,
                   QString* errorOut)
{
    if (errorOut)
        errorOut->clear();
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Arachnel-SteaMidra/0.4"));
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (timeoutMs > 0)
        timer.start(timeoutMs);
    loop.exec();

    if (timeoutMs > 0 && !timer.isActive() && reply->isRunning()) {
        reply->abort();
        reply->deleteLater();
        if (errorOut)
            *errorOut = QStringLiteral("HTTP timeout");
        return {};
    }
    timer.stop();

    if (reply->error() != QNetworkReply::NoError) {
        if (errorOut)
            *errorOut = reply->errorString();
        reply->deleteLater();
        return {};
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    return body;
}

QByteArray httpGetJsonBytes(const QString& url, int timeoutMs,
                            const QMap<QString, QString>& headers, QString* errorOut)
{
    return httpGet(url, timeoutMs, headers, errorOut);
}

} // namespace steamidra
