#pragma once

#include "plugin_interface.h"

#include <depotkit.h>

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QRecursiveMutex>
#include <QSet>
#include <QString>
#include <QVector>

namespace steamidra {

class SteamidraPlugin final : public arachnel::core::ISourcePlugin
{
public:
    explicit SteamidraPlugin(QString rootPath);

    QString id() const override;
    QString name() const override;
    QString description() const override;
    QString version() const override;
    QStringList capabilities() const override;

    QVector<arachnel::core::CatalogEntry> catalog() const override;
    QVector<arachnel::core::CatalogEntry> search(const QString& query) const override;
    std::optional<arachnel::core::CatalogEntry> entryById(const QString& entryId) const override;

    arachnel::core::InstallResult installFromDownload(
        const arachnel::core::InstallContext& ctx) const override;

    arachnel::core::InstallResult installAddonFromDownload(
        const arachnel::core::AddonInstallContext& ctx) const override;

    arachnel::core::InstallAnalysis analyzeDownload(
        const arachnel::core::InstallContext& ctx) const override;

    arachnel::core::InstallAnalysis analyzeFileNames(const QStringList& fileNames) const override;

    std::optional<QString> detectUpdate(const arachnel::core::LibraryGame& local,
                                        const arachnel::core::CatalogEntry& remote) const override;

    arachnel::core::LaunchInfo launchInfo(const arachnel::core::LibraryGame& local) const override;

    void resetCatalogCache() override;

    arachnel::core::InstallResult startOwnedDownload(
        const arachnel::core::InstallContext& ctx,
        const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress)
        const override;

    void cancelOwnedDownload(const QString& jobId) const override;
    void setOwnedDownloadPaused(const QString& jobId, bool paused) const override;

    bool applySelectedDlc(const arachnel::core::LibraryGame& local,
                          const QStringList& enabledAddonIds) const override;

    QVector<arachnel::core::GameLaunchOption> launchOptions(
        const arachnel::core::LibraryGame& local) const override;

    /** Sync on-disk catalog and return raw UTF-8 JSON (no in-memory row mirror). */
    QByteArray catalogJsonUtf8() const;

    void releaseCatalogMemory() const;

private:
    QString settingsPath() const;
    QString catalogCachePath() const;
    void ensureCatalogSeeded() const;

    QString m_rootPath;
    mutable QVector<arachnel::core::CatalogEntry> m_catalog;
    mutable bool m_catalogLoaded = false;
    mutable bool m_forceCatalogResync = false;
    mutable QDateTime m_catalogLoadedAt;
    mutable QRecursiveMutex m_catalogMutex;
    mutable QMutex m_cancelMutex;
    mutable QSet<QString> m_cancelledJobs;
    mutable QMutex m_downloadCtrlMutex;
    mutable QHash<QString, depotkit_control*> m_downloadControls;
};

} // namespace steamidra
