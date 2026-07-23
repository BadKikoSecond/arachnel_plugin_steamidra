#include "steamidra_plugin.h"

#include "worker_bridge.h"

#include "catalog_types.h"
#include "install_analysis.h"
#include "install_kind.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QUrl>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

namespace steamidra {

namespace {

constexpr auto kSourceId = "steamidra";

QString steamExecutablePath()
{
#if defined(Q_OS_WIN)
    auto fromReg = [](HKEY hive, const wchar_t* subkey, const wchar_t* valueName) -> QString {
        HKEY key = nullptr;
        if (RegOpenKeyExW(hive, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return {};
        wchar_t buf[512];
        DWORD size = sizeof(buf);
        DWORD type = 0;
        const LONG ok = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
        RegCloseKey(key);
        if (ok != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            return {};
        QString path = QString::fromWCharArray(buf).trimmed();
        if (path.isEmpty())
            return {};
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QFileInfo exe(path);
        if (exe.isFile())
            return exe.absoluteFilePath();
        const QFileInfo asDir(path + QStringLiteral("/steam.exe"));
        if (asDir.isFile())
            return asDir.absoluteFilePath();
        return {};
    };
    if (const QString p = fromReg(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe"); !p.isEmpty())
        return p;
    if (const QString p = fromReg(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"); !p.isEmpty())
        return p;
    if (const QString p = fromReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
        !p.isEmpty())
        return p;
    if (const QString p = fromReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"); !p.isEmpty())
        return p;
#endif
    const QString found = QStandardPaths::findExecutable(QStringLiteral("steam"));
    if (!found.isEmpty())
        return found;
    return {};
}

arachnel::core::CatalogItemKind itemKindFromString(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("dlc"))
        return arachnel::core::CatalogItemKind::Dlc;
    if (normalized == QStringLiteral("addon") || normalized == QStringLiteral("add-on"))
        return arachnel::core::CatalogItemKind::Addon;
    return arachnel::core::CatalogItemKind::Game;
}

arachnel::core::CatalogComponent addonFromJson(const QJsonObject& obj, const QString& parentId)
{
    arachnel::core::CatalogComponent component;
    const QString title = obj.value(QStringLiteral("title")).toString();
    component.title = title;
    component.id = obj.value(QStringLiteral("id")).toString();
    if (component.id.isEmpty()) {
        component.id = arachnel::core::slugifyCatalogId(
            title, QString::fromLatin1(kSourceId) + QStringLiteral("-addon"));
    }
    component.fileSize = obj.value(QStringLiteral("fileSize")).toString();
    component.uploadDate = obj.value(QStringLiteral("uploadDate")).toString();
    component.kind = itemKindFromString(obj.value(QStringLiteral("kind")).toString());
    const QString delivery = obj.value(QStringLiteral("delivery")).toString().trimmed().toLower();
    component.delivery = delivery == QStringLiteral("direct")
                             ? arachnel::core::ComponentDelivery::Direct
                             : arachnel::core::ComponentDelivery::Magnet;
    component.referer = obj.value(QStringLiteral("referer")).toString();
    component.getfileUrl = obj.value(QStringLiteral("getfileUrl")).toString();
    component.optional = obj.value(QStringLiteral("optional")).toBool(true);

    const QJsonArray uris = obj.value(QStringLiteral("uris")).toArray();
    for (const QJsonValue& uri : uris) {
        const QString value = uri.toString();
        if (value.startsWith(QStringLiteral("magnet:"), Qt::CaseInsensitive))
            component.magnetUris.append(value);
        else if (value.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
                 || value.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
            component.downloadUrl = value;
    }
    if (component.id == parentId)
        component.id += QStringLiteral("-component");
    return component;
}

arachnel::core::CatalogEntry entryFromJson(const QJsonObject& obj)
{
    arachnel::core::CatalogEntry e;
    e.id = obj.value(QStringLiteral("id")).toString();
    e.title = obj.value(QStringLiteral("title")).toString();
    e.coverUrl = obj.value(QStringLiteral("coverUrl")).toString();
    e.sourceId = QString::fromLatin1(kSourceId);
    e.steamAppId = obj.value(QStringLiteral("steamAppId")).toString();
    e.description = obj.value(QStringLiteral("description")).toString();
    e.genres = obj.value(QStringLiteral("genres")).toString();
    e.sizeLabel = obj.value(QStringLiteral("sizeLabel")).toString();
    if (e.sizeLabel.isEmpty())
        e.sizeLabel = obj.value(QStringLiteral("fileSize")).toString();
    e.uploadDate = obj.value(QStringLiteral("uploadDate")).toString();
    e.version = obj.value(QStringLiteral("version")).toString();
    if (e.version.isEmpty() && !e.uploadDate.isEmpty())
        e.version = e.uploadDate.left(10);
    {
        const int rawKind = obj.value(QStringLiteral("installKind")).toInt(-1);
        if (rawKind >= static_cast<int>(arachnel::core::InstallKind::PortableArchive)
            && rawKind <= static_cast<int>(arachnel::core::InstallKind::FixDownload)) {
            e.installKind = static_cast<arachnel::core::InstallKind>(rawKind);
        } else if (obj.value(QStringLiteral("needsOnlineFix")).toBool()) {
            e.installKind = arachnel::core::InstallKind::BundledFix;
        } else {
            // SteaMidra downloads depots; SteamFix is auto-embedded only for online titles.
            e.installKind = arachnel::core::InstallKind::PortableArchive;
        }
    }
    e.itemKind = arachnel::core::CatalogItemKind::Game;
    if (e.id.isEmpty() && !e.steamAppId.isEmpty())
        e.id = QStringLiteral("steam-%1").arg(e.steamAppId);
    if (e.coverUrl.isEmpty() && !e.steamAppId.isEmpty()) {
        e.coverUrl = QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/header.jpg")
                         .arg(e.steamAppId);
    }

    const QJsonArray addons = obj.value(QStringLiteral("addons")).toArray();
    e.addons.reserve(addons.size());
    for (const QJsonValue& addonValue : addons) {
        if (!addonValue.isObject())
            continue;
        e.addons.append(addonFromJson(addonValue.toObject(), e.id));
    }
    return e;
}

} // namespace

SteamidraPlugin::SteamidraPlugin(QString rootPath)
    : m_rootPath(std::move(rootPath))
{
}

QString SteamidraPlugin::id() const
{
    return QString::fromLatin1(kSourceId);
}

QString SteamidraPlugin::name() const
{
    return QStringLiteral("Steam");
}

QString SteamidraPlugin::description() const
{
    return QStringLiteral("Steam depot installs via Arachnel Ryuu relay (no torrents)");
}

QString SteamidraPlugin::version() const
{
    return QStringLiteral("0.3.32");
}

QStringList SteamidraPlugin::capabilities() const
{
    return {QStringLiteral("search"), QStringLiteral("install"), QStringLiteral("update"),
            QStringLiteral("launch"), QStringLiteral("owns_download")};
}

QString SteamidraPlugin::workerScriptPath() const
{
    return m_rootPath + QStringLiteral("/worker/steamidra_worker.py");
}

QString SteamidraPlugin::settingsPath() const
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QStringLiteral("/plugins/steamidra");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/settings.json");
}

void SteamidraPlugin::resetCatalogCache()
{
    QMutexLocker lock(&m_catalogMutex);
    m_catalogLoaded = false;
    m_catalog.clear();
}

void SteamidraPlugin::ensureCatalogSeeded() const
{
    QMutexLocker lock(&m_catalogMutex);
    if (m_catalogLoaded && !m_catalog.isEmpty())
        return;

    m_catalog.clear();

    const auto appendGames = [this](const QJsonArray& games) {
        m_catalog.reserve(m_catalog.size() + games.size());
        for (const QJsonValue& v : games) {
            if (!v.isObject())
                continue;
            m_catalog.append(entryFromJson(v.toObject()));
        }
    };

    const auto loadCatalogFile = [&](const QString& path) -> bool {
        if (path.isEmpty())
            return false;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        QJsonArray games;
        if (doc.isArray()) {
            games = doc.array();
        } else if (doc.isObject()) {
            const QJsonObject root = doc.object();
            games = root.value(QStringLiteral("entries")).toArray();
            if (games.isEmpty())
                games = root.value(QStringLiteral("games")).toArray();
        }
        if (games.isEmpty())
            return false;
        appendGames(games);
        return !m_catalog.isEmpty();
    };

    const QString settingsDir = QFileInfo(settingsPath()).absolutePath();
    const QString localCache = settingsDir + QStringLiteral("/games-manifest-catalog.json");

    // Fast path: use local cache first (avoid blocking UI on a 20MB relay fetch).
    if (loadCatalogFile(localCache)) {
        m_catalogLoaded = true;
        return;
    }
    if (loadCatalogFile(m_rootPath + QStringLiteral("/games-manifest-catalog.json"))) {
        m_catalogLoaded = true;
        return;
    }

    // Sync from Ryuu relay (TTL inside worker; first fetch ~20MB).
    const WorkerResult sync =
        runWorker(m_rootPath,
                  {QStringLiteral("catalog-seed"), QStringLiteral("--settings"), settingsPath()},
                  600000);
    const QJsonObject syncObj = parseWorkerJson(sync);
    QString catalogPath = syncObj.value(QStringLiteral("catalogPath")).toString();
    if (!catalogPath.isEmpty() && !QFileInfo(catalogPath).isAbsolute())
        catalogPath = settingsDir + QLatin1Char('/') + catalogPath;
    if (loadCatalogFile(catalogPath) || loadCatalogFile(localCache)) {
        m_catalogLoaded = true;
        return;
    }
    // Do not fall back to a stale bundled GitLab catalog — relay only.
    appendGames(syncObj.value(QStringLiteral("games")).toArray());
    m_catalogLoaded = !m_catalog.isEmpty();
}

QVector<arachnel::core::CatalogEntry> SteamidraPlugin::catalog() const
{
    ensureCatalogSeeded();
    QMutexLocker lock(&m_catalogMutex);
    return m_catalog;
}

QVector<arachnel::core::CatalogEntry> SteamidraPlugin::search(const QString& query) const
{
    if (query.trimmed().isEmpty())
        return catalog();

    const WorkerResult r = runWorker(
        m_rootPath,
        {QStringLiteral("search"), QStringLiteral("--query"), query, QStringLiteral("--settings"),
         settingsPath()},
        120000);
    const QJsonObject obj = parseWorkerJson(r);
    QVector<arachnel::core::CatalogEntry> out;
    const QJsonArray games = obj.value(QStringLiteral("games")).toArray();
    for (const QJsonValue& v : games) {
        if (!v.isObject())
            continue;
        out.append(entryFromJson(v.toObject()));
    }
    return out;
}

std::optional<arachnel::core::CatalogEntry> SteamidraPlugin::entryById(const QString& entryId) const
{
    ensureCatalogSeeded();
    for (const auto& e : m_catalog) {
        if (e.id == entryId)
            return e;
    }
    QString appId = entryId;
    if (appId.startsWith(QStringLiteral("steam-")))
        appId = appId.mid(6);
    if (appId.isEmpty())
        return std::nullopt;

    const WorkerResult r = runWorker(
        m_rootPath,
        {QStringLiteral("app-details"), QStringLiteral("--appid"), appId, QStringLiteral("--settings"),
         settingsPath()},
        30000);
    const QJsonObject obj = parseWorkerJson(r);
    if (!obj.value(QStringLiteral("ok")).toBool())
        return std::nullopt;
    const QJsonObject game = obj.value(QStringLiteral("game")).toObject();
    if (game.isEmpty())
        return std::nullopt;
    return entryFromJson(game);
}

arachnel::core::InstallResult SteamidraPlugin::installFromDownload(
    const arachnel::core::InstallContext& ctx) const
{
    (void)ctx;
    arachnel::core::InstallResult result;
    result.success = false;
    result.error = QStringLiteral("Use owns_download path (startOwnedDownload)");
    return result;
}

arachnel::core::InstallResult SteamidraPlugin::installAddonFromDownload(
    const arachnel::core::AddonInstallContext& ctx) const
{
    arachnel::core::InstallResult result;
    if (ctx.gameInstallPath.isEmpty() || !QDir(ctx.gameInstallPath).exists()) {
        result.error = QStringLiteral("Install the game first");
        return result;
    }
    if (ctx.downloadPath.isEmpty() || !QFileInfo::exists(ctx.downloadPath)) {
        result.error = QStringLiteral("Add-on files not found");
        return result;
    }

    const WorkerResult r = runWorker(
        m_rootPath,
        {QStringLiteral("install-addon"), QStringLiteral("--zip"), ctx.downloadPath,
         QStringLiteral("--dest"), ctx.gameInstallPath},
        600000);
    const QJsonObject obj = parseWorkerJson(r);
    if (!r.ok || !obj.value(QStringLiteral("ok")).toBool()) {
        result.error = obj.value(QStringLiteral("error")).toString();
        if (result.error.isEmpty())
            result.error = r.error.isEmpty() ? QStringLiteral("Add-on install failed") : r.error;
        return result;
    }
    result.success = true;
    result.installPath = ctx.gameInstallPath;
    return result;
}

arachnel::core::InstallAnalysis SteamidraPlugin::analyzeDownload(
    const arachnel::core::InstallContext& ctx) const
{
    (void)ctx;
    // Never claim torrent/HTTP folders — install goes only through startOwnedDownload.
    return arachnel::core::makeInstallAnalysis(arachnel::core::InstallKind::BundledFix,
                                              QStringLiteral("steamidra-owned"), 0,
                                              QStringLiteral("Steam installs via owns_download only"),
                                              false);
}

arachnel::core::InstallAnalysis SteamidraPlugin::analyzeFileNames(
    const QStringList& fileNames) const
{
    (void)fileNames;
    return arachnel::core::makeInstallAnalysis(arachnel::core::InstallKind::BundledFix,
                                              QStringLiteral("steamidra-owned"), 50,
                                              QStringLiteral("SteaMidra does not install from torrents"),
                                              false);
}

std::optional<QString> SteamidraPlugin::detectUpdate(
    const arachnel::core::LibraryGame& local, const arachnel::core::CatalogEntry& remote) const
{
    if (remote.uploadDate.isEmpty())
        return std::nullopt;
    if (local.uploadDate.isEmpty())
        return remote.version.isEmpty() ? remote.uploadDate.left(10) : remote.version;

    const QDateTime remoteDt = QDateTime::fromString(remote.uploadDate, Qt::ISODate);
    const QDateTime localDt = QDateTime::fromString(local.uploadDate, Qt::ISODate);
    bool newer = false;
    if (remoteDt.isValid() && localDt.isValid())
        newer = remoteDt > localDt;
    else {
        const QDate remoteDay = QDate::fromString(remote.uploadDate.left(10), Qt::ISODate);
        const QDate localDay = QDate::fromString(local.uploadDate.left(10), Qt::ISODate);
        if (remoteDay.isValid() && localDay.isValid())
            newer = remoteDay > localDay;
        else
            newer = remote.uploadDate > local.uploadDate;
    }
    if (!newer)
        return std::nullopt;
    return remote.version.isEmpty() ? remote.uploadDate.left(10) : remote.version;
}

arachnel::core::LaunchInfo SteamidraPlugin::launchInfo(
    const arachnel::core::LibraryGame& local) const
{
    arachnel::core::LaunchInfo info;
    info.workingDirectory = local.installPath;

    auto fileExists = [](const QString& path) {
        return !path.isEmpty() && QFileInfo::exists(path);
    };

    auto applyOnlineFixOverrides = [&](arachnel::core::LaunchInfo& out) {
        const QString dirPath =
            out.workingDirectory.isEmpty() ? local.installPath : out.workingDirectory;
        const QDir dir(dirPath);
        if (dir.exists(QStringLiteral("winmm.dll")) || dir.exists(QStringLiteral("SteamFix.ini"))
            || dir.exists(QStringLiteral("SteamFix64.dll")))
            out.wineDllOverrides = QStringLiteral("winmm=n,b");
    };

    const QString overridePath = local.executableOverride.trimmed();
    if (fileExists(overridePath)) {
        info.executable = overridePath;
        info.workingDirectory = QFileInfo(overridePath).absolutePath();
        if (QFileInfo::exists(info.workingDirectory + QLatin1Char('/')
                              + QFileInfo(overridePath).completeBaseName()
                              + QStringLiteral(".pck")))
            info.arguments.append(QStringLiteral("--steam"));
        applyOnlineFixOverrides(info);
        return info;
    }

    QString markerExe;
    bool markerPrefersLocal = false;
    const QString markerPath = local.installPath + QStringLiteral("/.arachnel-steamidra");
    if (QFileInfo::exists(markerPath)) {
        QFile markerFile(markerPath);
        if (markerFile.open(QIODevice::ReadOnly)) {
            const QJsonObject root =
                QJsonDocument::fromJson(markerFile.readAll()).object();
            const QString mode = root.value(QStringLiteral("mode")).toString();
            if (mode == QStringLiteral("ddmod") || mode == QStringLiteral("already-installed")
                || root.value(QStringLiteral("onlineFix")).isObject())
                markerPrefersLocal = true;
            const QJsonObject launch = root.value(QStringLiteral("launch")).toObject();
            if (launch.value(QStringLiteral("type")).toString() == QStringLiteral("exe")) {
                markerExe = launch.value(QStringLiteral("path")).toString().trimmed();
                const QJsonArray args = launch.value(QStringLiteral("args")).toArray();
                for (const QJsonValue& a : args) {
                    if (a.isString() && !a.toString().isEmpty())
                        info.arguments.append(a.toString());
                }
            }
        }
    }
    if (fileExists(markerExe)) {
        info.executable = markerExe;
        info.workingDirectory = QFileInfo(markerExe).absolutePath();
        // Godot Steam builds need --steam (FreeTP Pratfall Launcher.bat).
        if (info.arguments.isEmpty()
            && QFileInfo::exists(QFileInfo(markerExe).absolutePath() + QLatin1Char('/')
                                 + QFileInfo(markerExe).completeBaseName() + QStringLiteral(".pck")))
            info.arguments.append(QStringLiteral("--steam"));
        applyOnlineFixOverrides(info);
        return info;
    }

    const QDir installDir(local.installPath);
    const bool hasFixOverlay = installDir.exists(QStringLiteral("winmm.dll"))
        || installDir.exists(QStringLiteral("SteamFix.ini"));

    // Owned depot installs (and anything with SteamFix) must start the local binary.
    // steam:// skips winmm inject and often runs GameLauncher stubs → crashes (Pratfall).
    if (markerPrefersLocal || hasFixOverlay || !local.installPath.isEmpty()) {
        if (!local.installPath.isEmpty()) {
            const QStringList exes =
                installDir.entryList({QStringLiteral("*.exe")}, QDir::Files, QDir::Name);
            QString best;
            qint64 bestScore = -1;
            for (const QString& name : exes) {
                const QString lower = name.toLower();
                if (lower.contains(QStringLiteral("crash"))
                    || lower.contains(QStringLiteral("unins"))
                    || lower.contains(QStringLiteral("redist"))
                    || lower.contains(QStringLiteral("prereq")))
                    continue;
                qint64 score = QFileInfo(installDir.filePath(name)).size();
                if (lower.contains(QStringLiteral("launcher"))
                    || lower.contains(QStringLiteral("bootstrap")))
                    score -= 50LL * 1024 * 1024;
                const QString stem = QFileInfo(name).completeBaseName();
                if (installDir.exists(stem + QStringLiteral(".pck")))
                    score += 200LL * 1024 * 1024;
                if (installDir.exists(stem + QStringLiteral("_Data")))
                    score += 200LL * 1024 * 1024;
                if (score > bestScore) {
                    bestScore = score;
                    best = installDir.filePath(name);
                }
            }
            if (!best.isEmpty()) {
                info.executable = best;
                const QString stem = QFileInfo(best).completeBaseName();
                if (installDir.exists(stem + QStringLiteral(".pck")))
                    info.arguments.append(QStringLiteral("--steam"));
                applyOnlineFixOverrides(info);
                return info;
            }
        }
    }

    QString appId;
    if (local.id.startsWith(QStringLiteral("steam-")))
        appId = local.id.mid(6);
    if (appId.isEmpty()) {
        appId = local.magnetUri;
        if (appId.startsWith(QStringLiteral("steam://app/")))
            appId = appId.mid(QStringLiteral("steam://app/").size());
    }
    if (!appId.isEmpty()) {
        const QString steamExe = steamExecutablePath();
        if (!steamExe.isEmpty()) {
            info.executable = steamExe;
            info.workingDirectory = QFileInfo(steamExe).absolutePath();
        } else {
            info.executable = QStringLiteral("steam");
        }
        info.arguments = {QStringLiteral("steam://rungameid/%1").arg(appId)};
    }
    return info;
}

arachnel::core::InstallResult SteamidraPlugin::startOwnedDownload(
    const arachnel::core::InstallContext& ctx,
    const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress) const
{
    arachnel::core::InstallResult result;
    {
        QMutexLocker lock(&m_cancelMutex);
        m_cancelledJobs.remove(ctx.jobId);
    }

    QString appId = ctx.magnetUri;
    if (appId.startsWith(QStringLiteral("steam://app/")))
        appId = appId.mid(QStringLiteral("steam://app/").size());
    if (appId.isEmpty() && ctx.entryId.startsWith(QStringLiteral("steam-")))
        appId = ctx.entryId.mid(6);
    if (appId.isEmpty()) {
        result.error = QStringLiteral("Missing Steam App ID");
        return result;
    }

    auto cancelled = [this, jobId = ctx.jobId]() {
        QMutexLocker lock(&m_cancelMutex);
        return m_cancelledJobs.contains(jobId);
    };

    if (onProgress) {
        arachnel::core::OwnedDownloadProgress p;
        p.percent = 0;
        p.status = QStringLiteral("downloading");
        p.detail = QStringLiteral("Downloading from Steam CDN…");
        onProgress(p);
    }

    const WorkerResult r = runWorkerWithProgress(
        m_rootPath,
        {QStringLiteral("install"), QStringLiteral("--appid"), appId, QStringLiteral("--title"),
         ctx.title, QStringLiteral("--target"), ctx.targetPath, QStringLiteral("--downloads"),
         ctx.downloadsPath, QStringLiteral("--settings"), settingsPath(),
         QStringLiteral("--mode"), QStringLiteral("ddmod")},
        onProgress, cancelled, 0);

    if (cancelled()) {
        result.error = QStringLiteral("Cancelled");
        return result;
    }

    const QJsonObject obj = parseWorkerJson(r);
    if (!r.ok || !obj.value(QStringLiteral("ok")).toBool()) {
        result.error = obj.value(QStringLiteral("error")).toString();
        if (result.error.isEmpty())
            result.error = r.error;
        return result;
    }

    result.success = true;
    result.installPath = obj.value(QStringLiteral("installPath")).toString();
    if (result.installPath.isEmpty())
        result.installPath = ctx.targetPath;
    return result;
}

void SteamidraPlugin::cancelOwnedDownload(const QString& jobId) const
{
    QMutexLocker lock(&m_cancelMutex);
    m_cancelledJobs.insert(jobId);
}

} // namespace steamidra
