#include "install_pipeline.h"

#include "http_client.h"
#include "online_fix_embed.h"

#include <depotkit.h>

#include <QDebug>
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

#include <stdexcept>
#include <string>
#include <vector>

#if defined(Q_OS_WIN)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
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

    // Ryuu / Hubcap often leave stale setManifestid lines commented with `--`.
    // Matching those pins an old build (e.g. Meccha 3.0.0 vs 3.3.0).
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        const int comment = line.indexOf(QStringLiteral("--"));
        if (comment >= 0)
            line = line.left(comment);
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        auto it = addRe.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString depotId = m.captured(1);
            const QString key = m.captured(3).toLower();
            if (!key.isEmpty())
                out.depots.insert(depotId, key);
        }
        auto it2 = manRe.globalMatch(line);
        while (it2.hasNext()) {
            const QRegularExpressionMatch m = it2.next();
            out.manifests.insert(m.captured(1), m.captured(2));
        }
    }
    return out;
}

/** Extract .lua / .manifest from a ZIP in memory (no zip on disk — Defender MalUri).
 *  Handles store (method 0); deflated entries are skipped (Ryuu packs store). */
bool extractRelayZipMemory(const QByteArray& raw, const QString& staging, QString* luaOut)
{
    if (raw.size() < 30 || raw[0] != 'P' || raw[1] != 'K')
        return false;
    QDir().mkpath(staging);
    bool wrote = false;
    int pos = 0;
    while (pos + 30 <= raw.size()) {
        const auto u16 = [&](int off) -> quint16 {
            return quint16(quint8(raw[pos + off])) | (quint16(quint8(raw[pos + off + 1])) << 8);
        };
        const auto u32 = [&](int off) -> quint32 {
            return quint32(u16(off)) | (quint32(u16(off + 2)) << 16);
        };
        if (u32(0) != 0x04034b50u)
            break;
        const quint16 method = u16(8);
        const quint32 compSize = u32(18);
        const quint32 uncompSize = u32(22);
        const quint16 nameLen = u16(26);
        const quint16 extraLen = u16(28);
        const int dataOff = pos + 30 + nameLen + extraLen;
        if (dataOff < 0 || dataOff + int(compSize) > raw.size())
            break;
        const QString name = QString::fromUtf8(raw.constData() + pos + 30, nameLen);
        const QString base = QFileInfo(name).fileName().toLower();
        pos = dataOff + int(compSize);
        if (method != 0 || compSize != uncompSize)
            continue; // only stored entries
        if (!base.endsWith(QStringLiteral(".lua")) && !base.endsWith(QStringLiteral(".manifest")))
            continue;
        const QByteArray data = raw.mid(dataOff, int(compSize));
        if (base.endsWith(QStringLiteral(".lua")) && luaOut && luaOut->isEmpty())
            *luaOut = QString::fromUtf8(data);
        if (base.endsWith(QStringLiteral(".manifest"))) {
            QFile out(staging + QLatin1Char('/') + QFileInfo(name).fileName());
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.write(data);
                wrote = true;
            }
        } else if (base.endsWith(QStringLiteral(".lua"))) {
            QFile out(staging + QLatin1Char('/') + QFileInfo(name).fileName());
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                out.write(data);
            wrote = true;
        }
    }
    return wrote || (luaOut && !luaOut->isEmpty());
}

QString extractLuaFromBytes(const QByteArray& raw, const QString& staging)
{
    QDir().mkpath(staging);
    if (raw.size() >= 2 && raw[0] == 'P' && raw[1] == 'K') {
        QString luaText;
        if (extractRelayZipMemory(raw, staging, &luaText))
            return luaText;
        return {};
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
    QString luaText;

    emitProgress(onProgress, 0, QStringLiteral("Relay (lua)…"));
    {
        QString err;
        const QByteArray raw =
            httpGet(QStringLiteral("%1/v1/download/%2?file_type=lua").arg(relay, appid), 120000,
                    headers, &err);
        if (raw.isEmpty()) {
            errors.append(QStringLiteral("relay lua: %1").arg(err));
        } else {
            const QString text = QString::fromUtf8(raw);
            if (!text.toLower().contains(QStringLiteral("addappid"))) {
                errors.append(QStringLiteral("relay lua: invalid"));
            } else {
                luaText = text;
                QFile f(staging + QLatin1Char('/') + appid + QStringLiteral(".lua"));
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    f.write(text.toUtf8());
            }
        }
    }

    // Anonymous Steam CDN 401s without a local -manifestfile. Always pull the
    // relay zip (in memory — writing it triggers Defender MalUri) for *.manifest.
    emitProgress(onProgress, 0, QStringLiteral("Relay (manifest)…"));
    {
        QString err;
        const QByteArray raw =
            httpGet(QStringLiteral("%1/v1/download/%2?file_type=manifest").arg(relay, appid),
                    120000, headers, &err);
        if (raw.isEmpty()) {
            errors.append(QStringLiteral("relay manifest: %1").arg(err));
        } else {
            QString zipLua;
            if (!extractRelayZipMemory(raw, staging, &zipLua)) {
                errors.append(QStringLiteral("relay zip: extract failed"));
            } else if (luaText.isEmpty() && !zipLua.isEmpty()) {
                luaText = zipLua;
            }
        }
    }

    if (!luaText.isEmpty()) {
        const LuaParsed probe = parseLua(luaText);
        const bool hasMf =
            !QDir(staging).entryList({QStringLiteral("*.manifest")}, QDir::Files).isEmpty();
        if (hasMf || !probe.depots.isEmpty() || !probe.manifests.isEmpty())
            return luaText;
        errors.append(QStringLiteral("relay: lua without depots/manifests"));
    }

    throw std::runtime_error(
        QStringLiteral("Relay could not prepare app %1 (%2). Check https://ryuu.badkiko.ru")
            .arg(appid, errors.join(QStringLiteral("; ")))
            .toStdString());
}

void adoptStagingManifests(const QString& staging, LuaParsed& parsed)
{
    for (const QString& name : QDir(staging).entryList({QStringLiteral("*.manifest")}, QDir::Files)) {
        const QString stem = QFileInfo(name).completeBaseName();
        const int us = stem.indexOf(QLatin1Char('_'));
        if (us <= 0)
            continue;
        const QString depotId = stem.left(us);
        const QString mid = stem.mid(us + 1);
        bool ok1 = false, ok2 = false;
        depotId.toLongLong(&ok1);
        mid.toLongLong(&ok2);
        if (!ok1 || !ok2)
            continue;
        // Staging *.manifest bytes are authoritative for anonymous -manifestfile downloads.
        // Lua setManifestid is often stale vs the zip (and useless without the file).
        parsed.manifests.insert(depotId, mid);
    }
}

void filterPrimaryDepots(const QString& appid, LuaParsed& parsed, bool includeDlc)
{
    QMap<QString, QString> depots = parsed.depots;
    // App unlock key is not a content depot - downloading it needs no local manifest and fails.
    depots.remove(appid);
    if (!includeDlc) {
        // Soft: keep all remaining keyed depots (DLC filtering needs richer metadata).
    }
    if (depots.isEmpty() && !parsed.manifests.isEmpty()) {
        for (auto it = parsed.manifests.begin(); it != parsed.manifests.end(); ++it) {
            if (it.key() != appid)
                depots.insert(it.key(), QString());
        }
    }
    parsed.depots = depots;
}

void ensureInjectBestEffort(const QString& steam, const QString& pluginRoot)
{
    if (steam.isEmpty() || !QDir(steam).exists())
        return;
    QDir().mkpath(steam + QStringLiteral("/config/stplug-in"));
#if !defined(Q_OS_WIN)
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
#else
    Q_UNUSED(pluginRoot);
#endif
}

qint64 dirByteSize(const QString& path);

QString findStagingManifest(const QString& staging, const QString& depotId, const QString& manifestId)
{
    if (!manifestId.isEmpty()) {
        const QString exact =
            staging + QLatin1Char('/') + depotId + QLatin1Char('_') + manifestId
            + QStringLiteral(".manifest");
        if (QFileInfo::exists(exact))
            return exact;
    }
    const QStringList matches =
        QDir(staging).entryList({depotId + QStringLiteral("_*.manifest")}, QDir::Files, QDir::Name);
    if (!matches.isEmpty())
        return staging + QLatin1Char('/') + matches.first();
    return {};
}

struct DepotkitProgressBridge {
    std::function<void(const arachnel::core::OwnedDownloadProgress&)> onProgress;
    std::function<bool()> shouldCancel;
    depotkit_control* ctrl = nullptr;
};

void depotkitProgressThunk(const depotkit_progress* p, void* user)
{
    auto* bridge = static_cast<DepotkitProgressBridge*>(user);
    if (!bridge || !p)
        return;
    if (bridge->shouldCancel && bridge->shouldCancel() && bridge->ctrl)
        depotkit_cancel(bridge->ctrl);

    int pct = 0;
    if (p->bytes_total > 0)
        pct = qBound(1, 99, int((p->bytes_done * 100) / p->bytes_total));
    if (p->phase == DEPOTKIT_PHASE_DONE)
        pct = 99;
    else if (p->phase == DEPOTKIT_PHASE_ERROR || p->phase == DEPOTKIT_PHASE_CANCELLED)
        pct = qMax(1, pct);

    const QString detail = p->detail ? QString::fromUtf8(p->detail) : QStringLiteral("Downloading...");
    emitProgress(bridge->onProgress, pct, detail, QStringLiteral("downloading"),
                 static_cast<qint64>(p->bytes_done), static_cast<qint64>(p->bytes_total),
                 static_cast<int>(p->rate_bps));
}

void runDepotkit(const QString& appid, const LuaParsed& parsed, const QString& dest,
                 const QString& staging, const QString& depotOs, bool forceUpdate,
                 const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress,
                 const std::function<bool()>& shouldCancel)
{
    QDir().mkpath(dest);

    QStringList depotIds = parsed.depots.keys();
    if (depotIds.isEmpty())
        depotIds = parsed.manifests.keys();
    depotIds.removeAll(appid);
    if (depotIds.isEmpty())
        throw std::runtime_error("No depot IDs - cannot download");

    uint32_t wantFlags = 1u; // windows
    if (depotOs.compare(QStringLiteral("linux"), Qt::CaseInsensitive) == 0)
        wantFlags = 2u;
    else if (depotOs.compare(QStringLiteral("macos"), Qt::CaseInsensitive) == 0
             || depotOs.compare(QStringLiteral("osx"), Qt::CaseInsensitive) == 0)
        wantFlags = 4u;
#if !defined(Q_OS_WIN)
    if (depotOs.isEmpty() || depotOs.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0
        || depotOs.compare(QStringLiteral("windows"), Qt::CaseInsensitive) == 0) {
        // Linux hosts still usually want Windows depots for Proton.
        wantFlags = 1u;
    }
#endif

    struct Cand {
        QString depotId;
        QString key;
        QString manifestPath;
        uint32_t plat = 0;
    };
    std::vector<Cand> cands;
    cands.reserve(static_cast<size_t>(depotIds.size()));

    for (const QString& depotId : depotIds) {
        const QString key = parsed.depots.value(depotId);
        if (key.size() != 64)
            continue; // unlock-only / incomplete
        const QString mf =
            findStagingManifest(staging, depotId, parsed.manifests.value(depotId));
        if (mf.isEmpty())
            continue; // anonymous CDN cannot fetch without local manifest
        Cand c;
        c.depotId = depotId;
        c.key = key.toLower();
        c.manifestPath = mf;
        const QByteArray mfUtf8 = QDir::toNativeSeparators(mf).toUtf8();
        const QByteArray keyUtf8 = c.key.toUtf8();
        uint32_t flags = 0;
        if (depotkit_manifest_platform_hint(mfUtf8.constData(), keyUtf8.constData(), &flags)
            == DEPOTKIT_OK)
            c.plat = flags;
        cands.push_back(std::move(c));
    }

    if (cands.empty())
        throw std::runtime_error(
            "No downloadable depots with local manifests (anonymous CDN needs them)");

    std::vector<Cand> matched;
    for (const Cand& c : cands) {
        if (c.plat == 0 || (c.plat & wantFlags))
            matched.push_back(c);
    }
    // If nothing matched the OS hint (weird manifests), fall back to all with files.
    if (matched.empty())
        matched = cands;
    // Prefer exclusive OS matches when we have mixed multi-depot packages.
    std::vector<Cand> exclusive;
    for (const Cand& c : matched) {
        if (c.plat == wantFlags)
            exclusive.push_back(c);
    }
    if (!exclusive.empty())
        matched = exclusive;

    // Keep QString storage alive for C string pointers passed into depotkit.
    std::vector<QByteArray> pathBytes;
    std::vector<QByteArray> keyBytes;
    pathBytes.reserve(matched.size());
    keyBytes.reserve(matched.size());
    std::vector<depotkit_depot> depots;
    depots.reserve(matched.size());

    for (const Cand& c : matched) {
        pathBytes.push_back(QDir::toNativeSeparators(c.manifestPath).toUtf8());
        keyBytes.push_back(c.key.toUtf8());
        depotkit_depot d{};
        d.depot_id = c.depotId.toUInt();
        d.manifest_path = pathBytes.back().constData();
        d.key_hex = keyBytes.back().constData();
        depots.push_back(d);
    }

    const QByteArray installDir = QDir::toNativeSeparators(dest).toUtf8();
    depotkit_request req{};
    req.app_id = appid.toUInt();
    req.depots = depots.data();
    req.depot_count = depots.size();
    req.install_dir = installDir.constData();
    req.max_downloads = 64;
    req.validate = forceUpdate ? 1 : 0;
    req.cell_id = 0;

    depotkit_control* ctrl = depotkit_control_create();
    if (!ctrl)
        throw std::runtime_error("depotkit_control_create failed");

    DepotkitProgressBridge bridge;
    bridge.onProgress = onProgress;
    bridge.shouldCancel = shouldCancel;
    bridge.ctrl = ctrl;

    if (shouldCancel && shouldCancel()) {
        depotkit_control_destroy(ctrl);
        throw std::runtime_error("Cancelled");
    }

    emitProgress(onProgress, 1,
                 QStringLiteral("Steam CDN · %1 depot(s)…").arg(depots.size()));
    const depotkit_result rc = depotkit_download(&req, depotkitProgressThunk, &bridge, ctrl);
    depotkit_control_destroy(ctrl);

    if (rc == DEPOTKIT_ERR_CANCELLED || (shouldCancel && shouldCancel()))
        throw std::runtime_error("Cancelled");
    if (rc != DEPOTKIT_OK) {
        throw std::runtime_error(
            QStringLiteral("depotkit failed: %1")
                .arg(QString::fromUtf8(depotkit_strerror(rc)))
                .toStdString());
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
        // Local *.manifest from Ryuu are required: anonymous Steam returns 401 without
        // -manifestfile. Staging filenames override any lua setManifestid pins.
        adoptStagingManifests(staging.path(), parsed);
        filterPrimaryDepots(appid, parsed, settings.includeDlc);
        if (!steam.isEmpty()) {
            QDir().mkpath(steam + QStringLiteral("/config/stplug-in"));
            QFile luaOut(steam + QStringLiteral("/config/stplug-in/%1.lua").arg(appid));
            if (luaOut.open(QIODevice::WriteOnly | QIODevice::Truncate))
                luaOut.write(lua.toUtf8());
        }

        emitProgress(onProgress, 1,
                     force ? QStringLiteral("Updating…") : QStringLiteral("Downloading..."));
        QString depotOs = settings.depotOs.trimmed();
        if (depotOs.isEmpty() || depotOs.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0)
            depotOs = QStringLiteral("windows");
        runDepotkit(appid, parsed, installPath, staging.path(), depotOs, force, onProgress,
                    shouldCancel);

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
                                {QStringLiteral("mode"), QStringLiteral("depotkit")},
                            {QStringLiteral("launch"), launch}};
        QJsonObject manifestsObj;
        for (auto it = parsed.manifests.begin(); it != parsed.manifests.end(); ++it)
            manifestsObj.insert(it.key(), it.value());
        if (!manifestsObj.isEmpty())
            payload.insert(QStringLiteral("manifests"), manifestsObj);
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

        qInfo().noquote() << "[steamidra] installed" << appid << "force" << force
                          << "manifests" << parsed.manifests.keys().join(QLatin1Char(','));
        emitProgress(onProgress, 100, QStringLiteral("Installed"), QStringLiteral("completed"));
        result.ok = true;
        result.installPath = installPath;
        result.mode = QStringLiteral("depotkit");
        result.launch = launch;
        return result;
    } catch (const std::exception& ex) {
        result.ok = false;
        result.error = QString::fromUtf8(ex.what());
        return result;
    }
}

} // namespace steamidra
