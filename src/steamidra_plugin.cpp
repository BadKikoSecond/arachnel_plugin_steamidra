#include "steamidra_plugin.h"

#include "worker_bridge.h"

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

namespace steamidra {

namespace {

constexpr auto kSourceId = "steamidra";

arachnel::core::CatalogEntry entryFromJson(const QJsonObject& obj)
{
    arachnel::core::CatalogEntry e;
    e.id = obj.value(QStringLiteral("id")).toString();
    e.title = obj.value(QStringLiteral("title")).toString();
    e.coverUrl = obj.value(QStringLiteral("coverUrl")).toString();
    e.sourceId = QString::fromLatin1(kSourceId);
    e.steamAppId = obj.value(QStringLiteral("steamAppId")).toString();
    e.description = obj.value(QStringLiteral("description")).toString();
    e.sizeLabel = obj.value(QStringLiteral("sizeLabel")).toString();
    if (e.sizeLabel.isEmpty())
        e.sizeLabel = obj.value(QStringLiteral("fileSize")).toString();
    e.uploadDate = obj.value(QStringLiteral("uploadDate")).toString();
    e.version = obj.value(QStringLiteral("version")).toString();
    if (e.version.isEmpty() && !e.uploadDate.isEmpty())
        e.version = e.uploadDate.left(10);
    e.installKind = arachnel::core::InstallKind::BundledFix;
    e.itemKind = arachnel::core::CatalogItemKind::Game;
    if (e.id.isEmpty() && !e.steamAppId.isEmpty())
        e.id = QStringLiteral("steam-%1").arg(e.steamAppId);
    if (e.coverUrl.isEmpty() && !e.steamAppId.isEmpty()) {
        e.coverUrl = QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/header.jpg")
                         .arg(e.steamAppId);
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
    return QStringLiteral("SteaMidra");
}

QString SteamidraPlugin::description() const
{
    return QStringLiteral(
        "Steam depot installs via LumaCore / SLSsteam + DepotDownloader (no torrents)");
}

QString SteamidraPlugin::version() const
{
    return QStringLiteral("0.1.0");
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
    m_catalogLoaded = false;
    m_catalog.clear();
}

void SteamidraPlugin::ensureCatalogSeeded() const
{
    if (m_catalogLoaded)
        return;
    m_catalogLoaded = true;
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
            if (games.isEmpty())
                games = root.value(QStringLiteral("downloads")).toArray();
        }
        if (games.isEmpty())
            return false;
        appendGames(games);
        return true;
    };

    // Installable manifest catalog (GitLab / local cache) — keys-only DB is fallback.
    const QString settingsDir = QFileInfo(settingsPath()).absolutePath();
    if (loadCatalogFile(settingsDir + QStringLiteral("/games-manifest-catalog.json")))
        return;
    if (loadCatalogFile(m_rootPath + QStringLiteral("/config/games-manifest-catalog.json")))
        return;

    // Legacy full depot-keys catalog (~90k, no manifest guarantee).
    if (loadCatalogFile(settingsDir + QStringLiteral("/games-depotkeys.json")))
        return;

    // First run: download public depot-key DB and build catalog (~90k apps).
    // 5 min covers a cold 65MB download + JSON parse on slow links.
    const WorkerResult r =
        runWorker(m_rootPath,
                  {QStringLiteral("catalog-seed"), QStringLiteral("--settings"), settingsPath()},
                  300000);
    const QJsonObject obj = parseWorkerJson(r);
    const QString catalogPath = obj.value(QStringLiteral("catalogPath")).toString();
    if (loadCatalogFile(catalogPath))
        return;
    if (loadCatalogFile(settingsDir + QStringLiteral("/games-manifest-catalog.json")))
        return;
    if (loadCatalogFile(settingsDir + QStringLiteral("/games-depotkeys.json")))
        return;
    appendGames(obj.value(QStringLiteral("games")).toArray());
}

QVector<arachnel::core::CatalogEntry> SteamidraPlugin::catalog() const
{
    ensureCatalogSeeded();
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
        60000);
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
    // steam-12345 style
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

arachnel::core::InstallAnalysis SteamidraPlugin::analyzeDownload(
    const arachnel::core::InstallContext& ctx) const
{
    (void)ctx;
    return arachnel::core::makeInstallAnalysis(arachnel::core::InstallKind::BundledFix,
                                              QStringLiteral("steamidra-owned"), 100,
                                              QStringLiteral("Plugin-owned Steam depot install"),
                                              true);
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
    QString appId;
    if (local.id.startsWith(QStringLiteral("steam-")))
        appId = local.id.mid(6);
    if (appId.isEmpty()) {
        // magnetUri may store steam app id for this plugin
        appId = local.magnetUri;
        if (appId.startsWith(QStringLiteral("steam://app/")))
            appId = appId.mid(QStringLiteral("steam://app/").size());
    }
    if (!appId.isEmpty()) {
        info.executable = QStringLiteral("steam");
        info.arguments = {QStringLiteral("steam://rungameid/%1").arg(appId)};
        info.workingDirectory = local.installPath;
    } else if (!local.executableOverride.isEmpty()) {
        info.executable = local.executableOverride;
        info.workingDirectory = local.installPath;
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

    QString appId = ctx.magnetUri; // we pass steamAppId here from core
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
        p.detail = QStringLiteral("Preparing…");
        onProgress(p);
    }

    // Mode: auto (native then ddmod), or forced via settings
    const WorkerResult r = runWorkerWithProgress(
        m_rootPath,
        {QStringLiteral("install"), QStringLiteral("--appid"), appId, QStringLiteral("--title"),
         ctx.title, QStringLiteral("--target"), ctx.targetPath, QStringLiteral("--downloads"),
         ctx.downloadsPath, QStringLiteral("--settings"), settingsPath(),
         QStringLiteral("--mode"), QStringLiteral("auto")},
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
