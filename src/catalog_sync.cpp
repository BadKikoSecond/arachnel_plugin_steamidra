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
    QJsonArray games;
    QSet<QString> referencedDlc;
    for (const QJsonValue& v : rows) {
        if (!v.isObject())
            continue;
        const QJsonObject raw = v.toObject();
        collectReferencedDlcAppIds(raw, &referencedDlc);
        const QJsonObject n = normalizeCatalogGame(raw);
        if (!n.isEmpty())
            games.append(n);
    }
    if (referencedDlc.isEmpty())
        return games;

    QJsonArray filtered;
    for (const QJsonValue& v : games) {
        const QJsonObject o = v.toObject();
        const QString appid = o.value(QStringLiteral("steamAppId")).toString();
        if (!appid.isEmpty() && referencedDlc.contains(appid))
            continue;
        filtered.append(o);
    }
    return filtered;
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
        QFile peek(cachePath);
        if (peek.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(peek.readAll()).object();
            const QString kind = root.value(QStringLiteral("catalogKind")).toString();
            const int count = root.value(QStringLiteral("entries")).toArray().size();
            if (kind == QStringLiteral("installable-manifest") && count < 10000) {
                QFile::remove(cachePath);
                force = true;
            }
        }
    }

    const qint64 ttl = catalogTtlSeconds(settings);
    if (!force && QFileInfo::exists(cachePath) && ttl > 0) {
        const qint64 age = QFileInfo(cachePath).lastModified().secsTo(QDateTime::currentDateTime());
        if (age >= 0 && age < ttl)
            return cachePath;
    }

    QString err;
    const QByteArray payload = httpGet(url, 90000, {}, &err);
    if (payload.isEmpty())
        return QFileInfo::exists(cachePath) ? cachePath : QString();

    const QJsonArray games = parseCatalogPayload(QJsonDocument::fromJson(payload));
    if (games.size() < 10)
        return QFileInfo::exists(cachePath) ? cachePath : QString();

    QJsonObject doc;
    doc.insert(QStringLiteral("schemaVersion"), 1);
    doc.insert(QStringLiteral("catalogKind"),
               url.contains(QStringLiteral("ryuu")) ? QStringLiteral("ryuu-relay")
                                                    : QStringLiteral("installable-manifest"));
    doc.insert(QStringLiteral("updatedAt"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    doc.insert(QStringLiteral("count"), games.size());
    doc.insert(QStringLiteral("entries"), games);
    QFile out(cachePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    out.write(QJsonDocument(doc).toJson(QJsonDocument::Compact));
    return cachePath;
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
