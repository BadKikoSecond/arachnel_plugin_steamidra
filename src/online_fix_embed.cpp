#include "online_fix_embed.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace steamidra {
namespace {

QString kitDir(const QString& pluginRoot, const QJsonObject& settings)
{
    const QString configured = settings.value(QStringLiteral("onlineFixSourceDir")).toString().trimmed();
    if (!configured.isEmpty() && QFileInfo::exists(configured + QStringLiteral("/SteamFix64.dll")))
        return configured;
    const QString bundled = pluginRoot + QStringLiteral("/online_fix_kit");
    if (QFileInfo::exists(bundled + QStringLiteral("/SteamFix64.dll"))
        && QFileInfo::exists(bundled + QStringLiteral("/winmm.dll")))
        return bundled;
    return bundled;
}

bool copyFileBestEffort(const QString& src, const QString& dst)
{
    if (!QFileInfo::exists(src))
        return false;
    QFile::remove(dst);
    return QFile::copy(src, dst);
}

void writeEpicFixIni(const QString& path)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write("[EOS]\r\nNoAuth=1\r\nDeviceAuth=true\r\n");
    f.commit();
}

void writeWinmmTxt(const QString& path, bool includeEpic)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    QByteArray body = "SteamFix64.dll";
    if (includeEpic)
        body += "\r\nEpicFix64.dll";
    f.write(body);
    f.commit();
}

void writeSteamFixIni(const QString& path, const QString& appid, bool workshop)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    QByteArray t;
    t += "[Main]\r\n";
    t += "RealAppId=" + appid.toUtf8() + "\r\n";
    t += "FakeAppId=480\r\n#Language=english\r\nBuildId=0\r\n\r\n";
    t += "[Misc]\r\nOverlay=true\r\nFilterLobby=false\r\n\r\n";
    t += "[Interfaces]\r\nApps=true\r\nFriends=true\r\nUser=true\r\nInventory=true\r\n";
    t += "Stats=true\r\nStorage=true\r\nUtils=true\r\n";
    t += workshop ? "Workshop=true\r\n\r\n[DLC]\r\n0=dlc\r\n" : "Workshop=false\r\n\r\n[DLC]\r\n";
    f.write(t);
    f.commit();
}

bool skipOverlayPath(const QString& path)
{
    const QString l = path.toLower();
    static const char* skip[] = {".depotdownloader", "staging", "intermediate", "saved",
                                 "deriveddatacache", "thirdparty", "downloads", "proton", ".git"};
    for (const char* s : skip) {
        if (l.contains(QLatin1String(s)))
            return true;
    }
    return false;
}

bool gameNeedsEpicFix(const QString& root)
{
    QDirIterator it(root, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString name = it.fileName();
        if (it.fileInfo().isDir() && name.compare(QStringLiteral("RedpointEOS"), Qt::CaseInsensitive) == 0)
            return true;
        if (it.fileInfo().isFile()) {
            if (name.startsWith(QStringLiteral("RedpointEOS"), Qt::CaseInsensitive)
                && name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive))
                return true;
            if (name.startsWith(QStringLiteral("EOSSDK"), Qt::CaseInsensitive)
                && name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive))
                return true;
        }
    }
    return false;
}

bool gameHasSteamApi(const QString& root)
{
    QDirIterator it(root,
                    {QStringLiteral("steam_api64.dll"), QStringLiteral("steam_api.dll")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!skipOverlayPath(it.filePath()))
            return true;
    }
    return false;
}

QStringList findUeShippingDirs(const QString& root)
{
    QStringList out;
    QDirIterator it(root, {QStringLiteral("*Shipping*.exe"), QStringLiteral("*-Win64-Shipping.exe")},
                    QDir::Files, QDirIterator::Subdirectories);
    QSet<QString> seen;
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString lower = it.fileName().toLower();
        if (lower.contains(QStringLiteral("crash")) || lower.contains(QStringLiteral("prereq")))
            continue;
        const QString parent = it.fileInfo().absolutePath();
        if (seen.contains(parent))
            continue;
        seen.insert(parent);
        out.append(parent);
    }
    return out;
}

QStringList findUnityRoots(const QString& root)
{
    QStringList out;
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        if (!it.fileName().endsWith(QStringLiteral("_Data")))
            continue;
        const QString parent = QFileInfo(it.filePath()).absolutePath();
        if (!out.contains(parent))
            out.append(parent);
    }
    return out;
}

QStringList findOverlayTargets(const QString& installRoot)
{
    QStringList ue = findUeShippingDirs(installRoot);
    if (!ue.isEmpty()) {
        std::sort(ue.begin(), ue.end(), [&](const QString& a, const QString& b) {
            const int sa = (gameNeedsEpicFix(a) ? 2 : 0)
                           + (QDir(a).entryList({QStringLiteral("*Shipping*.exe")}, QDir::Files).isEmpty() ? 0 : 1);
            const int sb = (gameNeedsEpicFix(b) ? 2 : 0)
                           + (QDir(b).entryList({QStringLiteral("*Shipping*.exe")}, QDir::Files).isEmpty() ? 0 : 1);
            return sa > sb;
        });
        return {ue.first()};
    }
    const QStringList unity = findUnityRoots(installRoot);
    if (!unity.isEmpty())
        return {unity.first()};
    return {installRoot};
}

int ensureEpicBesideEos(const QString& installRoot, const QString& kit)
{
    const QString epicDll = kit + QStringLiteral("/EpicFix64.dll");
    if (!QFileInfo::exists(epicDll))
        return 0;
    int placed = 0;
    QDirIterator it(installRoot, {QStringLiteral("EOSSDK*.dll")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString dir = it.fileInfo().absolutePath();
        if (copyFileBestEffort(epicDll, dir + QStringLiteral("/EpicFix64.dll"))) {
            writeEpicFixIni(dir + QStringLiteral("/EpicFix.ini"));
            ++placed;
        }
    }
    return placed;
}

QString pickOverlayExe(const QString& target)
{
    QDir dir(target);
    QString best;
    qint64 bestScore = -1;
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.exe")}, QDir::Files)) {
        const QString lower = fi.fileName().toLower();
        if (lower == QStringLiteral("cmd.exe") || lower.contains(QStringLiteral("crash"))
            || lower.contains(QStringLiteral("unins")) || lower.contains(QStringLiteral("redist")))
            continue;
        qint64 score = fi.size();
        if (lower.contains(QStringLiteral("launcher")) || lower.contains(QStringLiteral("bootstrap")))
            score -= 50LL * 1024 * 1024;
        if (lower.contains(QStringLiteral("shipping")) || lower.contains(QStringLiteral("win64")))
            score += 100LL * 1024 * 1024;
        if (QFileInfo::exists(target + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("_Data")))
            score += 200LL * 1024 * 1024;
        if (score > bestScore) {
            bestScore = score;
            best = fi.absoluteFilePath();
        }
    }
    return best;
}

} // namespace

QString findLaunchExeForInstall(const QString& installPath)
{
    const QStringList targets = findOverlayTargets(installPath);
    if (!targets.isEmpty()) {
        const QString exe = pickOverlayExe(targets.first());
        if (!exe.isEmpty())
            return exe;
    }
    QString best;
    qint64 bestScore = -1;
    QDirIterator it(installPath, {QStringLiteral("*.exe")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString lower = it.fileName().toLower();
        if (lower.contains(QStringLiteral("crash")) || lower.contains(QStringLiteral("unins"))
            || lower.contains(QStringLiteral("redist")) || lower.contains(QStringLiteral("prereq")))
            continue;
        qint64 score = it.fileInfo().size();
        const bool hasOverlay = QFileInfo::exists(it.fileInfo().absolutePath() + QStringLiteral("/SteamFix64.dll"))
                                || QFileInfo::exists(it.fileInfo().absolutePath() + QStringLiteral("/winmm.dll"));
        if (hasOverlay)
            score += 500LL * 1024 * 1024;
        if (lower.contains(QStringLiteral("shipping")))
            score += 200LL * 1024 * 1024;
        if (score > bestScore) {
            bestScore = score;
            best = it.filePath();
        }
    }
    return best;
}

OnlineFixResult embedOnlineFix(const QString& installPath, const QString& appid,
                               const QString& pluginRoot, const QJsonObject& settingsJson, bool force)
{
    OnlineFixResult r;
    const bool autoEmbed = settingsJson.contains(QStringLiteral("autoEmbedOnlineFix"))
                               ? settingsJson.value(QStringLiteral("autoEmbedOnlineFix")).toBool(true)
                               : true;
    if (!autoEmbed && !force) {
        r.skipped = true;
        r.reason = QStringLiteral("disabled");
        return r;
    }
    if (!QDir(installPath).exists()) {
        r.ok = false;
        r.error = QStringLiteral("install path missing");
        return r;
    }

    const QString kit = kitDir(pluginRoot, settingsJson);
    if (!QFileInfo::exists(kit + QStringLiteral("/SteamFix64.dll"))
        || !QFileInfo::exists(kit + QStringLiteral("/winmm.dll"))) {
        r.ok = false;
        r.error = QStringLiteral("Online-fix kit missing (SteamFix64.dll / winmm.dll)");
        return r;
    }

    // Always embed when steam_api present or force — store tags often miss depot installs.
    bool needs = force || gameHasSteamApi(installPath);
    if (!needs)
        needs = true; // depot installs without license still need FakeAppId overlay

    const QStringList targets = findOverlayTargets(installPath);
    if (targets.isEmpty()) {
        r.ok = false;
        r.error = QStringLiteral("no overlay target directories found");
        return r;
    }

    const bool needEpic = gameNeedsEpicFix(installPath) || gameNeedsEpicFix(targets.first());
    if (needEpic && !QFileInfo::exists(kit + QStringLiteral("/EpicFix64.dll"))) {
        r.ok = false;
        r.error = QStringLiteral(
            "Game uses RedpointEOS/EOSSDK but EpicFix64.dll is not in online_fix_kit");
        return r;
    }

    for (const QString& target : targets) {
        writeSteamFixIni(target + QStringLiteral("/SteamFix.ini"), appid, false);
        copyFileBestEffort(kit + QStringLiteral("/SteamFix64.dll"),
                           target + QStringLiteral("/SteamFix64.dll"));
        copyFileBestEffort(kit + QStringLiteral("/winmm.dll"), target + QStringLiteral("/winmm.dll"));
        writeWinmmTxt(target + QStringLiteral("/winmm.txt"), needEpic);
        if (needEpic) {
            copyFileBestEffort(kit + QStringLiteral("/EpicFix64.dll"),
                               target + QStringLiteral("/EpicFix64.dll"));
            writeEpicFixIni(target + QStringLiteral("/EpicFix.ini"));
            ensureEpicBesideEos(installPath, kit);
        }
        // FreeTP ack folder (best-effort)
        QDir().mkpath(target + QStringLiteral("/FreeTP/UserData"));
        QFile anon(target + QStringLiteral("/FreeTP/UserData/AnonFolderSave.txt"));
        if (!anon.exists() && anon.open(QIODevice::WriteOnly))
            anon.close();
        r.targets.append(target);
    }

    r.embedded = !r.targets.isEmpty();
    r.epic = needEpic;
    // Update marker launch path
    const QString markerPath = installPath + QStringLiteral("/.arachnel-steamidra");
    QJsonObject payload;
    QFile marker(markerPath);
    if (marker.exists() && marker.open(QIODevice::ReadOnly)) {
        payload = QJsonDocument::fromJson(marker.readAll()).object();
        marker.close();
    }
    const QString launchExe = findLaunchExeForInstall(installPath);
    if (!launchExe.isEmpty()) {
        QJsonObject launch;
        launch.insert(QStringLiteral("type"), QStringLiteral("exe"));
        launch.insert(QStringLiteral("path"), launchExe);
        payload.insert(QStringLiteral("launch"), launch);
    }
    QJsonObject of;
    of.insert(QStringLiteral("embedded"), true);
    of.insert(QStringLiteral("epic"), needEpic);
    of.insert(QStringLiteral("targets"), QJsonArray::fromStringList(r.targets));
    payload.insert(QStringLiteral("onlineFix"), of);
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
        marker.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    return r;
}

} // namespace steamidra
