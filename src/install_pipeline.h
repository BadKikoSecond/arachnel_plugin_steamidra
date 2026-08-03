#pragma once

#include "plugin_settings.h"

#include "plugin_interface.h"

#include <depotkit.h>

#include <QJsonObject>
#include <QString>
#include <functional>

namespace steamidra {

struct InstallPipelineResult {
    bool ok = false;
    QString error;
    QString installPath;
    QString mode;
    QJsonObject launch;
    QJsonObject onlineFix;
    bool skippedDownload = false;
};

InstallPipelineResult runInstallPipeline(
    const QString& pluginRoot, const QString& appid, const QString& title, const QString& targetPath,
    const PluginSettings& settings, bool force,
    const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress,
    const std::function<bool()>& shouldCancel, depotkit_control* downloadCtrl = nullptr);

bool installAddonZip(const QString& zipPath, const QString& destPath, QString* errorOut);

QString findSteamPath(const PluginSettings& settings);

} // namespace steamidra
