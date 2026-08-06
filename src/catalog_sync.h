#pragma once

#include "catalog_types.h"
#include "plugin_settings.h"

#include <QString>
#include <QVector>

namespace steamidra {

/** Sync Ryuu catalog to cachePath; returns path to usable cache or empty. */
QString syncManifestCatalog(const PluginSettings& settings, const QString& cachePath, bool force);

QVector<arachnel::core::CatalogEntry> loadCatalogEntriesFromFile(const QString& path);

arachnel::core::CatalogEntry catalogEntryFromJson(const QJsonObject& obj);

/** Fill Steam Store DLC rows from relay GET /v1/app/{appid}/dlcs (blocking). */
void enrichCatalogEntryWithRelayDlcs(arachnel::core::CatalogEntry& entry, const PluginSettings& settings);

QJsonObject steamAppDetailsJson(const QString& appid);

} // namespace steamidra
