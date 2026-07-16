#pragma once

#include "plugin_interface.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace steamidra {

struct WorkerResult {
    bool ok = false;
    int exitCode = -1;
    QByteArray stdoutBytes;
    QByteArray stderrBytes;
    QString error;
};

/** Run bundled Python worker: python3 worker/steamidra_worker.py <cmd> ... */
WorkerResult runWorker(const QString& pluginRoot, const QStringList& args,
                       int timeoutMs = 120000);

WorkerResult runWorkerWithProgress(
    const QString& pluginRoot, const QStringList& args,
    const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress,
    const std::function<bool()>& shouldCancel, int timeoutMs = 0);

QJsonObject parseWorkerJson(const WorkerResult& result);

} // namespace steamidra
