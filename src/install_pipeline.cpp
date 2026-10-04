#include "install_pipeline.h"

#include "http_client.h"
#include "online_fix_embed.h"
#include "steam_launch.h"
#include "zip_local.h"

#include <depotkit.h>

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <climits>
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
    /** Steam's per-depot `config.oslist` (lowercase, e.g. "windows", "linux"), from
     *  steamcmd. Absent or empty = shared depot. Filled by filterPrimaryDepots(). */
    QHash<QString, QString> depotOsList;
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

/** Extract .lua / .manifest from a ZIP in memory (no zip on disk, Defender MalUri).
 *  Handles store (method 0); deflated entries are skipped (Ryuu packs store). */
bool extractRelayZipMemory(const QByteArray& raw, const QString& staging, QString* luaOut)
{
    if (raw.size() < 30 || raw.isEmpty())
        return false;
    QDir().mkpath(staging);
    bool wrote = false;
    size_t pos = 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(raw.constData());
    const size_t n = static_cast<size_t>(raw.size());
    while (pos < n) {
        ZipLocalView entry;
        if (!readZipLocal(bytes, n, pos, &entry))
            break;
        if (entry.nextPos <= pos)
            break;
        const int nameLen = entry.nameLen > static_cast<size_t>(INT_MAX)
                                ? 0
                                : static_cast<int>(entry.nameLen);
        const QString name = nameLen > 0
                                 ? QString::fromUtf8(raw.constData() + static_cast<qsizetype>(entry.nameOff),
                                                     nameLen)
                                 : QString();
        const QString base = QFileInfo(name).fileName().toLower();
        pos = entry.nextPos;
        if (entry.method != 0 || entry.compSize != entry.uncompSize)
            continue; // only stored entries
        if (!base.endsWith(QStringLiteral(".lua")) && !base.endsWith(QStringLiteral(".manifest")))
            continue;
        if (entry.compSize > static_cast<uint32_t>(INT_MAX))
            continue;
        const QByteArray data = raw.mid(static_cast<qsizetype>(entry.dataOff),
                                        static_cast<qsizetype>(entry.compSize));
        if (base.endsWith(QStringLiteral(".lua")) && luaOut && luaOut->isEmpty())
            *luaOut = QString::fromUtf8(data);
        const QString outName = QFileInfo(name).fileName();
        if (outName.isEmpty() || outName == QLatin1String(".") || outName == QLatin1String(".."))
            continue;
        if (base.endsWith(QStringLiteral(".manifest")) || base.endsWith(QStringLiteral(".lua"))) {
            QFile out(staging + QLatin1Char('/') + outName);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.write(data);
                wrote = true;
            }
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
        errors.append(QStringLiteral(
            "no depot manifests yet (provider listed the game but CDN package is empty)"));
    }

    throw std::runtime_error(
        QStringLiteral("Cannot install app %1 yet: %2")
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

void filterPrimaryDepots(const QString& appid, LuaParsed& parsed, bool includeAllDlc,
                         const QSet<QString>& selectedDlcAppIds)
{
    QMap<QString, QString> depots = parsed.depots;
    // App unlock key is not a content depot - downloading it needs no local manifest and fails.
    depots.remove(appid);

    QHash<QString, QString> depotToDlc;
    QString err;
    const QByteArray raw = httpGetJsonBytes(
        QStringLiteral("https://api.steamcmd.net/v1/info/%1").arg(appid), 45000, {}, &err);
    if (!raw.isEmpty()) {
        const QJsonObject root = QJsonDocument::fromJson(raw).object();
        const QJsonObject app =
            root.value(QStringLiteral("data")).toObject().value(appid).toObject();
        const QJsonObject depotsObj = app.value(QStringLiteral("depots")).toObject();
        for (auto it = depotsObj.begin(); it != depotsObj.end(); ++it) {
            if (it.key().isEmpty() || !it.key().at(0).isDigit() || !it.value().isObject())
                continue;
            const QString oslist = it.value()
                                       .toObject()
                                       .value(QStringLiteral("config"))
                                       .toObject()
                                       .value(QStringLiteral("oslist"))
                                       .toString()
                                       .trimmed()
                                       .toLower();
            if (!oslist.isEmpty())
                parsed.depotOsList.insert(it.key(), oslist);
            const QJsonValue dlcVal = it.value().toObject().value(QStringLiteral("dlcappid"));
            const QString dlcId = dlcVal.toVariant().toString().trimmed();
            if (dlcId.isEmpty() || dlcId == QStringLiteral("0"))
                continue;
            depotToDlc.insert(it.key(), dlcId);
        }
    }

    auto keepDepot = [&](const QString& depotId) -> bool {
        const QString dlcId = depotToDlc.value(depotId);
        if (!dlcId.isEmpty())
            return includeAllDlc || selectedDlcAppIds.contains(dlcId);
        if (includeAllDlc || selectedDlcAppIds.isEmpty()) {
            // No steamcmd map: drop far depots when selection is empty (Isaac 3353471).
            if (!includeAllDlc && selectedDlcAppIds.isEmpty()) {
                bool okApp = false;
                bool okDepot = false;
                const qint64 app = appid.toLongLong(&okApp);
                const qint64 depot = depotId.toLongLong(&okDepot);
                if (okApp && okDepot && !(depot >= app && depot < app + 100))
                    return false;
            }
            return true;
        }
        // Selection set: keep unmapped base-band depots only.
        bool okApp = false;
        bool okDepot = false;
        const qint64 app = appid.toLongLong(&okApp);
        const qint64 depot = depotId.toLongLong(&okDepot);
        if (okApp && okDepot && depot >= app && depot < app + 100)
            return true;
        return selectedDlcAppIds.contains(depotId);
    };

    if (!includeAllDlc) {
        QMap<QString, QString> keep;
        for (auto it = depots.begin(); it != depots.end(); ++it) {
            if (keepDepot(it.key()))
                keep.insert(it.key(), it.value());
        }
        depots = keep;
        QMap<QString, QString> manifests;
        for (auto it = parsed.manifests.begin(); it != parsed.manifests.end(); ++it) {
            if (it.key() == appid)
                continue;
            if (keepDepot(it.key()))
                manifests.insert(it.key(), it.value());
        }
        parsed.manifests = manifests;
    }

    if (depots.isEmpty() && !parsed.manifests.isEmpty()) {
        for (auto it = parsed.manifests.begin(); it != parsed.manifests.end(); ++it) {
            if (it.key() != appid)
                depots.insert(it.key(), QString());
        }
    }
    parsed.depots = depots;
}

QSet<QString> selectedDlcAppIds(const QStringList& selectedAddonIds)
{
    QSet<QString> out;
    for (QString id : selectedAddonIds) {
        id = id.trimmed();
        if (id.startsWith(QStringLiteral("steam-")))
            id = id.mid(6);
        bool ok = false;
        id.toLongLong(&ok);
        if (ok && !id.isEmpty())
            out.insert(id);
    }
    return out;
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
    QString lastDetail;
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

    QString status = QStringLiteral("downloading");
    if (p->phase == DEPOTKIT_PHASE_PAUSED)
        status = QStringLiteral("paused");
    else if (p->phase == DEPOTKIT_PHASE_CONNECTING || p->phase == DEPOTKIT_PHASE_PLANNING)
        status = QStringLiteral("starting");

    const QString detail = p->detail ? QString::fromUtf8(p->detail)
                                     : (status == QStringLiteral("paused") ? QStringLiteral("Paused")
                                                                           : QStringLiteral("Downloading..."));
    if (!detail.isEmpty())
        bridge->lastDetail = detail;
    const int bps = (p->phase == DEPOTKIT_PHASE_PAUSED) ? 0 : static_cast<int>(p->rate_bps);
    emitProgress(bridge->onProgress, pct, detail, status, static_cast<qint64>(p->bytes_done),
                 static_cast<qint64>(p->bytes_total), bps);
}

void runDepotkit(const QString& appid, const LuaParsed& parsed, const QString& dest,
                 const QString& staging, const QString& depotOs, bool forceUpdate,
                 const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress,
                 const std::function<bool()>& shouldCancel, depotkit_control* externalCtrl)
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
        if (mfUtf8.isEmpty() || !mfUtf8.constData() || keyUtf8.size() != 64 || !keyUtf8.constData())
            continue;
        uint32_t flags = 0;
        if (depotkit_manifest_platform_hint(mfUtf8.constData(), keyUtf8.constData(), &flags)
            == DEPOTKIT_OK)
            c.plat = flags;
        cands.push_back(std::move(c));
    }

    if (cands.empty())
        throw std::runtime_error(
            "No downloadable depots with local manifests (anonymous CDN needs them)");

    // Steam says which OS each depot is for; trust that over the manifest hint.
    // The hint is guessed from file contents and comes back 0 ("unknown") for a
    // plain Linux depot, which the check below then lets through. Core Keeper
    // (1621690) got its Linux depot 1621692 installed over Windows depot 1621691
    // that way: the Linux managed libraries overwrote the Windows ones, Mono took
    // the Unix code path under Proton, System.Random could not initialise, and the
    // game sat on its loading screen forever - on Windows hosts as well as Linux.
    // Depots with no oslist are shared content and are kept, as the worker does.
    const QString wantOsName = wantFlags == 2u   ? QStringLiteral("linux")
                               : wantFlags == 4u ? QStringLiteral("macos")
                                                 : QStringLiteral("windows");
    auto steamSaysOtherOs = [&](const QString& depotId) {
        const QString oslist = parsed.depotOsList.value(depotId);
        if (oslist.isEmpty())
            return false;
        const QStringList oses = oslist.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& os : oses) {
            if (os.trimmed() == wantOsName)
                return false;
        }
        return true;
    };

    std::vector<Cand> matched;
    for (const Cand& c : cands) {
        if (steamSaysOtherOs(c.depotId)) {
            qInfo().noquote() << "[steamidra] skip depot" << c.depotId << "oslist"
                              << parsed.depotOsList.value(c.depotId) << "wantOs" << wantOsName;
            continue;
        }
        if (c.plat == 0 || (c.plat & wantFlags))
            matched.push_back(c);
    }
    // If nothing matched the OS hint (weird manifests), fall back to all with files.
    if (matched.empty())
        matched = cands;
    // Do not prefer "exclusive" OS matches. That dropped Isaac DLC depots whose
    // platform hint is multi-bit or unknown while base 250902 is exclusive windows.

    QStringList matchedIds;
    matchedIds.reserve(static_cast<int>(matched.size()));
    for (const Cand& c : matched)
        matchedIds.append(c.depotId);
    qInfo().noquote() << "[steamidra] depotkit matched" << matchedIds.join(QLatin1Char(','))
                      << "wantOs" << depotOs << "cands" << int(cands.size());

    // Take C string pointers only after every QByteArray sits in its final
    // element. constData() dies if the vector grows. A selected DLC keeps
    // every lua depot, so this list is much larger than a base-game install.
    // depotkit writes through manifest_path, key_hex, and install_dir.
    struct DepotBytes {
        uint32_t depotId = 0;
        QByteArray path;
        QByteArray key;
    };
    std::vector<DepotBytes> held;
    held.reserve(matched.size());
    for (const Cand& c : matched) {
        if (c.manifestPath.isEmpty() || c.key.size() != 64 || c.depotId.isEmpty())
            continue;
        bool idOk = false;
        const qulonglong depotNum = c.depotId.toULongLong(&idOk);
        if (!idOk || depotNum == 0 || depotNum > 0xffffffffu)
            continue;
        DepotBytes row;
        row.depotId = static_cast<uint32_t>(depotNum);
        row.path = QDir::toNativeSeparators(c.manifestPath).toUtf8();
        row.key = c.key.toUtf8();
        if (row.path.isEmpty() || row.key.size() != 64)
            continue;
        held.push_back(std::move(row));
    }
    if (held.empty())
        throw std::runtime_error(
            "No downloadable depots with local manifests (anonymous CDN needs them)");

    std::vector<depotkit_depot> depots(held.size());
    for (size_t i = 0; i < held.size(); ++i) {
        depotkit_depot d{};
        d.depot_id = held[i].depotId;
        d.manifest_path = held[i].path.constData();
        d.key_hex = held[i].key.constData();
        if (!d.manifest_path || !d.key_hex || !d.manifest_path[0] || !d.key_hex[0])
            throw std::runtime_error("depotkit handoff missing path or key");
        depots[i] = d;
    }

    const QString nativeDest = QDir::toNativeSeparators(dest.trimmed());
    if (nativeDest.isEmpty())
        throw std::runtime_error("Install path is empty");
    const QByteArray installDir = nativeDest.toUtf8();
    if (installDir.isEmpty() || !installDir.constData())
        throw std::runtime_error("Install path is empty");
    depotkit_request req{};
    req.app_id = appid.toUInt();
    req.depots = depots.data();
    req.depot_count = depots.size();
    req.install_dir = installDir.constData();
    // 48 parallel HTTP/2 workers flake hard on some CDNs; keep it spicy but saner.
    req.max_downloads = 24;
    // Always SHA1-check existing files. Size-only skip treats SetEndOfFile stubs as done.
    req.validate = 1;
    req.cell_id = 0;
    (void)forceUpdate;

    const bool ownsCtrl = externalCtrl == nullptr;
    depotkit_control* ctrl = externalCtrl ? externalCtrl : depotkit_control_create();
    if (!ctrl)
        throw std::runtime_error("depotkit_control_create failed");

    DepotkitProgressBridge bridge;
    bridge.onProgress = onProgress;
    bridge.shouldCancel = shouldCancel;
    bridge.ctrl = ctrl;

    if (shouldCancel && shouldCancel()) {
        if (ownsCtrl)
            depotkit_control_destroy(ctrl);
        throw std::runtime_error("Cancelled");
    }

    emitProgress(onProgress, 1,
                 QStringLiteral("Steam CDN · %1 depot(s)…").arg(depots.size()));
    const depotkit_result rc = depotkit_download(&req, depotkitProgressThunk, &bridge, ctrl);
    if (ownsCtrl)
        depotkit_control_destroy(ctrl);

    if (rc == DEPOTKIT_ERR_CANCELLED || (shouldCancel && shouldCancel()))
        throw std::runtime_error("Cancelled");
    if (rc != DEPOTKIT_OK) {
        const QString why = bridge.lastDetail.isEmpty()
                                ? QString::fromUtf8(depotkit_strerror(rc))
                                : bridge.lastDetail;
        qWarning().noquote() << "[steamidra] depotkit failed" << why;
        throw std::runtime_error(
            QStringLiteral("depotkit failed: %1").arg(why).toStdString());
    }
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
    const PluginSettings& settings, bool force, const QStringList& selectedAddonIds,
    const std::function<void(const arachnel::core::OwnedDownloadProgress&)>& onProgress,
    const std::function<bool()>& shouldCancel, depotkit_control* downloadCtrl)
{
    InstallPipelineResult result;
    try {
        const QString app = appid.trimmed();
        if (app.isEmpty())
            throw std::runtime_error("Missing Steam App ID");
        QStringList addons;
        addons.reserve(selectedAddonIds.size());
        for (const QString& rawId : selectedAddonIds) {
            const QString id = rawId.trimmed();
            if (!id.isEmpty())
                addons.append(id);
        }

        const QString steam = findSteamPath(settings);
        const QString installdir = sanitizeInstallDir(title, app);
        QString installPath = targetPath.trimmed();
        installPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (installPath.isEmpty()) {
            if (!settings.libraryFolder.isEmpty())
                installPath = settings.libraryFolder + QLatin1Char('/') + installdir;
            else {
                const QString base =
                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                    + QStringLiteral("/ArachnelGames");
                installPath = base + QStringLiteral("/%1-%2").arg(app, installdir);
            }
        }

        ensureInjectBestEffort(steam, pluginRoot);

        QTemporaryDir staging;
        if (!staging.isValid())
            throw std::runtime_error("Cannot create staging directory");

        emitProgress(onProgress, 0, QStringLiteral("Getting game info…"));
        const QString lua = fetchLua(app, settings, staging.path(), onProgress);
        LuaParsed parsed = parseLua(lua);
        // Local *.manifest from Ryuu are required: anonymous Steam returns 401 without
        // -manifestfile. Staging filenames override any lua setManifestid pins.
        adoptStagingManifests(staging.path(), parsed);
        const QSet<QString> selectedDlc = selectedDlcAppIds(addons);
        // Any selected DLC → keep all lua depots. steamcmd dlcappid maps are incomplete
        // (Isaac kept only 250902-250911 and skipped Afterbirth/etc.). Valheim's public
        // steamcmd payload has no dlcappid rows, so steam-1620250 cannot be mapped and
        // this branch is what actually starts the DLC download.
        const bool downloadAllDlc = settings.includeDlc || !selectedDlc.isEmpty();
        filterPrimaryDepots(app, parsed, downloadAllDlc, selectedDlc);
        if (!steam.isEmpty()) {
            QDir().mkpath(steam + QStringLiteral("/config/stplug-in"));
            QFile luaOut(steam + QStringLiteral("/config/stplug-in/%1.lua").arg(app));
            if (luaOut.open(QIODevice::WriteOnly | QIODevice::Truncate))
                luaOut.write(lua.toUtf8());
        }

        qInfo().noquote() << "[steamidra] depots" << parsed.depots.keys().join(QLatin1Char(','))
                          << "selectedDlc" << selectedDlc.values().join(QLatin1Char(','))
                          << "downloadAllDlc" << downloadAllDlc;

        emitProgress(onProgress, 1,
                     force ? QStringLiteral("Updating…") : QStringLiteral("Downloading..."));
        QString depotOs = settings.depotOs.trimmed();
        if (depotOs.isEmpty() || depotOs.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0)
            depotOs = QStringLiteral("windows");
        runDepotkit(app, parsed, installPath, staging.path(), depotOs, force, onProgress,
                    shouldCancel, downloadCtrl);

        QJsonObject settingsJson;
        settingsJson.insert(QStringLiteral("autoEmbedOnlineFix"), settings.autoEmbedOnlineFix);
        settingsJson.insert(QStringLiteral("onlineFixSourceDir"), settings.onlineFixSourceDir);
        settingsJson.insert(QStringLiteral("hasWorkshop"), true);
        settingsJson.insert(QStringLiteral("unlockAllDlc"), downloadAllDlc);
        const OnlineFixResult fix =
            embedOnlineFix(installPath, app, pluginRoot, settingsJson, false);
        if (!fix.ok && !fix.error.isEmpty()) {
            emitProgress(onProgress, 95,
                         QStringLiteral("Online-Fix: %1").arg(fix.error),
                         QStringLiteral("warning"));
        }

        const ResolvedLaunchExe resolved = resolveInstallLaunch(installPath, app);
        const QVector<SteamLaunchOption> allOptions = collectLaunchOptions(installPath, app);
        QJsonObject launch;
        if (!resolved.path.isEmpty()) {
            launch.insert(QStringLiteral("type"), QStringLiteral("exe"));
            launch.insert(QStringLiteral("path"), resolved.path);
            if (!resolved.workingDirectory.isEmpty())
                launch.insert(QStringLiteral("workingDirectory"), resolved.workingDirectory);
            if (!resolved.arguments.isEmpty())
                launch.insert(QStringLiteral("arguments"),
                              QJsonArray::fromStringList(resolved.arguments));
            launch.insert(QStringLiteral("source"), resolved.source);
        } else {
            launch.insert(QStringLiteral("type"), QStringLiteral("steam"));
            launch.insert(QStringLiteral("uri"), QStringLiteral("steam://rungameid/%1").arg(app));
        }
        QFile marker(installPath + QStringLiteral("/.arachnel-steamidra"));
        QJsonObject payload{{QStringLiteral("appid"), app},
                            {QStringLiteral("mode"), QStringLiteral("depotkit")},
                            {QStringLiteral("launch"), launch}};
        if (!allOptions.isEmpty()) {
            QJsonArray optionsArr;
            for (const auto& opt : allOptions) {
                QJsonObject o;
                o.insert(QStringLiteral("id"), opt.id);
                o.insert(QStringLiteral("title"), opt.title);
                o.insert(QStringLiteral("path"), opt.path);
                o.insert(QStringLiteral("workingDirectory"), opt.workingDirectory);
                o.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(opt.arguments));
                o.insert(QStringLiteral("type"), opt.type);
                o.insert(QStringLiteral("isDefault"), opt.isDefault);
                optionsArr.append(o);
            }
            payload.insert(QStringLiteral("launchOptions"), optionsArr);
        }
        QJsonArray selectedDlcArr;
        for (const QString& id : selectedDlc)
            selectedDlcArr.append(id);
        payload.insert(QStringLiteral("selectedDlc"), selectedDlcArr);
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

        qInfo().noquote() << "[steamidra] installed" << app << "force" << force
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
