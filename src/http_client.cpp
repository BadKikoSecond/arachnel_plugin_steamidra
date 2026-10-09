#include "http_client.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
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
    // Drain the reply as it arrives instead of readAll() at the end: the reply buffers every
    // byte it has not handed out, so a 70 MB catalog used to sit in memory twice (reply
    // buffer + the copy) with geometric growth slack on top.
    QByteArray body;
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [&body](qint64, qint64 total) {
                         if (total > 0 && body.capacity() < total)
                             body.reserve(total + 4096);
                     });
    QObject::connect(reply, &QNetworkReply::readyRead, reply,
                     [&body, reply]() { body.append(reply->readAll()); });
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
    body.append(reply->readAll());
    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = statusVar.isValid() ? statusVar.toInt() : 0;
    reply->deleteLater();
    if (status >= 400) {
        if (errorOut) {
            QString detail = QString::fromUtf8(body).trimmed();
            // Prefer JSON {"detail":"…"} from the relay.
            if (detail.startsWith(QLatin1Char('{'))) {
                const QJsonObject obj = QJsonDocument::fromJson(body).object();
                const QString d = obj.value(QStringLiteral("detail")).toString().trimmed();
                if (!d.isEmpty())
                    detail = d;
            }
            if (detail.size() > 280)
                detail = detail.left(277) + QStringLiteral("...");
            *errorOut = detail.isEmpty() ? QStringLiteral("HTTP %1").arg(status)
                                         : QStringLiteral("HTTP %1: %2").arg(status).arg(detail);
        }
        return {};
    }
    return body;
}

QByteArray httpGetJsonBytes(const QString& url, int timeoutMs,
                            const QMap<QString, QString>& headers, QString* errorOut)
{
    return httpGet(url, timeoutMs, headers, errorOut);
}

} // namespace steamidra
