#include "steamidra_plugin.h"

#include "catalog_sync.h"
#include "install_pipeline.h"
#include "online_fix_embed.h"
#include "plugin_settings.h"

#include "install_analysis.h"
#include "install_kind.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>

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
        const LONG ok =
            RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
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
    if (const QString p =
            fromReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
        !p.isEmpty())
        return p;
#endif
    return QStandardPaths::findExecutable(QStringLiteral("steam"));
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
    return QStringLiteral("Steam direct download via Ryuu manifests");
}

QString SteamidraPlugin::version() const
{
    const QString manifest = m_rootPath + QStringLiteral("/plugin.json");
    QFile file(manifest);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
        const QString ver = obj.value(QStringLiteral("version")).toString().trimmed();
        if (!ver.isEmpty())
            return ver;
    }
    return QStringLiteral("0.6.13");
}

QStringList SteamidraPlugin::capabilities() const
{
    return {QStringLiteral("search"), QStringLiteral("install"), QStringLiteral("update"),
            QStringLiteral("launch"), QStringLiteral("owns_download")};
}

QString SteamidraPlugin::settingsPath() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QStringLiteral("/plugins/steamidra");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/settings.json");
}

QString SteamidraPlugin::catalogCachePath() const
{
    return QFileInfo(settingsPath()).absolutePath()
           + QStringLiteral("/games-manifest-catalog.json");
}

void SteamidraPlugin::resetCatalogCache()
{
    QMutexLocker lock(&m_catalogMutex);
    m_catalogLoaded = false;
    m_catalogLoadedAt = {};
    m_forceCatalogResync = true;
    // Avoid CatalogEntry::~ across DSO when the host interposes a different SDK layout.
    if (!m_catalog.isEmpty()) {
        auto* abandoned = new QVector<arachnel::core::CatalogEntry>();
        m_catalog.swap(*abandoned);
    }
    QFile::remove(catalogCachePath());
}

void SteamidraPlugin::releaseCatalogMemory() const
{
    QMutexLocker lock(&m_catalogMutex);
    m_catalog.clear();
    m_catalog.squeeze();
    m_catalogLoaded = false;
    m_catalogLoadedAt = {};
}

QByteArray SteamidraPlugin::catalogJsonUtf8() const
{
    const QString localCache = catalogCachePath();
    bool forceResync = false;
    {
        QMutexLocker lock(&m_catalogMutex);
        forceResync = m_forceCatalogResync;
        m_forceCatalogResync = false;
        if (forceResync) {
            m_catalog.clear();
            m_catalog.squeeze();
            m_catalogLoaded = false;
            m_catalogLoadedAt = {};
        }
    }

    const PluginSettings settings = loadPluginSettings(settingsPath());
    const QString synced = syncManifestCatalog(settings, localCache, forceResync);
    QString path = synced;
    if (path.isEmpty() || !QFileInfo::exists(path))
        path = localCache;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    // Drop the in-memory mirror after serving JSON so UI threads do not stall on
    // QVector<CatalogEntry> copies while a catalog worker is active.
    releaseCatalogMemory();
    return file.readAll();
}

void SteamidraPlugin::ensureCatalogSeeded() const
{
    constexpr qint64 kMemoryTtlMs = 60 * 1000;
    {
        QMutexLocker lock(&m_catalogMutex);
        if (m_catalogLoaded && !m_catalog.isEmpty() && !m_forceCatalogResync
            && m_catalogLoadedAt.isValid()
            && m_catalogLoadedAt.msecsTo(QDateTime::currentDateTimeUtc()) < kMemoryTtlMs)
            return;
    }

    const QString localCache = catalogCachePath();
    bool forceResync = false;
    {
        QMutexLocker lock(&m_catalogMutex);
        forceResync = m_forceCatalogResync;
        m_forceCatalogResync = false;
        if (forceResync) {
            m_catalog.clear();
            m_catalogLoaded = false;
            m_catalogLoadedAt = {};
        }
    }

    const PluginSettings settings = loadPluginSettings(settingsPath());
    const QString synced = syncManifestCatalog(settings, localCache, forceResync);
    QVector<arachnel::core::CatalogEntry> entries = loadCatalogEntriesFromFile(synced);
    if (entries.isEmpty())
        entries = loadCatalogEntriesFromFile(localCache);

    QMutexLocker lock(&m_catalogMutex);
    if (!entries.isEmpty()) {
        m_catalog = std::move(entries);
        m_catalogLoaded = true;
        m_catalogLoadedAt = QDateTime::currentDateTimeUtc();
    }
}

QVector<arachnel::core::CatalogEntry> SteamidraPlugin::catalog() const
{
    ensureCatalogSeeded();
    QMutexLocker lock(&m_catalogMutex);
    return m_catalog;
}

QVector<arachnel::core::CatalogEntry> SteamidraPlugin::search(const QString& query) const
{
    const QString q = query.trimmed();
    if (q.isEmpty())
        return {};

    if (QRegularExpression(QStringLiteral("^\\d+$")).match(q).hasMatch()) {
        const QJsonObject game = steamAppDetailsJson(q);
        if (!game.isEmpty())
            return {catalogEntryFromJson(game)};
        return {};
    }

    ensureCatalogSeeded();
    QVector<arachnel::core::CatalogEntry> out;
    {
        QMutexLocker lock(&m_catalogMutex);
        const QString ql = q.toLower();
        for (const auto& e : m_catalog) {
            if (e.title.toLower().contains(ql) || e.steamAppId == q)
                out.append(e);
            if (out.size() >= 80)
                break;
        }
    }
    releaseCatalogMemory();
    return out;
}

std::optional<arachnel::core::CatalogEntry> SteamidraPlugin::entryById(const QString& entryId) const
{
    std::optional<arachnel::core::CatalogEntry> entry;
    {
        QMutexLocker lock(&m_catalogMutex);
        for (const auto& e : m_catalog) {
            if (e.id == entryId) {
                entry = e;
                break;
            }
        }
    }
    if (!entry) {
        QString appId = entryId;
        if (appId.startsWith(QStringLiteral("steam-")))
            appId = appId.mid(6);
        if (appId.isEmpty())
            return std::nullopt;
        const QJsonObject game = steamAppDetailsJson(appId);
        if (game.isEmpty())
            return std::nullopt;
        entry = catalogEntryFromJson(game);
    }
    // List rows keep addons empty on purpose - fill from relay before Arachnel shows the picker.
    const PluginSettings settings = loadPluginSettings(settingsPath());
    enrichCatalogEntryWithRelayDlcs(*entry, settings);
    return entry;
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
    QString err;
    if (!installAddonZip(ctx.downloadPath, ctx.gameInstallPath, &err)) {
        result.error = err;
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
    return arachnel::core::makeInstallAnalysis(arachnel::core::InstallKind::BundledFix,
                                              QStringLiteral("steamidra-owned"), 0,
                                              QStringLiteral("Steam installs via owns_download only"),
                                              false);
}

arachnel::core::InstallAnalysis SteamidraPlugin::analyzeFileNames(const QStringList& fileNames) const
{
    (void)fileNames;
    return arachnel::core::makeInstallAnalysis(arachnel::core::InstallKind::BundledFix,
                                              QStringLiteral("steamidra-owned"), 50,
                                              QStringLiteral("SteaMidra does not install from torrents"),
                                              false);
}

std::optional<QString> SteamidraPlugin::detectUpdate(const arachnel::core::LibraryGame& local,
                                                     const arachnel::core::CatalogEntry& remote) const
{
    const auto remoteLabel = [&]() -> QString {
        if (!remote.version.isEmpty())
            return remote.version;
        if (!remote.uploadDate.isEmpty())
            return remote.uploadDate.left(10);
        return {};
    };

    // 1) Steam buildId / catalog version changed (preferred over Ryuu updated_date).
    // Relays now set version=public buildid when known; date-only versions still work.
    if (!remote.version.isEmpty() && !local.version.isEmpty()
        && remote.version != local.version) {
        const bool remoteIsDate = QDate::fromString(remote.version.left(10), Qt::ISODate).isValid()
                                  && remote.version.size() <= 10;
        const bool localIsDate = QDate::fromString(local.version.left(10), Qt::ISODate).isValid()
                                 && local.version.size() <= 10;
        if (!(remoteIsDate && localIsDate)) {
            // buildId vs buildId, or buildId vs old date marker → treat as update.
            return remoteLabel();
        }
        // Both dates: fall through to uploadDate compare below.
    } else if (!remote.version.isEmpty() && local.version.isEmpty()) {
        return remoteLabel();
    }

    // 2) Classic uploadDate compare (Ryuu listing date - can lag behind real patches).
    if (!remote.uploadDate.isEmpty()) {
        if (local.uploadDate.isEmpty())
            return remoteLabel();

        const QDateTime remoteDt = QDateTime::fromString(remote.uploadDate, Qt::ISODate);
        const QDateTime localDt = QDateTime::fromString(local.uploadDate, Qt::ISODate);
        bool newer = false;
        if (remoteDt.isValid() && localDt.isValid())
            newer = remoteDt > localDt;
        else
            newer = remote.uploadDate > local.uploadDate;
        if (newer)
            return remoteLabel();
    }

    // 3) Date-only version compare when uploadDate missing/equal.
    if (!remote.version.isEmpty() && !local.version.isEmpty() && remote.version != local.version) {
        const QDate remoteDay = QDate::fromString(remote.version.left(10), Qt::ISODate);
        const QDate localDay = QDate::fromString(local.version.left(10), Qt::ISODate);
        if (remoteDay.isValid() && localDay.isValid() && remoteDay > localDay)
            return remoteLabel();
    }

    return std::nullopt;
}

arachnel::core::LaunchInfo SteamidraPlugin::launchInfo(const arachnel::core::LibraryGame& local) const
{
    arachnel::core::LaunchInfo info;
    info.workingDirectory = local.installPath;

    QString appId = local.steamAppId;
    if (appId.isEmpty() && local.id.startsWith(QStringLiteral("steam-")))
        appId = local.id.mid(6);
    if (appId.isEmpty())
        appId = local.magnetUri;
    if (appId.startsWith(QStringLiteral("steam://app/")))
        appId = appId.mid(QStringLiteral("steam://app/").size());
    repairOnlineFixBeforeLaunch(local.installPath, appId, m_rootPath);

    const QString markerPath = local.installPath + QStringLiteral("/.arachnel-steamidra");
    if (QFileInfo::exists(markerPath)) {
        QFile f(markerPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            const QJsonObject launch = root.value(QStringLiteral("launch")).toObject();
            const QString type = launch.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("exe")) {
                const QString path = launch.value(QStringLiteral("path")).toString();
                if (QFileInfo::exists(path)) {
                    info.executable = path;
                    info.workingDirectory = QFileInfo(path).absolutePath();
                    return info;
                }
            }
        }
    }

    // Prefer shipping / SteamFix-adjacent exe
    QString best;
    qint64 bestScore = -1;
    QDirIterator it(local.installPath, {QStringLiteral("*.exe")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString lower = it.fileName().toLower();
        if (lower.contains(QStringLiteral("crash")) || lower.contains(QStringLiteral("unins"))
            || lower.contains(QStringLiteral("redist")))
            continue;
        qint64 score = it.fileInfo().size();
        const QString parent = it.fileInfo().absolutePath();
        if (QFileInfo::exists(parent + QStringLiteral("/SteamFix64.dll"))
            || QFileInfo::exists(parent + QStringLiteral("/winmm.dll")))
            score += 500LL * 1024 * 1024;
        if (lower.contains(QStringLiteral("shipping")))
            score += 200LL * 1024 * 1024;
        if (score > bestScore) {
            bestScore = score;
            best = it.filePath();
        }
    }
    if (!best.isEmpty()) {
        info.executable = best;
        info.workingDirectory = QFileInfo(best).absolutePath();
        return info;
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

    depotkit_control* ctrl = depotkit_control_create();
    if (!ctrl) {
        result.error = QStringLiteral("depotkit_control_create failed");
        return result;
    }
    {
        QMutexLocker lock(&m_downloadCtrlMutex);
        m_downloadControls.insert(ctx.jobId, ctrl);
    }

    const PluginSettings settings = loadPluginSettings(settingsPath());
    const bool forceUpdate =
        ctx.installMode.compare(QStringLiteral("update"), Qt::CaseInsensitive) == 0
        || ctx.installMode.compare(QStringLiteral("force"), Qt::CaseInsensitive) == 0;
    const InstallPipelineResult pipe =
        runInstallPipeline(m_rootPath, appId, ctx.title, ctx.targetPath, settings, forceUpdate,
                           ctx.selectedAddonIds, onProgress, cancelled, ctrl);

    {
        QMutexLocker lock(&m_downloadCtrlMutex);
        m_downloadControls.remove(ctx.jobId);
    }
    depotkit_control_destroy(ctrl);

    if (cancelled()) {
        result.error = QStringLiteral("Cancelled");
        return result;
    }
    if (!pipe.ok) {
        result.error = pipe.error;
        return result;
    }
    result.success = true;
    result.installPath = pipe.installPath.isEmpty() ? ctx.targetPath : pipe.installPath;
    return result;
}

void SteamidraPlugin::cancelOwnedDownload(const QString& jobId) const
{
    QMutexLocker lock(&m_cancelMutex);
    m_cancelledJobs.insert(jobId);
    QMutexLocker ctrlLock(&m_downloadCtrlMutex);
    if (depotkit_control* ctrl = m_downloadControls.value(jobId, nullptr))
        depotkit_cancel(ctrl);
}

void SteamidraPlugin::setOwnedDownloadPaused(const QString& jobId, bool paused) const
{
    QMutexLocker lock(&m_downloadCtrlMutex);
    depotkit_control* ctrl = m_downloadControls.value(jobId, nullptr);
    if (!ctrl)
        return;
    if (paused)
        depotkit_pause(ctrl);
    else
        depotkit_resume(ctrl);
}

bool SteamidraPlugin::applySelectedDlc(const arachnel::core::LibraryGame& local,
                                       const QStringList& enabledAddonIds) const
{
    QString appId = local.steamAppId;
    if (appId.isEmpty() && local.id.startsWith(QStringLiteral("steam-")))
        appId = local.id.mid(6);
    if (appId.isEmpty() || local.installPath.isEmpty())
        return false;
    return applyDlcUnlocks(local.installPath, appId, enabledAddonIds);
}

} // namespace steamidra
