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

/**
 * Prefer Steam appinfo config.launch executable (steamcmd API), then filesystem
 * heuristics. Never returns FreeTP promo stub cmd.exe.
 */
ResolvedLaunchExe resolveInstallLaunch(const QString& installPath, const QString& appid = {});

/** Absolute exe path only (compat). */
QString findLaunchExeForInstall(const QString& installPath, const QString& appid = {});

} // namespace steamidra
