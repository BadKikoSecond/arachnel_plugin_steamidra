#pragma once

#include <QString>
#include <QStringList>

namespace steamidra {

struct ResolvedLaunchExe {
    QString path;
    QString workingDirectory;
    QStringList arguments;
    /** "steam" | "heuristic" | empty */
    QString source;
};

struct SteamLaunchOption {
    QString id;
    QString title;
    QString path;
    QString workingDirectory;
    QStringList arguments;
    QString type;
    bool isDefault = false;
};

/**
 * Prefer Steam appinfo config.launch executable (steamcmd API), then filesystem
 * heuristics. Never returns FreeTP promo stub cmd.exe.
 */
ResolvedLaunchExe resolveInstallLaunch(const QString& installPath, const QString& appid = {});

/** Collect all valid launch options for the game (e.g. DX11, Vulkan, Safe Mode, VR). */
QVector<SteamLaunchOption> collectLaunchOptions(const QString& installPath, const QString& appid = {});

/** Absolute exe path only (compat). */
QString findLaunchExeForInstall(const QString& installPath, const QString& appid = {});

} // namespace steamidra
