#include "plugin_settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace steamidra {

PluginSettings loadPluginSettings(const QString& settingsFilePath)
{
    PluginSettings s;
    s.settingsPath = settingsFilePath;
    if (settingsFilePath.isEmpty())
        return s;

    QFile f(settingsFilePath);
    if (!f.exists()) {
        QDir().mkpath(QFileInfo(settingsFilePath).absolutePath());
        const QJsonObject pub{
            {QStringLiteral("relayBaseUrl"), s.relayBaseUrl},
            {QStringLiteral("manifestCatalogUrl"), s.manifestCatalogUrl},
            {QStringLiteral("manifestCatalogTtlMinutes"), s.manifestCatalogTtlMinutes},
            {QStringLiteral("depotOs"), s.depotOs},
            {QStringLiteral("includeDlc"), s.includeDlc},
            {QStringLiteral("autoEmbedOnlineFix"), s.autoEmbedOnlineFix},
            {QStringLiteral("onlineFixOnWeakMultiplayer"), s.onlineFixOnWeakMultiplayer},
        };
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QJsonDocument(pub).toJson(QJsonDocument::Indented));
        return s;
    }
    if (!f.open(QIODevice::ReadOnly))
        return s;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    auto str = [&](const char* k, QString& dest) {
        if (o.contains(QLatin1String(k)))
            dest = o.value(QLatin1String(k)).toString(dest);
    };
    str("relayBaseUrl", s.relayBaseUrl);
    str("relayClientToken", s.relayClientToken);
    str("manifestCatalogUrl", s.manifestCatalogUrl);
    str("steamPath", s.steamPath);
    str("libraryFolder", s.libraryFolder);
    str("depotOs", s.depotOs);
    str("onlineFixSourceDir", s.onlineFixSourceDir);
    if (o.contains(QStringLiteral("manifestCatalogTtlMinutes")))
        s.manifestCatalogTtlMinutes = o.value(QStringLiteral("manifestCatalogTtlMinutes")).toDouble(5.0);
    else if (o.contains(QStringLiteral("manifestCatalogTtlHours"))) {
        const double h = o.value(QStringLiteral("manifestCatalogTtlHours")).toDouble(0);
        if (h > 0)
            s.manifestCatalogTtlMinutes = h * 60.0;
    }
    if (o.contains(QStringLiteral("includeDlc")))
        s.includeDlc = o.value(QStringLiteral("includeDlc")).toBool();
    if (o.contains(QStringLiteral("autoEmbedOnlineFix")))
        s.autoEmbedOnlineFix = o.value(QStringLiteral("autoEmbedOnlineFix")).toBool(true);
    if (o.contains(QStringLiteral("onlineFixOnWeakMultiplayer")))
        s.onlineFixOnWeakMultiplayer = o.value(QStringLiteral("onlineFixOnWeakMultiplayer")).toBool();
    if (s.relayBaseUrl.endsWith(QLatin1Char('/')))
        s.relayBaseUrl.chop(1);
    return s;
}

qint64 catalogTtlSeconds(const PluginSettings& s)
{
    return qMax(0.0, s.manifestCatalogTtlMinutes) * 60.0;
}

} // namespace steamidra
