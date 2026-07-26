#include "catalog_sync.h"
#include "http_client.h"
#include "online_fix_embed.h"
#include "plugin_settings.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonObject>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;

    QString err;
    const QByteArray health = steamidra::httpGet(QStringLiteral("https://ryuu.badkiko.ru/health"), 30000, {}, &err);
    qInfo().noquote() << "health bytes=" << health.size() << "err=" << err;
    if (health.isEmpty())
        ++failures;

    const QString cache = QDir::temp().filePath(QStringLiteral("arachnel_smoke_catalog.json"));
    steamidra::PluginSettings settings;
    const QString path = steamidra::syncManifestCatalog(settings, cache, true);
    const auto entries = steamidra::loadCatalogEntriesFromFile(path);
    qInfo().noquote() << "catalog path=" << path << "entries=" << entries.size();
    if (entries.size() < 100)
        ++failures;

    QString luaErr;
    const QByteArray lua =
        steamidra::httpGet(QStringLiteral("https://ryuu.badkiko.ru/v1/download/4704690?file_type=lua"),
                           120000, {}, &luaErr);
    qInfo().noquote() << "lua bytes=" << lua.size() << "head=" << QString::fromUtf8(lua.left(60))
                      << "err=" << luaErr;
    if (!QString::fromUtf8(lua).toLower().contains(QStringLiteral("addappid")))
        ++failures;

    const QString meccha = QStringLiteral("D:/ArachnelGame/steam-4704690");
    if (QDir(meccha).exists()) {
        QJsonObject st;
        st.insert(QStringLiteral("autoEmbedOnlineFix"), true);
        const auto r = steamidra::embedOnlineFix(
            meccha, QStringLiteral("4704690"),
            QStringLiteral("D:/PetWork/arachnel-plugin-steamidra"), st, true);
        qInfo().noquote() << "embed ok=" << r.ok << "embedded=" << r.embedded << "epic=" << r.epic
                          << "err=" << r.error << "targets=" << r.targets;
        const bool epicBeside =
            QFile::exists(meccha + QStringLiteral("/Chameleon/Binaries/Win64/RedpointEOS/EpicFix64.dll"));
        qInfo().noquote() << "RedpointEOS/EpicFix64.dll=" << epicBeside;
        qInfo().noquote() << "launch=" << steamidra::findLaunchExeForInstall(meccha);
        if (!r.ok || !r.embedded || !epicBeside)
            ++failures;
    } else {
        qInfo() << "Meccha install not present — skip embed smoke";
    }

    qInfo() << "failures=" << failures;
    return failures == 0 ? 0 : 1;
}
