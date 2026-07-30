#include "online_fix_embed.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdint>
#include <cstring>

namespace {

bool writeMinimalPe(const QString& path, quint16 machine)
{
    // Tiny MZ/PE so peMachine() can read IMAGE_FILE_MACHINE.
    QByteArray buf(0x80, '\0');
    buf[0] = 'M';
    buf[1] = 'Z';
    const quint32 peOff = 0x40;
    buf[0x3C] = char(peOff & 0xff);
    buf[0x3D] = char((peOff >> 8) & 0xff);
    buf[peOff + 0] = 'P';
    buf[peOff + 1] = 'E';
    buf[peOff + 4] = char(machine & 0xff);
    buf[peOff + 5] = char((machine >> 8) & 0xff);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(buf) == buf.size();
}

bool touch(const QString& path, const QByteArray& body = QByteArrayLiteral("x"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(body) >= 0;
}

bool exists(const QString& path)
{
    return QFileInfo::exists(path);
}

int fail(const char* msg)
{
    qWarning().noquote() << "FAIL:" << msg;
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;

    const QString fromEnv = QString::fromLocal8Bit(qgetenv("STEAMIDRA_ROOT"));
#if defined(STEAMIDRA_DEFAULT_PLUGIN_ROOT)
    const QString pluginRoot = !fromEnv.isEmpty()
                                   ? fromEnv
                                   : QStringLiteral(STEAMIDRA_DEFAULT_PLUGIN_ROOT);
#else
    const QString pluginRoot = !fromEnv.isEmpty()
                                   ? fromEnv
                                   : (QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath()
                                      + QStringLiteral("/plugin-bundle"));
#endif
    const QString kitRoot = pluginRoot + QStringLiteral("/online_fix_kit");
    qInfo() << "pluginRoot=" << pluginRoot;
    if (!QDir(kitRoot).exists()) {
        qWarning() << "online_fix_kit missing at" << kitRoot;
        return 2;
    }

    QJsonObject settings;
    settings.insert(QStringLiteral("autoEmbedOnlineFix"), true);

    // --- Fixture A: Unity x64 (OF.me on Linux / SteamFix on Windows host kit) ---
    {
        QTemporaryDir tmp;
        const QString root = tmp.path() + QStringLiteral("/unity64");
        QDir().mkpath(root + QStringLiteral("/Game_Data"));
        writeMinimalPe(root + QStringLiteral("/Game.exe"), 0x8664);
        touch(root + QStringLiteral("/steam_api64.dll"));
        const auto r = steamidra::embedOnlineFix(root, QStringLiteral("123"), pluginRoot, settings, true);
        qInfo().noquote() << "unity64 ok=" << r.ok << "epic=" << r.epic << "err=" << r.error
                          << "targets=" << r.targets;
        if (!r.ok || !r.embedded)
            failures += fail("unity64 embed failed");
#if defined(Q_OS_WIN)
        if (!exists(root + QStringLiteral("/SteamFix64.dll"))
            || !exists(root + QStringLiteral("/winmm.dll"))
            || !exists(root + QStringLiteral("/SteamFix.ini")))
            failures += fail("unity64 expected FreeTP SteamFix layout");
        if (exists(root + QStringLiteral("/OnlineFix64.dll")))
            failures += fail("unity64 must not get OF.me on Windows kit");
#else
        if (!exists(root + QStringLiteral("/OnlineFix64.dll"))
            || !exists(root + QStringLiteral("/dlllist.txt"))
            || !exists(root + QStringLiteral("/OnlineFix.ini")))
            failures += fail("unity64 expected OF.me layout");
        if (exists(root + QStringLiteral("/SteamFix64.dll")))
            failures += fail("unity64 must not get FreeTP SteamFix on Linux kit");
#endif
        if (exists(root + QStringLiteral("/EpicFix64.dll")))
            failures += fail("unity64 must not get EpicFix without Redpoint");
    }

    // --- Fixture B: Unity x86 ---
    {
        QTemporaryDir tmp;
        const QString root = tmp.path() + QStringLiteral("/unity32");
        QDir().mkpath(root + QStringLiteral("/Game_Data"));
        writeMinimalPe(root + QStringLiteral("/Game.exe"), 0x14c);
        touch(root + QStringLiteral("/steam_api.dll"));
        const auto r = steamidra::embedOnlineFix(root, QStringLiteral("2749770"), pluginRoot, settings, true);
        qInfo().noquote() << "unity32 ok=" << r.ok << "err=" << r.error << "targets=" << r.targets;
        if (!r.ok || !r.embedded)
            failures += fail("unity32 embed failed");
#if defined(Q_OS_WIN)
        if (!exists(root + QStringLiteral("/SteamFix32.dll"))
            || !exists(root + QStringLiteral("/winmm.dll")))
            failures += fail("unity32 expected SteamFix32+winmm");
#else
        if (!exists(root + QStringLiteral("/OnlineFix.dll"))
            || !exists(root + QStringLiteral("/winmm.dll"))
            || !exists(root + QStringLiteral("/SteamOverlay32.dll")))
            failures += fail("unity32 expected OF.me OnlineFix.dll+winmm+Overlay32");
        if (exists(root + QStringLiteral("/OnlineFix64.dll"))
            || exists(root + QStringLiteral("/SteamFix64.dll")))
            failures += fail("unity32 must not keep x64 fix DLLs");
#endif
        if (exists(root + QStringLiteral("/EpicFix64.dll")))
            failures += fail("unity32 must not get EpicFix64");
    }

    // --- Fixture C: UE + RedpointEOS (EpicFix required) ---
    {
        QTemporaryDir tmp;
        const QString root = tmp.path() + QStringLiteral("/ue_eos");
        const QString win64 = root + QStringLiteral("/Game/Binaries/Win64");
        const QString redpoint = win64 + QStringLiteral("/RedpointEOS");
        QDir().mkpath(redpoint);
        writeMinimalPe(win64 + QStringLiteral("/Game-Win64-Shipping.exe"), 0x8664);
        touch(win64 + QStringLiteral("/steam_api64.dll"));
        touch(redpoint + QStringLiteral("/EOSSDK-Win64-Shipping.dll"));
        const auto r = steamidra::embedOnlineFix(root, QStringLiteral("4704690"), pluginRoot, settings, true);
        qInfo().noquote() << "ue_eos ok=" << r.ok << "epic=" << r.epic << "err=" << r.error
                          << "targets=" << r.targets;
        if (!r.ok || !r.embedded || !r.epic)
            failures += fail("ue_eos embed/epic failed");
        if (!exists(win64 + QStringLiteral("/EpicFix64.dll"))
            || !exists(win64 + QStringLiteral("/EpicFix.ini")))
            failures += fail("ue_eos missing EpicFix in Win64");
        if (!exists(redpoint + QStringLiteral("/EpicFix64.dll")))
            failures += fail("ue_eos missing EpicFix beside EOSSDK");
#if defined(Q_OS_WIN)
        if (!exists(win64 + QStringLiteral("/SteamFix64.dll")))
            failures += fail("ue_eos expected SteamFix on Windows");
#else
        if (!exists(win64 + QStringLiteral("/OnlineFix64.dll")))
            failures += fail("ue_eos expected OnlineFix64 on Linux");
        // dlllist should mention EpicFix
        QFile list(win64 + QStringLiteral("/dlllist.txt"));
        if (!list.open(QIODevice::ReadOnly)
            || !QString::fromUtf8(list.readAll()).contains(QStringLiteral("EpicFix64.dll")))
            failures += fail("ue_eos dlllist missing EpicFix64.dll");
#endif
    }

    // --- Fixture D: plain EOSSDK without Redpoint → no EpicFix ---
    {
        QTemporaryDir tmp;
        const QString root = tmp.path() + QStringLiteral("/plain_eos");
        QDir().mkpath(root + QStringLiteral("/Game_Data"));
        writeMinimalPe(root + QStringLiteral("/Game.exe"), 0x8664);
        touch(root + QStringLiteral("/steam_api64.dll"));
        touch(root + QStringLiteral("/EOSSDK-Win64-Shipping.dll"));
        const auto r = steamidra::embedOnlineFix(root, QStringLiteral("999"), pluginRoot, settings, true);
        qInfo().noquote() << "plain_eos ok=" << r.ok << "epic=" << r.epic << "err=" << r.error;
        if (!r.ok || !r.embedded)
            failures += fail("plain_eos embed failed");
        if (r.epic || exists(root + QStringLiteral("/EpicFix64.dll")))
            failures += fail("plain_eos must not get EpicFix");
    }

    qInfo() << "embed fixture failures=" << failures;
    return failures == 0 ? 0 : 1;
}
