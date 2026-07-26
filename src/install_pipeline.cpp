#include "install_pipeline.h"

#include "http_client.h"
#include "online_fix_embed.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <stdexcept>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

namespace steamidra {
namespace {

void emitProgress(const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& cb, int pct,
                  const QString& detail, const QString& status = QStringLiteral("downloading"),
                  qint64 downloaded = 0, qint64 total = 0, int bps = 0)
{
    if (!cb)
        return;
    arachnel::core::OwnedDownloadProgress p;
    p.percent = pct;
    p.detail = detail;
    p.status = status;
    p.bytesDownloaded = downloaded;
    p.totalBytes = total;
    p.downloadRateBps = bps;
    cb(p);
}

bool steamLooksValid(const QString& path)
{
    return QFileInfo::exists(path + QStringLiteral("/steam.exe"))
           || QFileInfo::exists(path + QStringLiteral("/steam.sh"))
           || QDir(path + QStringLiteral("/steamapps")).exists();
}

QString findDdmod(const QString& pluginRoot)
{
    const QStringList cands = {
        pluginRoot + QStringLiteral("/third_party/DDMod/DepotDownloaderMod.dll"),
        pluginRoot + QStringLiteral("/third_party/windows/DDMod/DepotDownloaderMod.dll"),
        pluginRoot + QStringLiteral("/third_party/linux/DDMod/DepotDownloaderMod.dll"),
    };
    for (const QString& c : cands) {
        if (QFileInfo::exists(c))
            return c;
    }
    return {};
}

QString findDotnet(const QString& pluginRoot)
{
#if defined(Q_OS_WIN)
    const QStringList cands = {
        pluginRoot + QStringLiteral("/third_party/dotnet/dotnet.exe"),
        pluginRoot + QStringLiteral("/third_party/windows/dotnet/dotnet.exe"),
    };
#else
    const QStringList cands = {
        pluginRoot + QStringLiteral("/third_party/dotnet/dotnet"),
        pluginRoot + QStringLiteral("/third_party/linux/dotnet/dotnet"),
    };
#endif
    for (const QString& c : cands) {
        if (QFileInfo::exists(c))
            return c;
    }
    return QStandardPaths::findExecutable(QStringLiteral("dotnet"));
}

bool extractZipToDir(const QString& zipPath, const QString& destDir)
{
    QDir().mkpath(destDir);
#if defined(Q_OS_WIN)
    QProcess p;
    p.start(QStringLiteral("tar"),
            {QStringLiteral("-xf"), zipPath, QStringLiteral("-C"), destDir});
    if (!p.waitForFinished(600000) || p.exitCode() != 0) {
        // Fallback: PowerShell
        QProcess ps;
        const QString cmd = QStringLiteral(
            "Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                                .arg(zipPath, destDir);
        ps.start(QStringLiteral("powershell"),
                 {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), cmd});
        return ps.waitForFinished(600000) && ps.exitCode() == 0;
    }
    return true;
#else
    QProcess p;
    p.start(QStringLiteral("unzip"), {QStringLiteral("-o"), zipPath, QStringLiteral("-d"), destDir});
    if (p.waitForFinished(600000) && p.exitCode() == 0)
        return true;
    QProcess tar;
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("-xf"), zipPath, QStringLiteral("-C"), destDir});
    return tar.waitForFinished(600000) && tar.exitCode() == 0;
#endif
}

struct LuaParsed {
    QMap<QString, QString> depots;
    QMap<QString, QString> manifests;
};

LuaParsed parseLua(const QString& text)
{
    LuaParsed out;
    static const QRegularExpression addRe(
        QStringLiteral(
            R"(addappid\(\s*(\d+)\s*(?:,\s*(\d+)\s*)?(?:,\s*["']([0-9a-fA-F]{64})["']\s*)?\))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression manRe(
        QStringLiteral(R"(setManifestid\(\s*(\d+)\s*,\s*["']?(\d+)["']?\s*(?:,|\)))"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = addRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString depotId = m.captured(1);
        const QString key = m.captured(3).toLower();
        if (!key.isEmpty())
            out.depots.insert(depotId, key);
    }
    auto it2 = manRe.globalMatch(text);
    while (it2.hasNext()) {
        const QRegularExpressionMatch m = it2.next();
        out.manifests.insert(m.captured(1), m.captured(2));
    }
    return out;
}

QString extractLuaFromBytes(const QByteArray& raw, const QString& staging)
{
    QDir().mkpath(staging);
    if (raw.size() >= 2 && raw[0] == 'P' && raw[1] == 'K') {
        const QString zipPath = staging + QStringLiteral("/_relay.zip");
        QFile zf(zipPath);
        if (!zf.open(QIODevice::WriteOnly))
            return {};
        zf.write(raw);
        zf.close();
        const QString extractDir = staging + QStringLiteral("/_zip");
        QDir().mkpath(extractDir);
        if (!extractZipToDir(zipPath, extractDir))
            return {};
        QString luaText;
        QDirIterator it(extractDir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QString lower = it.fileName().toLower();
            QFile f(it.filePath());
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QByteArray data = f.readAll();
            if (lower.endsWith(QStringLiteral(".lua")) && luaText.isEmpty())
                luaText = QString::fromUtf8(data);
            if (lower.endsWith(QStringLiteral(".manifest"))) {
                QFile out(staging + QLatin1Char('/') + it.fileName());
                if (out.open(QIODevice::WriteOnly))
                    out.write(data);
            }
        }
        return luaText;
    }
    const QString text = QString::fromUtf8(raw);
    if (text.toLower().contains(QStringLiteral("addappid")))
        return text;
    return {};
}

QString fetchLua(const QString& appid, const PluginSettings& settings, const QString& staging,
                 const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress)
{
    const QString relay = settings.relayBaseUrl.trimmed();
    if (relay.isEmpty())
        throw std::runtime_error("relayBaseUrl is empty");
    QMap<QString, QString> headers{{QStringLiteral("Accept"), QStringLiteral("*/*")}};
    if (!settings.relayClientToken.isEmpty())
        headers.insert(QStringLiteral("X-Arachnel-Token"), settings.relayClientToken);

    QStringList errors;
    for (const QString& fileType : {QStringLiteral("manifest"), QStringLiteral("lua")}) {
        emitProgress(onProgress, 0, QStringLiteral("Relay (%1)…").arg(fileType));
        QString err;
        const QByteArray raw =
            httpGet(QStringLiteral("%1/v1/download/%2?file_type=%3").arg(relay, appid, fileType),
                    120000, headers, &err);
        if (raw.isEmpty()) {
            errors.append(QStringLiteral("relay %1: %2").arg(fileType, err));
            continue;
        }
        if (fileType == QStringLiteral("lua")) {
            const QString text = QString::fromUtf8(raw);
            if (!text.toLower().contains(QStringLiteral("addappid"))) {
                errors.append(QStringLiteral("relay lua: invalid"));
                continue;
            }
            QFile f(staging + QLatin1Char('/') + appid + QStringLiteral(".lua"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(text.toUtf8());
            const bool hasMf = !QDir(staging).entryList({QStringLiteral("*.manifest")}, QDir::Files).isEmpty();
            if (hasMf || text.toLower().contains(QStringLiteral("setmanifestid")))
                return text;
            errors.append(QStringLiteral("relay lua: no manifests"));
            continue;
        }
        const QString lua = extractLuaFromBytes(raw, staging);
        const bool hasMf = !QDir(staging).entryList({QStringLiteral("*.manifest")}, QDir::Files).isEmpty();
        if (!lua.isEmpty()
            && (hasMf || lua.toLower().contains(QStringLiteral("setmanifestid"))))
            return lua;
        errors.append(lua.isEmpty() ? QStringLiteral("relay zip: no lua")
                                    : QStringLiteral("relay zip: lua without manifests"));
    }
    throw std::runtime_error(
        QStringLiteral("Relay could not prepare app %1 (%2). Check https://ryuu.badkiko.ru")
            .arg(appid, errors.join(QStringLiteral("; ")))
            .toStdString());
}

void filterPrimaryDepots(const QString& appid, LuaParsed& parsed, bool includeDlc)
{
    // Keep appid itself out of depot download list; keep keyed depots.
    QMap<QString, QString> depots = parsed.depots;
    if (!includeDlc) {
        // Heuristic: keep depots whose id is close to appid or all keyed depots (DDMod needs keys).
        // Prefer all keyed depots — DLC exclusion is soft.
        Q_UNUSED(appid);
    }
    if (depots.isEmpty() && !parsed.manifests.isEmpty()) {
        for (auto it = parsed.manifests.begin(); it != parsed.manifests.end(); ++it)
            depots.insert(it.key(), QString());
    }
    parsed.depots = depots;
}

void copyManifestsFromStaging(const QString& staging)
{
    const QString tmp = QDir::tempPath();
    for (const QString& name : QDir(staging).entryList({QStringLiteral("*.manifest")}, QDir::Files)) {
        QFile::copy(staging + QLatin1Char('/') + name, tmp + QLatin1Char('/') + name);
    }
}

void ensureInjectBestEffort(const QString& steam, const QString& pluginRoot)
{
    if (steam.isEmpty() || !QDir(steam).exists())
        return;
    QDir().mkpath(steam + QStringLiteral("/config/stplug-in"));
#if defined(Q_OS_WIN)
    const QStringList lumas = {
        pluginRoot + QStringLiteral("/third_party/LumaCore"),
        pluginRoot + QStringLiteral("/third_party/windows/LumaCore"),
    };
    const QStringList wanted = {QStringLiteral("dwmapi.dll"), QStringLiteral("xinput1_4.dll"),
                                QStringLiteral("lumacore.dll"), QStringLiteral("lumacorepayload.dll")};
    for (const QString& root : lumas) {
        if (!QDir(root).exists())
            continue;
        int copied = 0;
        QDirIterator it(root, wanted, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            if (QFile::copy(it.filePath(), steam + QLatin1Char('/') + it.fileName())
                || QFileInfo::exists(steam + QLatin1Char('/') + it.fileName()))
                ++copied;
        }
        if (copied > 0)
            return;
    }
#else
    const QStringList sls = {
        pluginRoot + QStringLiteral("/third_party/SLSsteam"),
        pluginRoot + QStringLiteral("/third_party/linux/SLSsteam"),
    };
    for (const QString& root : sls) {
        const QString so = root + QStringLiteral("/SLSsteam.so");
        if (QFileInfo::exists(so)) {
            QDir().mkpath(steam + QStringLiteral("/config"));
            QFile::copy(so, steam + QStringLiteral("/config/SLSsteam.so"));
            return;
        }
    }
#endif
}

void runDdmod(const QString& pluginRoot, const QString& appid, const LuaParsed& parsed,
              const QString& dest, const QString& depotOs,
              const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress,
              const std::function<bool()>& shouldCancel)
{
    const QString dll = findDdmod(pluginRoot);
    const QString dotnet = findDotnet(pluginRoot);
    if (dll.isEmpty())
        throw std::runtime_error("DepotDownloaderMod.dll missing");
    if (dotnet.isEmpty())
        throw std::runtime_error("Bundled .NET runtime missing");

    QDir().mkpath(dest);
    const QString keysTmp = QDir::tempPath() + QStringLiteral("/arachnel_mistwalker_keys.vdf");
    {
        QFile kf(keysTmp);
        if (!kf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            throw std::runtime_error("Cannot write depot keys file");
        for (auto it = parsed.depots.begin(); it != parsed.depots.end(); ++it)
            kf.write(QStringLiteral("%1;%2\n").arg(it.key(), it.value()).toUtf8());
    }

    QStringList depots = parsed.depots.keys();
    if (depots.isEmpty())
        depots = parsed.manifests.keys();
    if (depots.isEmpty())
        throw std::runtime_error("No depot IDs — cannot run DepotDownloaderMod");

    const int total = qMax(1, depots.size());
    for (int i = 0; i < depots.size(); ++i) {
        if (shouldCancel && shouldCancel())
            throw std::runtime_error("Cancelled");
        const QString depotId = depots.at(i);
        const int basePct = qMax(1, int(99.0 * double(i) / double(total)));
        emitProgress(onProgress, basePct, QStringLiteral("Downloading depot %1…").arg(depotId));

        QStringList args{dll,
                         QStringLiteral("-app"),
                         appid,
                         QStringLiteral("-depot"),
                         depotId,
                         QStringLiteral("-depotkeys"),
                         keysTmp,
                         QStringLiteral("-max-downloads"),
                         QStringLiteral("64"),
                         QStringLiteral("-dir"),
                         dest,
                         QStringLiteral("-os"),
                         depotOs};
        if (parsed.manifests.contains(depotId))
            args << QStringLiteral("-manifest") << parsed.manifests.value(depotId);

        // Prefer local normalized manifest in temp
        const QString localMf =
            QDir::tempPath()
            + QStringLiteral("/arachnel_dd_%1.manifest").arg(depotId);
        QString stagingMf;
        if (parsed.manifests.contains(depotId)) {
            stagingMf = QDir::tempPath()
                        + QStringLiteral("/%1_%2.manifest")
                              .arg(depotId, parsed.manifests.value(depotId));
        }
        if (QFileInfo::exists(localMf))
            args << QStringLiteral("-manifestfile") << localMf;
        else if (!stagingMf.isEmpty() && QFileInfo::exists(stagingMf))
            args << QStringLiteral("-manifestfile") << stagingMf;

        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DOTNET_ROOT"), QFileInfo(dotnet).absolutePath());
        proc.setProcessEnvironment(env);
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(dotnet, args);
        if (!proc.waitForStarted(30000))
            throw std::runtime_error("Failed to start DepotDownloaderMod");

        static const QRegularExpression pctRe(QStringLiteral(R"(^\s*(\d+(?:\.\d+)?)\s*%)"));
        while (proc.state() != QProcess::NotRunning) {
            if (shouldCancel && shouldCancel()) {
                proc.kill();
                throw std::runtime_error("Cancelled");
            }
            proc.waitForReadyRead(200);
            const QByteArray chunk = proc.readAll();
            const QList<QByteArray> lines = chunk.split('\n');
            for (const QByteArray& line : lines) {
                const auto m = pctRe.match(QString::fromUtf8(line));
                if (m.hasMatch()) {
                    const double local = m.captured(1).toDouble();
                    const int overall =
                        qBound(1, 99, int(99.0 * (double(i) + local / 100.0) / double(total)));
                    emitProgress(onProgress, overall, QStringLiteral("Downloading..."));
                }
            }
            QThread::msleep(20);
        }
        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
            const QString err = QString::fromUtf8(proc.readAllStandardOutput()).right(500);
            throw std::runtime_error(
                QStringLiteral("DepotDownloader failed for depot %1: %2")
                    .arg(depotId, err)
                    .toStdString());
        }
    }
}

qint64 dirByteSize(const QString& path)
{
    qint64 n = 0;
    QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        n += it.fileInfo().size();
    }
    return n;
}

bool alreadyInstalled(const QString& path, const QString& appid)
{
    if (!QDir(path).exists())
        return false;
    const QString marker = path + QStringLiteral("/.arachnel-steamidra");
    if (QFileInfo::exists(marker)) {
        QFile f(marker);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            const QString id = o.value(QStringLiteral("appid")).toString();
            if (id.isEmpty() || id == appid)
                return dirByteSize(path) > 80LL * 1024 * 1024;
        }
    }
    return dirByteSize(path) > 80LL * 1024 * 1024
           && !findLaunchExeForInstall(path).isEmpty();
}

QString sanitizeInstallDir(const QString& title, const QString& appid)
{
    QString s;
    for (QChar c : title) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char(' '))
            s.append(c);
    }
    s = s.trimmed().remove(QLatin1Char(' '));
    if (s.isEmpty())
        s = QStringLiteral("App%1").arg(appid);
    return s.left(40);
}

} // namespace

QString findSteamPath(const PluginSettings& settings)
{
    if (!settings.steamPath.isEmpty() && steamLooksValid(settings.steamPath))
        return settings.steamPath;
#if defined(Q_OS_WIN)
    auto fromReg = [](HKEY hive, const wchar_t* subkey, const wchar_t* valueName) -> QString {
        HKEY key = nullptr;
        if (RegOpenKeyExW(hive, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return {};
        wchar_t buf[512];
        DWORD size = sizeof(buf);
        DWORD type = 0;
        const LONG ok =
            RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
        RegCloseKey(key);
        if (ok != ERROR_SUCCESS)
            return {};
        return QString::fromWCharArray(buf).trimmed().replace(QLatin1Char('\\'), QLatin1Char('/'));
    };
    for (const QString& p :
         {fromReg(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
          fromReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath")}) {
        if (!p.isEmpty() && steamLooksValid(p))
            return p;
    }
#endif
    const QString home = QDir::homePath();
    const QStringList cands = {
        home + QStringLiteral("/.steam/steam"),
        home + QStringLiteral("/.local/share/Steam"),
        QStringLiteral("C:/Program Files (x86)/Steam"),
        QStringLiteral("D:/Steam"),
    };
    for (const QString& c : cands) {
        if (steamLooksValid(c))
            return c;
    }
    return {};
}

bool installAddonZip(const QString& zipPath, const QString& destPath, QString* errorOut)
{
    if (!QFileInfo::exists(zipPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("Add-on archive not found");
        return false;
    }
    if (!QDir(destPath).exists()) {
        if (errorOut)
            *errorOut = QStringLiteral("Game install path missing");
        return false;
    }
    if (zipPath.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        if (!extractZipToDir(zipPath, destPath)) {
            if (errorOut)
                *errorOut = QStringLiteral("Failed to extract add-on zip");
            return false;
        }
        return true;
    }
    const QString destFile = destPath + QLatin1Char('/') + QFileInfo(zipPath).fileName();
    QFile::remove(destFile);
    if (!QFile::copy(zipPath, destFile)) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to copy add-on file");
        return false;
    }
    return true;
}

InstallPipelineResult runInstallPipeline(
    const QString& pluginRoot, const QString& appid, const QString& title, const QString& targetPath,
    const PluginSettings& settings, bool force,
    const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress,
    const std::function<bool()>& shouldCancel)
{
    InstallPipelineResult result;
    try {
        const QString steam = findSteamPath(settings);
        const QString installdir = sanitizeInstallDir(title, appid);
        QString installPath = targetPath;
        if (installPath.isEmpty()) {
            if (!settings.libraryFolder.isEmpty())
                installPath = settings.libraryFolder + QLatin1Char('/') + installdir;
            else {
                const QString base =
                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                    + QStringLiteral("/ArachnelGames");
                installPath = base + QStringLiteral("/%1-%2").arg(appid, installdir);
            }
        }

        if (!force && alreadyInstalled(installPath, appid)) {
            const qint64 size = dirByteSize(installPath);
            emitProgress(onProgress, 100,
                         QStringLiteral("Already installed · %1 MB").arg(size / (1024 * 1024)),
                         QStringLiteral("completed"), size, size, 0);
            QJsonObject settingsJson;
            settingsJson.insert(QStringLiteral("autoEmbedOnlineFix"), settings.autoEmbedOnlineFix);
            settingsJson.insert(QStringLiteral("onlineFixSourceDir"), settings.onlineFixSourceDir);
            const OnlineFixResult fix =
                embedOnlineFix(installPath, appid, pluginRoot, settingsJson, false);
            QString launchPath = findLaunchExeForInstall(installPath);
            QJsonObject launch;
            if (!launchPath.isEmpty()) {
                launch.insert(QStringLiteral("type"), QStringLiteral("exe"));
                launch.insert(QStringLiteral("path"), launchPath);
            } else {
                launch.insert(QStringLiteral("type"), QStringLiteral("steam"));
                launch.insert(QStringLiteral("uri"), QStringLiteral("steam://rungameid/%1").arg(appid));
            }
            QFile marker(installPath + QStringLiteral("/.arachnel-steamidra"));
            QJsonObject payload{{QStringLiteral("appid"), appid},
                                {QStringLiteral("mode"), QStringLiteral("already-installed")},
                                {QStringLiteral("launch"), launch}};
            if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
                marker.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
            result.ok = true;
            result.installPath = installPath;
            result.mode = QStringLiteral("already-installed");
            result.launch = launch;
            result.skippedDownload = true;
            if (fix.embedded) {
                QJsonObject of;
                of.insert(QStringLiteral("embedded"), true);
                of.insert(QStringLiteral("epic"), fix.epic);
                result.onlineFix = of;
            }
            return result;
        }

        ensureInjectBestEffort(steam, pluginRoot);

        QTemporaryDir staging;
        if (!staging.isValid())
            throw std::runtime_error("Cannot create staging directory");

        emitProgress(onProgress, 0, QStringLiteral("Getting game info…"));
        const QString lua = fetchLua(appid, settings, staging.path(), onProgress);
        LuaParsed parsed = parseLua(lua);
        for (const QString& name :
             QDir(staging.path()).entryList({QStringLiteral("*.manifest")}, QDir::Files)) {
            const QString stem = QFileInfo(name).completeBaseName();
            const int us = stem.indexOf(QLatin1Char('_'));
            if (us <= 0)
                continue;
            const QString depotId = stem.left(us);
            const QString mid = stem.mid(us + 1);
            bool ok1 = false, ok2 = false;
            depotId.toLongLong(&ok1);
            mid.toLongLong(&ok2);
            if (ok1 && ok2)
                parsed.manifests.insert(depotId, mid);
        }
        filterPrimaryDepots(appid, parsed, settings.includeDlc);
        if (!steam.isEmpty()) {
            QDir().mkpath(steam + QStringLiteral("/config/stplug-in"));
            QFile luaOut(steam + QStringLiteral("/config/stplug-in/%1.lua").arg(appid));
            if (luaOut.open(QIODevice::WriteOnly | QIODevice::Truncate))
                luaOut.write(lua.toUtf8());
        }
        copyManifestsFromStaging(staging.path());

        QString depotOs = settings.depotOs;
        if (depotOs.isEmpty())
            depotOs = QStringLiteral("windows");
#if !defined(Q_OS_WIN)
        if (depotOs == QStringLiteral("auto"))
            depotOs = QStringLiteral("windows");
#endif

        emitProgress(onProgress, 1, QStringLiteral("Downloading..."));
        runDdmod(pluginRoot, appid, parsed, installPath, depotOs, onProgress, shouldCancel);

        QJsonObject settingsJson;
        settingsJson.insert(QStringLiteral("autoEmbedOnlineFix"), settings.autoEmbedOnlineFix);
        settingsJson.insert(QStringLiteral("onlineFixSourceDir"), settings.onlineFixSourceDir);
        const OnlineFixResult fix =
            embedOnlineFix(installPath, appid, pluginRoot, settingsJson, false);

        QString launchPath = findLaunchExeForInstall(installPath);
        QJsonObject launch;
        if (!launchPath.isEmpty()) {
            launch.insert(QStringLiteral("type"), QStringLiteral("exe"));
            launch.insert(QStringLiteral("path"), launchPath);
        } else {
            launch.insert(QStringLiteral("type"), QStringLiteral("steam"));
            launch.insert(QStringLiteral("uri"), QStringLiteral("steam://rungameid/%1").arg(appid));
        }
        QFile marker(installPath + QStringLiteral("/.arachnel-steamidra"));
        QJsonObject payload{{QStringLiteral("appid"), appid},
                            {QStringLiteral("mode"), QStringLiteral("ddmod")},
                            {QStringLiteral("launch"), launch}};
        if (fix.embedded) {
            QJsonObject of;
            of.insert(QStringLiteral("embedded"), true);
            of.insert(QStringLiteral("epic"), fix.epic);
            of.insert(QStringLiteral("targets"), QJsonArray::fromStringList(fix.targets));
            payload.insert(QStringLiteral("onlineFix"), of);
            result.onlineFix = of;
        }
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
            marker.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));

        emitProgress(onProgress, 100, QStringLiteral("Installed"), QStringLiteral("completed"));
        result.ok = true;
        result.installPath = installPath;
        result.mode = QStringLiteral("ddmod");
        result.launch = launch;
        return result;
    } catch (const std::exception& ex) {
        result.ok = false;
        result.error = QString::fromUtf8(ex.what());
        return result;
    }
}

} // namespace steamidra
