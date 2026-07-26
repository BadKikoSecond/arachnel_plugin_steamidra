#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace steamidra {

struct OnlineFixResult {
    bool ok = true;
    bool embedded = false;
    bool skipped = false;
    bool epic = false;
    QString error;
    QString reason;
    QStringList targets;
};

OnlineFixResult embedOnlineFix(const QString& installPath, const QString& appid,
                               const QString& pluginRoot, const QJsonObject& settingsJson,
                               bool force = false);

QString findLaunchExeForInstall(const QString& installPath);

} // namespace steamidra
