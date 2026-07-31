#include "install_pipeline.h"

#include "http_client.h"
#include "online_fix_embed.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
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

    // Ryuu / Hubcap often leave stale setManifestid lines commented with `--`.
    // Matching those pins DepotDownloader to an old build (e.g. Meccha 3.0.0 vs 3.3.0).
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

void clearTempManifestPollution(const QStringList& depotIds)
{
    const QString tmp = QDir::tempPath();
    for (const QString& depotId : depotIds) {
        QFile::remove(tmp + QStringLiteral("/arachnel_dd_%1.manifest").arg(depotId));
        for (const QString& name :
             QDir(tmp).entryList({depotId + QStringLiteral("_*.manifest")}, QDir::Files)) {
            QFile::remove(tmp + QLatin1Char('/') + name);
        }
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

bool processIoBytes(qint64 pid, qint64* readOut, qint64* writeOut);
qint64 dirByteSize(const QString& path);

void runDdmod(const QString& pluginRoot, const QString& appid, const LuaParsed& parsed,
              const QString& dest, const QString& depotOs, const QString& staging, bool forceUpdate,
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
    const QString keysTmp = staging + QStringLiteral("/depotkeys.vdf");
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

    // DepotDownloaderMod may auto-bind leftover %TEMP%/<depot>_*.manifest → anonymous 401.
    clearTempManifestPollution(depots);

    const int depotCount = qMax(1, depots.size());
    qint64 completedWritten = 0;
    qint64 totalHint = 0;
    QElapsedTimer sizeTimer;
    sizeTimer.start();

    for (int i = 0; i < depots.size(); ++i) {
        if (shouldCancel && shouldCancel())
            throw std::runtime_error("Cancelled");
        const QString depotId = depots.at(i);
        const int basePct = qMax(1, int(99.0 * double(i) / double(depotCount)));
        emitProgress(onProgress, basePct, QStringLiteral("Downloading depot %1…").arg(depotId),
                     QStringLiteral("downloading"), completedWritten, totalHint);

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
        if (forceUpdate)
            args << QStringLiteral("-validate");
        if (parsed.manifests.contains(depotId)) {
            const QString mid = parsed.manifests.value(depotId);
            args << QStringLiteral("-manifest") << mid;
            const QString stagingMf =
                staging + QLatin1Char('/') + depotId + QLatin1Char('_') + mid
                + QStringLiteral(".manifest");
            if (QFileInfo::exists(stagingMf))
                args << QStringLiteral("-manifestfile") << stagingMf;
        }

        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DOTNET_ROOT"), QFileInfo(dotnet).absolutePath());
        proc.setProcessEnvironment(env);
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(dotnet, args);
        if (!proc.waitForStarted(30000))
            throw std::runtime_error("Failed to start DepotDownloaderMod");

        qint64 readStart = 0, writeStart = 0;
        const bool ioOk = processIoBytes(proc.processId(), &readStart, &writeStart);
        qint64 procWrittenMax = 0;
        double depotPeakLocal = 0.0;
        QElapsedTimer emitTimer;
        emitTimer.start();

        static const QRegularExpression pctRe(QStringLiteral(R"(^\s*(\d+(?:\.\d+)?)\s*%)"));
        QByteArray log;
        while (proc.state() != QProcess::NotRunning) {
            if (shouldCancel && shouldCancel()) {
                proc.kill();
                throw std::runtime_error("Cancelled");
            }
            proc.waitForReadyRead(200);
            const QByteArray chunk = proc.readAll();
            if (!chunk.isEmpty())
                log += chunk;
            for (const QByteArray& line : chunk.split('\n')) {
                const auto m = pctRe.match(QString::fromUtf8(line));
                if (m.hasMatch())
                    depotPeakLocal = qMax(depotPeakLocal, qMin(100.0, m.captured(1).toDouble()));
            }

            if (emitTimer.elapsed() < 400)
                continue;
            emitTimer.restart();

            if (ioOk) {
                qint64 readNow = 0, writeNow = 0;
                if (processIoBytes(proc.processId(), &readNow, &writeNow)) {
                    Q_UNUSED(readNow);
                    procWrittenMax = qMax(procWrittenMax, qMax<qint64>(0, writeNow - writeStart));
                }
            }
            // Logical size after SetLength = good total; do not use as downloaded.
            // Never max(total, downloaded) - that collapsed total≈downloaded and stuck at 99%.
            if (sizeTimer.elapsed() >= 1000 || totalHint <= 0) {
                totalHint = qMax(totalHint, dirByteSize(dest));
                sizeTimer.restart();
            }
            const qint64 downloaded = completedWritten + procWrittenMax;
            int overall =
                qBound(1, 99, int(99.0 * (double(i) + depotPeakLocal / 100.0) / double(depotCount)));
            if (totalHint >= 64LL * 1024 * 1024 && downloaded > 0 && totalHint > downloaded
                && (totalHint - downloaded)
                       >= qMax<qint64>(1024LL * 1024, totalHint / 50)) {
                const int ratioPct = qBound(1, 99, int(downloaded * 100 / totalHint));
                overall = qMax(overall, ratioPct);
            }
            emitProgress(onProgress, overall, QStringLiteral("Downloading..."),
                         QStringLiteral("downloading"), downloaded, totalHint);
        }
        log += proc.readAll();
        if (ioOk) {
            qint64 readNow = 0, writeNow = 0;
            if (processIoBytes(proc.processId(), &readNow, &writeNow))
                procWrittenMax = qMax(procWrittenMax, qMax<qint64>(0, writeNow - writeStart));
        }
        completedWritten += procWrittenMax;
        totalHint = qMax(totalHint, dirByteSize(dest));

        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
            const QString full = QString::fromUtf8(log);
            QString err;
            for (const QString& line : full.split(QLatin1Char('\n'))) {
                const QString t = line.trimmed();
                if (t.contains(QStringLiteral("401"), Qt::CaseInsensitive)
                    || t.contains(QStringLiteral("Unable"), Qt::CaseInsensitive)
                    || t.contains(QStringLiteral("Error"), Qt::CaseInsensitive)
                    || t.contains(QStringLiteral("failed"), Qt::CaseInsensitive)
                    || t.contains(QStringLiteral("Aborting"), Qt::CaseInsensitive)) {
                    err += t + QLatin1Char(' ');
                }
            }
            if (err.isEmpty())
                err = full.trimmed();
            err = err.simplified().right(600);
            if (err.isEmpty())
                err = QStringLiteral("exit %1 (no output — often missing -manifestfile / AV lock)")
                          .arg(proc.exitCode());
            throw std::runtime_error(
                QStringLiteral("DepotDownloader failed for depot %1: %2")
                    .arg(depotId, err)
                    .toStdString());
        }

        emitProgress(onProgress,
                     qBound(1, 99, int(99.0 * double(i + 1) / double(depotCount))),
                     QStringLiteral("Downloading..."), QStringLiteral("downloading"),
                     completedWritten, totalHint);
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

/** Actual bytes written by DepotDownloader (SetLength prealloc does not inflate this). */
bool processIoBytes(qint64 pid, qint64* readOut, qint64* writeOut)
{
    if (pid <= 0)
        return false;
#if defined(Q_OS_WIN)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return false;
    IO_COUNTERS counters{};
    const BOOL ok = GetProcessIoCounters(h, &counters);
    CloseHandle(h);
    if (!ok)
        return false;
    if (readOut)
        *readOut = static_cast<qint64>(counters.ReadTransferCount);
    if (writeOut)
        *writeOut = static_cast<qint64>(counters.WriteTransferCount);
    return true;
#else
    QFile f(QStringLiteral("/proc/%1/io").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    qint64 r = -1, w = -1;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.startsWith("read_bytes:"))
            r = line.mid(11).trimmed().toLongLong();
        else if (line.startsWith("write_bytes:"))
            w = line.mid(12).trimmed().toLongLong();
    }
    if (r < 0 || w < 0)
        return false;
    if (readOut)
        *readOut = r;
    if (writeOut)
        *writeOut = w;
    return true;
#endif
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

        QString depotOs = settings.depotOs;
        if (depotOs.isEmpty())
            depotOs = QStringLiteral("windows");
#if !defined(Q_OS_WIN)
        if (depotOs == QStringLiteral("auto"))
            depotOs = QStringLiteral("windows");
#endif

        emitProgress(onProgress, 1,
                     force ? QStringLiteral("Updating…") : QStringLiteral("Downloading..."));
        runDdmod(pluginRoot, appid, parsed, installPath, depotOs, staging.path(), force, onProgress,
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
                            {QStringLiteral("mode"), QStringLiteral("ddmod")},
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
