#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

namespace steamidra {

/** Blocking HTTP GET (safe to call from worker threads). Uses QNetworkAccessManager. */
QByteArray httpGet(const QString& url, int timeoutMs = 120000,
                   const QMap<QString, QString>& headers = {}, QString* errorOut = nullptr);

QByteArray httpGetJsonBytes(const QString& url, int timeoutMs = 60000,
                            const QMap<QString, QString>& headers = {}, QString* errorOut = nullptr);

} // namespace steamidra
