#include "steamidra_plugin.h"
#include "catalog_sync.h"
#include "plugin_api.h"
#include "plugin_catalog_json.h"

#include "catalog_types.h"

#include <cstdlib>
#include <cstring>

extern "C" {

int arachnel_plugin_api_version()
{
    return ARACHNEL_PLUGIN_API_VERSION;
}

int arachnel_plugin_catalog_entry_size()
{
    // Legacy canary for API 2/3 hosts; unused when apiVersion >= 4.
    return static_cast<int>(sizeof(arachnel::core::CatalogEntry));
}

arachnel::core::ISourcePlugin* arachnel_plugin_create(const char* plugin_root_utf8)
{
    const QString root =
        plugin_root_utf8 ? QString::fromUtf8(plugin_root_utf8) : QString();
    return new steamidra::SteamidraPlugin(root);
}

void arachnel_plugin_destroy(arachnel::core::ISourcePlugin* plugin)
{
    if (plugin)
        plugin->resetCatalogCache();
    delete plugin;
}

int arachnel_plugin_catalog_json(arachnel::core::ISourcePlugin* plugin, char** out_utf8,
                                 size_t* out_len)
{
    if (!plugin || !out_utf8 || !out_len)
        return -1;
    *out_utf8 = nullptr;
    *out_len = 0;
    auto* steam = dynamic_cast<steamidra::SteamidraPlugin*>(plugin);
    const QByteArray bytes = steam ? steam->catalogJsonUtf8()
                                   : arachnel::core::serializePluginCatalogJson(plugin->catalog());
    char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(bytes.size()) + 1));
    if (!buf)
        return -2;
    if (!bytes.isEmpty())
        std::memcpy(buf, bytes.constData(), static_cast<size_t>(bytes.size()));
    buf[bytes.size()] = '\0';
    *out_utf8 = buf;
    *out_len = static_cast<size_t>(bytes.size());
    return 0;
}

void arachnel_plugin_catalog_json_free(char* p)
{
    std::free(p);
}

// ---- Optional catalog-source extension (ext version 1) --------------------------------------
// Additive on purpose: apiVersion stays 4 and plugin.json minArachnel is unchanged. An older
// Arachnel never looks these symbols up and keeps using arachnel_plugin_catalog_json above, which
// must stay fully functional. Declared here (not only in plugin_api.h) so the plugin still builds
// against an SDK checkout that predates them.

namespace {
int copyOut(const QByteArray& bytes, char** out, size_t* outLen)
{
    char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(bytes.size()) + 1));
    if (!buf)
        return -2;
    if (!bytes.isEmpty())
        std::memcpy(buf, bytes.constData(), static_cast<size_t>(bytes.size()));
    buf[bytes.size()] = '\0';
    *out = buf;
    *outLen = static_cast<size_t>(bytes.size());
    return 0;
}
} // namespace

ARACHNEL_PLUGIN_EXPORT int arachnel_plugin_source_ext_version()
{
    return 1;
}

ARACHNEL_PLUGIN_EXPORT int arachnel_plugin_catalog_source(arachnel::core::ISourcePlugin* plugin,
                                                         char** out_json, size_t* out_len)
{
    if (!plugin || !out_json || !out_len)
        return -1;
    *out_json = nullptr;
    *out_len = 0;
    auto* steam = dynamic_cast<steamidra::SteamidraPlugin*>(plugin);
    if (!steam)
        return -1;
    const QByteArray bytes = steam->catalogSourceJson();
    if (bytes.isEmpty())
        return -4; // no catalog URL configured: host falls back to arachnel_plugin_catalog_json
    return copyOut(bytes, out_json, out_len);
}

ARACHNEL_PLUGIN_EXPORT int arachnel_plugin_normalize_rows(arachnel::core::ISourcePlugin* plugin,
                                                         const char* rows_json, size_t rows_len,
                                                         char** out_json, size_t* out_len)
{
    if (!plugin || !rows_json || !out_json || !out_len)
        return -1;
    *out_json = nullptr;
    *out_len = 0;
    const QByteArray bytes = steamidra::normalizeRowsJson(
        QByteArray::fromRawData(rows_json, static_cast<qsizetype>(rows_len)));
    if (bytes.isEmpty())
        return -5;
    return copyOut(bytes, out_json, out_len);
}

ARACHNEL_PLUGIN_EXPORT void arachnel_plugin_source_free(char* p)
{
    std::free(p);
}

} // extern "C"
