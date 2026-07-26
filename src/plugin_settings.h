#pragma once

#include <QJsonObject>
#include <QString>

namespace steamidra {

inline constexpr auto kDefaultRelayBase = "https://ryuu.badkiko.ru";
inline constexpr auto kDefaultCatalogUrl = "https://ryuu.badkiko.ru/v1/catalog";

struct PluginSettings {
    QString relayBaseUrl = QString::fromUtf8(kDefaultRelayBase);
    QString relayClientToken;
    QString manifestCatalogUrl = QString::fromUtf8(kDefaultCatalogUrl);
    double manifestCatalogTtlMinutes = 5.0;
    QString steamPath;
    QString libraryFolder;
    QString depotOs = QStringLiteral("windows");
    bool includeDlc = false;
    bool autoEmbedOnlineFix = true;
    bool onlineFixOnWeakMultiplayer = false;
    QString onlineFixSourceDir;
    QString settingsPath;
};

PluginSettings loadPluginSettings(const QString& settingsFilePath);
qint64 catalogTtlSeconds(const PluginSettings& s);

} // namespace steamidra
