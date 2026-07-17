#include "worker_bridge.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>

namespace steamidra {

namespace {

QString pythonExecutable()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("python");
#else
    return QStringLiteral("python3");
#endif
}

} // namespace

WorkerResult runWorker(const QString& pluginRoot, const QStringList& args, int timeoutMs)
{
    WorkerResult out;
    const QString script = pluginRoot + QStringLiteral("/worker/steamidra_worker.py");
    QProcess process;
    process.setProgram(pythonExecutable());
    QStringList fullArgs;
    fullArgs << script << args;
    process.setArguments(fullArgs);
    process.setWorkingDirectory(pluginRoot);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process.setProcessEnvironment(env);
    process.start();
    if (!process.waitForStarted(10000)) {
        out.error = QStringLiteral("Failed to start python worker");
        return out;
    }
    if (timeoutMs > 0) {
        if (!process.waitForFinished(timeoutMs)) {
            process.kill();
            out.error = QStringLiteral("Worker timed out");
            return out;
        }
    } else {
        process.waitForFinished(-1);
    }
    out.exitCode = process.exitCode();
    out.stdoutBytes = process.readAllStandardOutput();
    out.stderrBytes = process.readAllStandardError();
    out.ok = (process.exitStatus() == QProcess::NormalExit && out.exitCode == 0);
    if (!out.ok && out.error.isEmpty()) {
        out.error = QString::fromUtf8(out.stderrBytes).trimmed();
        if (out.error.isEmpty())
            out.error = QStringLiteral("Worker failed (exit %1)").arg(out.exitCode);
    }
    return out;
}

WorkerResult runWorkerWithProgress(
    const QString& pluginRoot, const QStringList& args,
    const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress,
    const std::function<bool()>& shouldCancel, int timeoutMs)
{
    WorkerResult out;
    const QString script = pluginRoot + QStringLiteral("/worker/steamidra_worker.py");
    QProcess process;
    process.setProgram(pythonExecutable());
    QStringList fullArgs;
    fullArgs << script << QStringLiteral("--progress-json") << args;
    process.setArguments(fullArgs);
    process.setWorkingDirectory(pluginRoot);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000)) {
        out.error = QStringLiteral("Failed to start python worker");
        return out;
    }

    QByteArray lineBuf;
    QByteArray resultJson;
    const auto flushLines = [&](const QByteArray& chunk) {
        lineBuf += chunk;
        while (true) {
            const int nl = lineBuf.indexOf('\n');
            if (nl < 0)
                break;
            const QByteArray line = lineBuf.left(nl).trimmed();
            lineBuf.remove(0, nl + 1);
            if (line.isEmpty())
                continue;
            if (line.startsWith("RESULT:")) {
                resultJson = line.mid(7);
                continue;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject())
                continue;
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("type")).toString() != QStringLiteral("progress"))
                continue;
            arachnel::core::OwnedDownloadProgress p;
            p.percent = obj.value(QStringLiteral("percent")).toInt();
            p.bytesDownloaded =
                static_cast<qint64>(obj.value(QStringLiteral("bytesDownloaded")).toDouble());
            p.totalBytes = static_cast<qint64>(obj.value(QStringLiteral("totalBytes")).toDouble());
            p.downloadRateBps = obj.value(QStringLiteral("downloadRateBps")).toInt();
            p.detail = obj.value(QStringLiteral("detail")).toString();
            p.status = obj.value(QStringLiteral("status")).toString(QStringLiteral("downloading"));
            if (onProgress)
                onProgress(p);
        }
    };

    const qint64 deadline =
        timeoutMs > 0 ? QDateTime::currentMSecsSinceEpoch() + timeoutMs : 0;
    while (process.state() != QProcess::NotRunning) {
        if (shouldCancel && shouldCancel()) {
            process.kill();
            out.error = QStringLiteral("Cancelled");
            out.ok = false;
            return out;
        }
        if (deadline > 0 && QDateTime::currentMSecsSinceEpoch() > deadline) {
            process.kill();
            out.error = QStringLiteral("Worker timed out");
            return out;
        }
        process.waitForReadyRead(20);
        flushLines(process.readAllStandardOutput());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    flushLines(process.readAllStandardOutput());
    out.exitCode = process.exitCode();
    out.stderrBytes = process.readAllStandardError();
    if (!resultJson.isEmpty())
        out.stdoutBytes = resultJson;
    else
        out.stdoutBytes = lineBuf;

    out.ok = (process.exitStatus() == QProcess::NormalExit && out.exitCode == 0);
    if (!out.ok && out.error.isEmpty()) {
        out.error = QString::fromUtf8(out.stderrBytes).trimmed();
        if (out.error.isEmpty())
            out.error = QStringLiteral("Worker failed (exit %1)").arg(out.exitCode);
    }
    return out;
}

QJsonObject parseWorkerJson(const WorkerResult& result)
{
    QByteArray bytes = result.stdoutBytes.trimmed();
    // Skip any leading noise; keep the first JSON object (not the last nested '{').
    const int firstBrace = bytes.indexOf('{');
    if (firstBrace > 0)
        bytes = bytes.mid(firstBrace);
    else if (firstBrace < 0)
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

} // namespace steamidra
