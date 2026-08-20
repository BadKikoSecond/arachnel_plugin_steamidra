#include "steamidra_plugin.h"
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

} // extern "C"
