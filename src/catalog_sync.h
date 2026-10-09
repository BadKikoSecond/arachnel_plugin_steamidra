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

/**
 * Catalog-source extension (host-driven sync, see Arachnel plugin_api.h): descriptor JSON the
 * host uses to download the raw feed itself, empty when no catalog URL is configured.
 */
QByteArray catalogSourceDescriptorJson(const PluginSettings& settings);

/**
 * Normalizes a JSON array of raw relay rows into
 * {"schema":"arachnel.plugin.catalog.v1","entries":[...],"supersedes":["steam-<appid>",...]}
 * where `supersedes` are DLC appids referenced by a game row (they are listed under the game).
 * Pure function of its input; safe to call from several threads.
 */
QByteArray normalizeRowsJson(const QByteArray& rowsJsonArray);

QJsonObject steamAppDetailsJson(const QString& appid);

} // namespace steamidra
