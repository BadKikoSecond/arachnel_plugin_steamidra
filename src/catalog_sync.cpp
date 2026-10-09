#include "catalog_sync.h"

#include "http_client.h"
#include "install_kind.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

namespace steamidra {
namespace {

arachnel::core::CatalogItemKind itemKindFromString(const QString& value)
{
    const QString n = value.trimmed().toLower();
    if (n == QStringLiteral("dlc"))
        return arachnel::core::CatalogItemKind::Dlc;
    if (n == QStringLiteral("addon") || n == QStringLiteral("add-on"))
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
        const QString appid = obj.value(QStringLiteral("appid")).toString();
        if (!appid.isEmpty())
            component.id = QStringLiteral("steam-%1").arg(appid);
        else
            component.id = arachnel::core::slugifyCatalogId(title, QStringLiteral("steamidra-addon"));
    }
    component.fileSize = obj.value(QStringLiteral("sizeLabel")).toString().trimmed();
    if (component.fileSize.isEmpty())
        component.fileSize = obj.value(QStringLiteral("fileSize")).toString().trimmed();
    if (component.fileSize.isEmpty()) {
        const QJsonValue rawBytes = obj.value(QStringLiteral("sizeBytes"));
        qint64 bytes = 0;
        if (rawBytes.isDouble())
            bytes = static_cast<qint64>(rawBytes.toDouble());
        else if (rawBytes.isString())
            bytes = rawBytes.toString().toLongLong();
        if (bytes > 0) {
            static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
            double value = static_cast<double>(bytes);
            int unit = 0;
            while (value >= 1024.0 && unit < 4) {
                value /= 1024.0;
                ++unit;
            }
            component.fileSize = unit == 0
                                     ? QStringLiteral("%1 %2").arg(static_cast<qint64>(value)).arg(
                                           QString::fromUtf8(units[unit]))
                                     : QStringLiteral("%1 %2")
                                           .arg(value, 0, 'f', 1)
                                           .arg(QString::fromUtf8(units[unit]));
        }
    }
    component.uploadDate = obj.value(QStringLiteral("uploadDate")).toString();
    component.coverUrl = obj.value(QStringLiteral("coverUrl")).toString();
    component.contentAvailable = obj.value(QStringLiteral("contentAvailable")).toBool(true);
    component.kind = itemKindFromString(obj.value(QStringLiteral("kind")).toString());
    if (component.kind == arachnel::core::CatalogItemKind::Game
        && (obj.value(QStringLiteral("kind")).toString().isEmpty()
            || component.id.startsWith(QStringLiteral("steam-"))))
        component.kind = arachnel::core::CatalogItemKind::Dlc;
    const QString delivery = obj.value(QStringLiteral("delivery")).toString().trimmed().toLower();
    component.delivery = delivery == QStringLiteral("direct") || delivery.isEmpty()
                             ? arachnel::core::ComponentDelivery::Direct
                             : arachnel::core::ComponentDelivery::Magnet;
    component.referer = obj.value(QStringLiteral("referer")).toString();
    component.getfileUrl = obj.value(QStringLiteral("getfileUrl")).toString();
    component.optional = obj.value(QStringLiteral("optional")).toBool(true);
    for (const QJsonValue& shot : obj.value(QStringLiteral("screenshotUrls")).toArray()) {
        const QString url = shot.toString().trimmed();
        if (!url.isEmpty())
            component.screenshotUrls.append(url);
    }
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

struct RelayDlcPayload {
    QVector<arachnel::core::CatalogComponent> addons;
    bool hasWorkshop = false;
    bool ok = false;
};

RelayDlcPayload fetchRelayDlcs(const PluginSettings& settings, const QString& appid)
{
    RelayDlcPayload out;
    if (appid.isEmpty())
        return out;
    QString base = settings.relayBaseUrl.trimmed();
    if (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (base.isEmpty())
        base = QString::fromUtf8(kDefaultRelayBase);
    const QString url = base + QStringLiteral("/v1/app/%1/dlcs").arg(appid);
    QMap<QString, QString> headers;
    if (!settings.relayClientToken.isEmpty())
        headers.insert(QStringLiteral("X-Arachnel-Token"), settings.relayClientToken);
    QString err;
    const QByteArray raw = httpGet(url, 30000, headers, &err);
    if (raw.isEmpty())
        return out;
    const QJsonObject root = QJsonDocument::fromJson(raw).object();
    const QJsonArray dlcs = root.value(QStringLiteral("dlcs")).toArray();
    const QString parentId = QStringLiteral("steam-%1").arg(appid);
    out.addons.reserve(dlcs.size());
    for (const QJsonValue& v : dlcs) {
        if (!v.isObject())
            continue;
        out.addons.append(addonFromJson(v.toObject(), parentId));
    }
    out.hasWorkshop = root.value(QStringLiteral("hasWorkshop")).toBool(false);
    out.ok = true;
    return out;
}

QJsonObject normalizeCatalogGame(const QJsonObject& raw)
{
    QString appid = raw.value(QStringLiteral("steamAppId")).toString();
    if (appid.isEmpty())
        appid = QString::number(raw.value(QStringLiteral("steamAppId")).toInt());
    if (appid.isEmpty())
        appid = raw.value(QStringLiteral("appid")).toString();
    if (appid.isEmpty()) {
        const QString entryId = raw.value(QStringLiteral("id")).toString();
        if (entryId.startsWith(QStringLiteral("steam-")))
            appid = entryId.mid(6);
    }
    bool okDigit = false;
    appid.toLongLong(&okDigit);
    if (!okDigit)
        return {};
    if (raw.contains(QStringLiteral("hasManifest")) && !raw.value(QStringLiteral("hasManifest")).toBool(true))
        return {};
    {
        const QString type = raw.value(QStringLiteral("type")).toString().trimmed().toLower();
        if (type == QStringLiteral("dlc") || type == QStringLiteral("music")
            || type == QStringLiteral("demo") || type == QStringLiteral("mod")
            || type == QStringLiteral("video")) {
            return {};
        }
    }

    const QString upload = raw.value(QStringLiteral("uploadDate")).toString().trimmed();
    QString version = raw.value(QStringLiteral("version")).toString().trimmed();
    if (version.isEmpty() && !upload.isEmpty())
        version = upload.left(10);
    QString genres = raw.value(QStringLiteral("genres")).toString();
    bool needsFix = raw.value(QStringLiteral("needsOnlineFix")).toBool(false);
    if (!raw.contains(QStringLiteral("needsOnlineFix"))) {
        const QString g = genres.toLower();
        needsFix = g.contains(QStringLiteral("online fix")) || g.contains(QStringLiteral("online co-op"))
                   || g.contains(QStringLiteral("online pvp"));
    }
    int installKind = raw.value(QStringLiteral("installKind")).toInt(-1);
    if (installKind < 0)
        installKind = needsFix ? 2 : 0;

    QJsonObject entry;
    entry.insert(QStringLiteral("id"),
                 raw.value(QStringLiteral("id")).toString(QStringLiteral("steam-%1").arg(appid)));
    entry.insert(QStringLiteral("title"),
                 raw.value(QStringLiteral("title")).toString(
                     raw.value(QStringLiteral("name")).toString(QStringLiteral("App %1").arg(appid))));
    entry.insert(QStringLiteral("steamAppId"), appid);
    QString cover = raw.value(QStringLiteral("coverUrl")).toString();
    if (cover.isEmpty())
        cover = QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/header.jpg").arg(appid);
    entry.insert(QStringLiteral("coverUrl"), cover);
    entry.insert(QStringLiteral("description"), raw.value(QStringLiteral("description")).toString());
    entry.insert(QStringLiteral("sizeLabel"),
                 raw.value(QStringLiteral("sizeLabel")).toString(raw.value(QStringLiteral("fileSize")).toString()));
    entry.insert(QStringLiteral("uploadDate"), upload);
    entry.insert(QStringLiteral("version"), version);
    entry.insert(QStringLiteral("genres"), genres);
    entry.insert(QStringLiteral("needsOnlineFix"), needsFix);
    entry.insert(QStringLiteral("installKind"), installKind);
    entry.insert(QStringLiteral("sourceId"), QStringLiteral("steamidra"));
    // Keep relay DLC metadata so Arachnel can show "N add-ons" and enrich via /dlcs.
    if (raw.value(QStringLiteral("dlcCount")).isDouble())
        entry.insert(QStringLiteral("dlcCount"), raw.value(QStringLiteral("dlcCount")).toInt());
    else if (raw.value(QStringLiteral("dlc")).isArray())
        entry.insert(QStringLiteral("dlcCount"), raw.value(QStringLiteral("dlc")).toArray().size());
    if (raw.value(QStringLiteral("dlc")).isArray())
        entry.insert(QStringLiteral("dlc"), raw.value(QStringLiteral("dlc")).toArray());
    if (raw.value(QStringLiteral("hasWorkshop")).isBool())
        entry.insert(QStringLiteral("hasWorkshop"), raw.value(QStringLiteral("hasWorkshop")).toBool());
    // Online-Fix zip addons stay hidden until that flow is ready.
    return entry;
}

void collectReferencedDlcAppIds(const QJsonObject& raw, QSet<QString>* out)
{
    if (!out)
        return;
    const auto addId = [out](QString id) {
        id = id.trimmed();
        if (id.startsWith(QStringLiteral("steam-")))
            id = id.mid(6);
        if (!id.isEmpty())
            out->insert(id);
    };
    const QJsonValue dlcVal = raw.value(QStringLiteral("dlc"));
    if (dlcVal.isArray()) {
        for (const QJsonValue& d : dlcVal.toArray()) {
            if (d.isString())
                addId(d.toString());
            else if (d.isDouble())
                addId(QString::number(d.toInteger()));
        }
    } else if (dlcVal.isString()) {
        for (const QString& part : dlcVal.toString().split(QLatin1Char(',')))
            addId(part);
    }
}

QJsonArray parseCatalogPayload(const QJsonDocument& doc)
{
    QJsonArray rows;
    if (doc.isArray())
        rows = doc.array();
    else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        rows = root.value(QStringLiteral("entries")).toArray();
        if (rows.isEmpty())
            rows = root.value(QStringLiteral("games")).toArray();
        if (rows.isEmpty())
            rows = root.value(QStringLiteral("downloads")).toArray();
    }
    // Pass 1: DLC appids referenced by any row. Pass 2: normalize and keep only rows that are
    // not such a DLC. (This used to build the full normalized array first and then copy it
    // into a filtered one, holding two ~115 MB arrays at once.)
    QSet<QString> referencedDlc;
    for (const QJsonValue& v : rows) {
        if (v.isObject())
            collectReferencedDlcAppIds(v.toObject(), &referencedDlc);
    }
    QJsonArray games;
    for (const QJsonValue& v : rows) {
        if (!v.isObject())
            continue;
        const QJsonObject n = normalizeCatalogGame(v.toObject());
        if (n.isEmpty())
            continue;
        if (!referencedDlc.isEmpty()) {
            const QString appid = n.value(QStringLiteral("steamAppId")).toString();
            if (!appid.isEmpty() && referencedDlc.contains(appid))
                continue;
        }
        games.append(n);
    }
    return games;
}

// ---- streaming catalog normalization --------------------------------------------------------
// QJsonDocument keeps a ~4x larger tree than the JSON text (about 300 MB for the 72 MB feed).
// The feed is a flat array of small objects, so instead of building one tree the rows are
// located in the raw bytes and parsed one at a time.

// Index just past the JSON value that starts at p[i]; -1 on malformed input.
qsizetype skipJsonValue(const char* p, qsizetype n, qsizetype i)
{
    if (i >= n)
        return -1;
    const char c = p[i];
    if (c == '"') {
        for (++i; i < n;) {
            if (p[i] == '\\')
                i += 2;
            else if (p[i] == '"')
                return i + 1;
            else
                ++i;
        }
        return -1;
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        bool inString = false;
        for (; i < n; ++i) {
            const char ch = p[i];
            if (inString) {
                if (ch == '\\')
                    ++i;
                else if (ch == '"')
                    inString = false;
            } else if (ch == '"') {
                inString = true;
            } else if (ch == '{' || ch == '[') {
                ++depth;
            } else if (ch == '}' || ch == ']') {
                if (--depth == 0)
                    return i + 1;
            }
        }
        return -1;
    }
    while (i < n && p[i] != ',' && p[i] != ']' && p[i] != '}' && p[i] != ' ' && p[i] != '\n'
           && p[i] != '\r' && p[i] != '\t') {
        ++i;
    }
    return i;
}

qsizetype skipJsonSpace(const char* p, qsizetype n, qsizetype i)
{
    while (i < n && (p[i] == ' ' || p[i] == '\n' || p[i] == '\r' || p[i] == '\t'))
        ++i;
    return i;
}

struct RowsRange
{
    qsizetype begin = -1; // just after '['
    qsizetype end = -1;   // at the matching ']'
    bool valid() const { return begin >= 0 && end >= begin; }
};

// Locates the rows array the same way parseCatalogPayload does: a top-level array, or the first
// non-empty of "entries", "games", "downloads" in a top-level object.
bool locateRows(const QByteArray& payload, RowsRange* out)
{
    const char* p = payload.constData();
    const qsizetype n = payload.size();
    qsizetype i = skipJsonSpace(p, n, 0);
    if (i >= n)
        return false;
    const auto rangeOfArray = [&](qsizetype at) {
        RowsRange r;
        const qsizetype stop = skipJsonValue(p, n, at);
        if (stop > at) {
            r.begin = at + 1;
            r.end = stop - 1;
        }
        return r;
    };
    const auto nonEmpty = [&](const RowsRange& r) {
        return r.valid() && skipJsonSpace(p, n, r.begin) < r.end;
    };
    if (p[i] == '[') {
        *out = rangeOfArray(i);
        return out->valid();
    }
    if (p[i] != '{')
        return false;
    RowsRange entries, games, downloads;
    ++i;
    for (;;) {
        i = skipJsonSpace(p, n, i);
        if (i >= n)
            return false;
        if (p[i] == '}')
            break;
        if (p[i] == ',') {
            ++i;
            continue;
        }
        if (p[i] != '"')
            return false;
        const qsizetype keyEnd = skipJsonValue(p, n, i);
        if (keyEnd < 0)
            return false;
        const QByteArrayView key(p + i + 1, keyEnd - i - 2);
        i = skipJsonSpace(p, n, keyEnd);
        if (i >= n || p[i] != ':')
            return false;
        i = skipJsonSpace(p, n, i + 1);
        if (i >= n)
            return false;
        const qsizetype valueEnd = skipJsonValue(p, n, i);
        if (valueEnd < 0)
            return false;
        if (p[i] == '[') {
            if (key == "entries")
                entries = rangeOfArray(i);
            else if (key == "games")
                games = rangeOfArray(i);
            else if (key == "downloads")
                downloads = rangeOfArray(i);
        }
        i = valueEnd;
    }
    if (nonEmpty(entries))
        *out = entries;
    else if (nonEmpty(games))
        *out = games;
    else if (nonEmpty(downloads))
        *out = downloads;
    else
        return false; // nothing usable: let the caller take the whole-document path
    return true;
}

// Normalizes every row of `payload` without building a document tree. Each kept row is stored
// already serialized (compact JSON, the exact bytes the cache file wants) together with its
// steamAppId; `referenced` collects DLC appids that must be dropped afterwards.
bool normalizeRowsStreaming(const QByteArray& payload, QVector<QByteArray>* rows,
                            QVector<QString>* appIds, QSet<QString>* referenced)
{
    RowsRange range;
    if (!locateRows(payload, &range))
        return false;
    const char* p = payload.constData();
    const qsizetype n = payload.size();
    qsizetype i = range.begin;
    rows->reserve(130000);
    appIds->reserve(130000);
    for (;;) {
        i = skipJsonSpace(p, n, i);
        if (i >= range.end)
            break;
        if (p[i] == ',') {
            ++i;
            continue;
        }
        const qsizetype stop = skipJsonValue(p, n, i);
        if (stop < 0 || stop > range.end)
            return false;
        if (p[i] == '{') {
            const QJsonDocument one =
                QJsonDocument::fromJson(QByteArray::fromRawData(p + i, stop - i));
            if (one.isObject()) {
                const QJsonObject raw = one.object();
                collectReferencedDlcAppIds(raw, referenced);
                const QJsonObject norm = normalizeCatalogGame(raw);
                if (!norm.isEmpty()) {
                    appIds->append(norm.value(QStringLiteral("steamAppId")).toString());
                    rows->append(QJsonDocument(norm).toJson(QJsonDocument::Compact));
                }
            }
        }
        i = stop;
    }
    return true;
}

// The cache is written with sorted compact keys: {"catalogKind":"..","count":N,"entries":[...
// so its kind and size are in the first bytes. Reading them avoids parsing the whole file
// (~150 MB of QJson data for a 70 MB feed) on every sync call.
bool peekCacheHeader(const QString& path, QString* kind, int* count)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(512);
    static const QRegularExpression re(
        QStringLiteral(R"re("catalogKind":"([^"]*)","count":(\d+),"entries":\[)re"));
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(head));
    if (!m.hasMatch())
        return false;
    *kind = m.captured(1);
    *count = m.captured(2).toInt();
    return true;
}

QByteArray quoteAscii(const QString& s)
{
    return '"' + s.toLatin1() + '"';
}

// Streams the cache file entry by entry. Same bytes QJsonDocument(doc).toJson(Compact) used to
// produce, without a second 70 MB copy of the array and the serialized text.
bool writeCatalogCache(const QString& cachePath, const QString& kind, const QJsonArray& games)
{
    QSaveFile out(cachePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QByteArray chunk;
    chunk.reserve(1 << 20);
    chunk += "{\"catalogKind\":" + quoteAscii(kind) + ",\"count\":"
        + QByteArray::number(games.size()) + ",\"entries\":[";
    bool first = true;
    for (const QJsonValue& v : games) {
        if (!first)
            chunk += ',';
        first = false;
        chunk += QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
        if (chunk.size() >= (1 << 20)) {
            if (out.write(chunk) != chunk.size()) {
                out.cancelWriting();
                return false;
            }
            chunk.clear();
        }
    }
    chunk += "],\"schemaVersion\":1,\"updatedAt\":"
        + quoteAscii(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)) + "}";
    if (out.write(chunk) != chunk.size()) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

// Same file as writeCatalogCache, from rows that are already serialized; `skipped` entries
// (referenced DLC ids) are left out.
bool writeCatalogCacheRows(const QString& cachePath, const QString& kind,
                           const QVector<QByteArray>& rows, const QVector<QString>& appIds,
                           const QSet<QString>& referenced, int keptCount)
{
    QSaveFile out(cachePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QByteArray chunk;
    chunk.reserve(1 << 20);
    chunk += "{\"catalogKind\":" + quoteAscii(kind) + ",\"count\":" + QByteArray::number(keptCount)
        + ",\"entries\":[";
    bool first = true;
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (!referenced.isEmpty() && !appIds.at(i).isEmpty() && referenced.contains(appIds.at(i)))
            continue;
        if (!first)
            chunk += ',';
        first = false;
        chunk += rows.at(i);
        if (chunk.size() >= (1 << 20)) {
            if (out.write(chunk) != chunk.size()) {
                out.cancelWriting();
                return false;
            }
            chunk.clear();
        }
    }
    chunk += "],\"schemaVersion\":1,\"updatedAt\":"
        + quoteAscii(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)) + "}";
    if (out.write(chunk) != chunk.size()) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

} // namespace

arachnel::core::CatalogEntry catalogEntryFromJson(const QJsonObject& obj)
{
    arachnel::core::CatalogEntry e;
    e.id = obj.value(QStringLiteral("id")).toString();
    e.title = obj.value(QStringLiteral("title")).toString();
    e.sourceId = QStringLiteral("steamidra");
    e.steamAppId = obj.value(QStringLiteral("steamAppId")).toString();
    {
        const QString feedCover = obj.value(QStringLiteral("coverUrl")).toString().trimmed();
        if (feedCover.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
            || feedCover.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
            e.remoteCoverUrl = feedCover;
        } else if (feedCover.startsWith(QStringLiteral("file:"))) {
            e.coverUrl = feedCover;
        }
    }
    e.description = obj.value(QStringLiteral("description")).toString();
    e.genres = obj.value(QStringLiteral("genres")).toString();
    e.sizeLabel = obj.value(QStringLiteral("sizeLabel")).toString();
    if (e.sizeLabel.isEmpty())
        e.sizeLabel = obj.value(QStringLiteral("fileSize")).toString();
    e.uploadDate = obj.value(QStringLiteral("uploadDate")).toString();
    e.version = obj.value(QStringLiteral("version")).toString();
    if (e.version.isEmpty() && !e.uploadDate.isEmpty())
        e.version = e.uploadDate.left(10);
    const int rawKind = obj.value(QStringLiteral("installKind")).toInt(-1);
    if (rawKind >= static_cast<int>(arachnel::core::InstallKind::PortableArchive)
        && rawKind <= static_cast<int>(arachnel::core::InstallKind::FixDownload)) {
        e.installKind = static_cast<arachnel::core::InstallKind>(rawKind);
    } else if (obj.value(QStringLiteral("needsOnlineFix")).toBool()) {
        e.installKind = arachnel::core::InstallKind::BundledFix;
    } else {
        e.installKind = arachnel::core::InstallKind::PortableArchive;
    }
    e.itemKind = arachnel::core::CatalogItemKind::Game;
    if (e.id.isEmpty() && !e.steamAppId.isEmpty())
        e.id = QStringLiteral("steam-%1").arg(e.steamAppId);
    // Last-resort remote hint only - never invent display https into coverUrl.
    if (e.remoteCoverUrl.isEmpty() && !e.steamAppId.isEmpty()) {
        e.remoteCoverUrl =
            QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/header.jpg")
                .arg(e.steamAppId);
    }
    // Keep list rows light: dlcCount for badge; full rows load via /dlcs in entryById.
    e.dlcCount = obj.value(QStringLiteral("dlcCount")).toInt(0);
    if (e.dlcCount <= 0 && obj.value(QStringLiteral("dlc")).isArray())
        e.dlcCount = obj.value(QStringLiteral("dlc")).toArray().size();
    e.hasWorkshop = obj.value(QStringLiteral("hasWorkshop")).toBool(false);
    e.addons.clear();
    return e;
}

void enrichCatalogEntryWithRelayDlcs(arachnel::core::CatalogEntry& entry, const PluginSettings& settings)
{
    QString appid = entry.steamAppId.trimmed();
    if (appid.isEmpty() && entry.id.startsWith(QStringLiteral("steam-")))
        appid = entry.id.mid(6);
    if (appid.isEmpty())
        return;
    const RelayDlcPayload payload = fetchRelayDlcs(settings, appid);
    if (!payload.ok)
        return;
    entry.addons = payload.addons;
    entry.hasWorkshop = payload.hasWorkshop;
    if (!payload.addons.isEmpty())
        entry.dlcCount = payload.addons.size();
}

QVector<arachnel::core::CatalogEntry> loadCatalogEntriesFromFile(const QString& path)
{
    QVector<arachnel::core::CatalogEntry> out;
    if (path.isEmpty())
        return out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QJsonArray games = parseCatalogPayload(QJsonDocument::fromJson(f.readAll()));
    out.reserve(games.size());
    for (const QJsonValue& v : games) {
        if (v.isObject())
            out.append(catalogEntryFromJson(v.toObject()));
    }
    return out;
}

QString syncManifestCatalog(const PluginSettings& settings, const QString& cachePath, bool force)
{
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    const QString url = settings.manifestCatalogUrl.trimmed();
    if (url.isEmpty())
        return QFileInfo::exists(cachePath) ? cachePath : QString();

    if (QFileInfo::exists(cachePath)) {
        QString kind;
        int count = 0;
        bool known = peekCacheHeader(cachePath, &kind, &count);
        if (!known) {
            // Older layout: fall back to reading the whole file.
            QFile peek(cachePath);
            if (peek.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(peek.readAll()).object();
                kind = root.value(QStringLiteral("catalogKind")).toString();
                count = root.value(QStringLiteral("entries")).toArray().size();
            }
        }
        if (kind == QStringLiteral("installable-manifest") && count < 10000) {
            QFile::remove(cachePath);
            force = true;
        }
    }

    const qint64 ttl = catalogTtlSeconds(settings);
    if (!force && QFileInfo::exists(cachePath) && ttl > 0) {
        const qint64 age = QFileInfo(cachePath).lastModified().secsTo(QDateTime::currentDateTime());
        if (age >= 0 && age < ttl)
            return cachePath;
    }

    const QString kind = url.contains(QStringLiteral("ryuu")) ? QStringLiteral("ryuu-relay")
                                                              : QStringLiteral("installable-manifest");
    QByteArray payload;
    {
        QString err;
        payload = httpGet(url, 90000, {}, &err);
    }
    if (payload.isEmpty())
        return QFileInfo::exists(cachePath) ? cachePath : QString();

    // Preferred path: row-by-row, no document tree (peak ~ payload + serialized rows).
    {
        QVector<QByteArray> rows;
        QVector<QString> appIds;
        QSet<QString> referenced;
        if (normalizeRowsStreaming(payload, &rows, &appIds, &referenced)) {
            payload = QByteArray();
            int kept = 0;
            for (qsizetype i = 0; i < rows.size(); ++i) {
                if (referenced.isEmpty() || appIds.at(i).isEmpty()
                    || !referenced.contains(appIds.at(i)))
                    ++kept;
            }
            if (kept < 10)
                return QFileInfo::exists(cachePath) ? cachePath : QString();
            if (!writeCatalogCacheRows(cachePath, kind, rows, appIds, referenced, kept))
                return {};
            return cachePath;
        }
    }

    // Fallback (unexpected layout): the whole-document path.
    QJsonArray games;
    {
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        payload = QByteArray();
        games = parseCatalogPayload(doc);
    }
    if (games.size() < 10)
        return QFileInfo::exists(cachePath) ? cachePath : QString();
    if (!writeCatalogCache(cachePath, kind, games))
        return {};
    return cachePath;
}

QByteArray catalogSourceDescriptorJson(const PluginSettings& settings)
{
    const QString url = settings.manifestCatalogUrl.trimmed();
    if (url.isEmpty())
        return {};
    QJsonObject o;
    o.insert(QStringLiteral("url"), url);
    o.insert(QStringLiteral("rowsKeys"),
             QJsonArray{QStringLiteral("entries"), QStringLiteral("games"),
                        QStringLiteral("downloads")});
    o.insert(QStringLiteral("ttlSeconds"), static_cast<double>(catalogTtlSeconds(settings)));
    o.insert(QStringLiteral("minRows"), 10);
    o.insert(QStringLiteral("parallel"), true);
    o.insert(QStringLiteral("userAgent"), QStringLiteral("Arachnel-SteaMidra/0.4"));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray normalizeRowsJson(const QByteArray& rowsJsonArray)
{
    const QJsonDocument doc = QJsonDocument::fromJson(rowsJsonArray);
    if (!doc.isArray())
        return {};
    QJsonArray entries;
    QSet<QString> referenced;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject())
            continue;
        const QJsonObject raw = v.toObject();
        collectReferencedDlcAppIds(raw, &referenced);
        const QJsonObject norm = normalizeCatalogGame(raw);
        if (!norm.isEmpty())
            entries.append(norm);
    }
    QJsonArray supersedes;
    for (const QString& appid : referenced)
        supersedes.append(QStringLiteral("steam-%1").arg(appid));
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("arachnel.plugin.catalog.v1"));
    root.insert(QStringLiteral("entries"), entries);
    root.insert(QStringLiteral("supersedes"), supersedes);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QJsonObject steamAppDetailsJson(const QString& appid)
{
    const QString url =
        QStringLiteral("https://store.steampowered.com/api/appdetails?appids=%1&l=english").arg(appid);
    QString err;
    const QByteArray raw = httpGet(url, 30000, {}, &err);
    if (raw.isEmpty())
        return {};
    const QJsonObject root = QJsonDocument::fromJson(raw).object();
    const QJsonObject block = root.value(appid).toObject();
    if (!block.value(QStringLiteral("success")).toBool())
        return {};
    const QJsonObject data = block.value(QStringLiteral("data")).toObject();
    if (data.isEmpty())
        return {};
    QJsonObject entry;
    entry.insert(QStringLiteral("id"), QStringLiteral("steam-%1").arg(appid));
    entry.insert(QStringLiteral("title"), data.value(QStringLiteral("name")).toString());
    entry.insert(QStringLiteral("steamAppId"), appid);
    entry.insert(QStringLiteral("description"), data.value(QStringLiteral("short_description")).toString());
    entry.insert(QStringLiteral("coverUrl"),
                 QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/header.jpg").arg(appid));
    QStringList genres;
    for (const QJsonValue& g : data.value(QStringLiteral("genres")).toArray()) {
        if (g.isObject())
            genres.append(g.toObject().value(QStringLiteral("description")).toString());
    }
    for (const QJsonValue& c : data.value(QStringLiteral("categories")).toArray()) {
        if (c.isObject())
            genres.append(c.toObject().value(QStringLiteral("description")).toString());
    }
    genres.removeDuplicates();
    entry.insert(QStringLiteral("genres"), genres.join(QStringLiteral(", ")));
    return entry;
}

} // namespace steamidra
