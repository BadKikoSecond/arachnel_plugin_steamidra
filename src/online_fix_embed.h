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

/**
 * Sync SteamFix / marker unlocks with enabled DLC.
 * Non-empty enabled list → 0=dlc; empty → base only.
 */
bool applyDlcUnlocks(const QString& installPath, const QString& appid,
                     const QStringList& enabledDlcAppIds);

/** Best-effort repair before launch (registry seed + FreeTP cmd.exe stub for
 *  Windows and Proton - blocks freetp.org CreateProcess). */
void repairOnlineFixBeforeLaunch(const QString& installPath, const QString& appid,
                                 const QString& pluginRoot);

QString findLaunchExeForInstall(const QString& installPath);

} // namespace steamidra
