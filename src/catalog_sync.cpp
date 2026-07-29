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
    if (component.id.isEmpty())
        component.id = arachnel::core::slugifyCatalogId(title, QStringLiteral("steamidra-addon"));
    component.fileSize = obj.value(QStringLiteral("fileSize")).toString();
    component.uploadDate = obj.value(QStringLiteral("uploadDate")).toString();
    component.kind = itemKindFromString(obj.value(QStringLiteral("kind")).toString());
    const QString delivery = obj.value(QStringLiteral("delivery")).toString().trimmed().toLower();
    component.delivery = delivery == QStringLiteral("direct")
                             ? arachnel::core::ComponentDelivery::Direct
                             : arachnel::core::ComponentDelivery::Magnet;
    component.referer = obj.value(QStringLiteral("referer")).toString();
    component.getfileUrl = obj.value(QStringLiteral("getfileUrl")).toString();
    component.optional = obj.value(QStringLiteral("optional")).toBool(true);
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
    // TEMP: hide catalog add-ons (Online-Fix repair zips, etc.) until the flow is ready.
    // if (raw.value(QStringLiteral("addons")).isArray())
    //     entry.insert(QStringLiteral("addons"), raw.value(QStringLiteral("addons")));
    return entry;
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
    for (const QJsonValue& v : rows) {
        if (!v.isObject())
            continue;
        const QJsonObject n = normalizeCatalogGame(v.toObject());
        if (!n.isEmpty())
            games.append(n);
    }
    return games;
}

} // namespace

arachnel::core::CatalogEntry catalogEntryFromJson(const QJsonObject& obj)
{
    arachnel::core::CatalogEntry e;
    e.id = obj.value(QStringLiteral("id")).toString();
    e.title = obj.value(QStringLiteral("title")).toString();
    e.coverUrl = obj.value(QStringLiteral("coverUrl")).toString();
    e.sourceId = QStringLiteral("steamidra");
    e.steamAppId = obj.value(QStringLiteral("steamAppId")).toString();
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
    if (e.coverUrl.isEmpty() && !e.steamAppId.isEmpty()) {
        e.coverUrl = QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/header.jpg")
                         .arg(e.steamAppId);
    }
    // TEMP: hide catalog add-ons (Online-Fix repair zips, etc.) in the download sheet.
    e.addons.clear();
    return e;
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
    const QByteArray payload = httpGet(url, 300000, {}, &err);
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
