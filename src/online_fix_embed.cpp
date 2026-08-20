#include "online_fix_embed.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <string>

namespace steamidra {
namespace {

QString pickOverlayExe(const QString& target);

enum class KitFlavor { None, SteamFix, OnlineFix };

bool kitLooksSteamFix(const QString& dir)
{
    return QFileInfo::exists(dir + QStringLiteral("/SteamFix64.dll"))
           && QFileInfo::exists(dir + QStringLiteral("/winmm.dll"));
}

bool kitLooksOnlineFix(const QString& dir)
{
    return QFileInfo::exists(dir + QStringLiteral("/OnlineFix64.dll"))
           || QFileInfo::exists(dir + QStringLiteral("/OnlineFix.dll"));
}

KitFlavor detectKitFlavor(const QString& dir)
{
    if (kitLooksOnlineFix(dir))
        return KitFlavor::OnlineFix;
    if (kitLooksSteamFix(dir))
        return KitFlavor::SteamFix;
    return KitFlavor::None;
}

bool kitUsable(const QString& dir)
{
    return detectKitFlavor(dir) != KitFlavor::None;
}

QString hostKitSubdir()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

bool extractZipToDir(const QString& zipPath, const QString& destDir)
{
    QDir().mkpath(destDir);
#if defined(Q_OS_WIN)
    QProcess p;
    p.start(QStringLiteral("tar"),
            {QStringLiteral("-xf"), zipPath, QStringLiteral("-C"), destDir});
    if (!p.waitForFinished(120000) || p.exitCode() != 0) {
        QProcess ps;
        const QString cmd = QStringLiteral(
            "Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                                .arg(zipPath, destDir);
        ps.start(QStringLiteral("powershell"),
                 {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), cmd});
        return ps.waitForFinished(120000) && ps.exitCode() == 0;
    }
    return true;
#else
    QProcess p;
    p.start(QStringLiteral("unzip"), {QStringLiteral("-o"), zipPath, QStringLiteral("-d"), destDir});
    if (p.waitForFinished(120000) && p.exitCode() == 0)
        return true;
    QProcess tar;
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("-xf"), zipPath, QStringLiteral("-C"), destDir});
    return tar.waitForFinished(120000) && tar.exitCode() == 0;
#endif
}

QString pickBestKitDir(const QString& root)
{
    if (kitUsable(root))
        return root;
    const QString host = root + QLatin1Char('/') + hostKitSubdir();
    if (kitUsable(host))
        return host;
#if !defined(Q_OS_WIN)
    // Universal packages ship the Linux kit as linux.zip so Windows Defender never
    // sees loose OnlineFix*.dll under %AppData%. Extract once on Linux hosts only.
    const QString linuxDir = root + QStringLiteral("/linux");
    const QString linuxZip = root + QStringLiteral("/linux.zip");
    if (!kitUsable(linuxDir) && QFileInfo::exists(linuxZip)) {
        QDir().mkpath(linuxDir);
        if (extractZipToDir(linuxZip, linuxDir) && kitUsable(linuxDir))
            return linuxDir;
    }
    if (kitUsable(linuxDir))
        return linuxDir;
    // Incomplete linux kit: SteamFix x64 under windows/ still works on Proton.
    const QString win = root + QStringLiteral("/windows");
    if (kitLooksSteamFix(win))
        return win;
#endif
    return root;
}

QString kitDir(const QString& pluginRoot, const QJsonObject& settings)
{
    const QString configured = settings.value(QStringLiteral("onlineFixSourceDir")).toString().trimmed();
    if (!configured.isEmpty() && kitUsable(configured))
        return configured;
    const QString bundled = pluginRoot + QStringLiteral("/online_fix_kit");
    return pickBestKitDir(bundled);
}

bool copyFileBestEffort(const QString& src, const QString& dst)
{
    if (!QFileInfo::exists(src))
        return false;
    QFile::remove(dst);
    return QFile::copy(src, dst);
}

void writeEpicFixIni(const QString& path)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write("[EOS]\r\nNoAuth=1\r\nDeviceAuth=true\r\n");
    f.commit();
}

void writeWinmmTxt(const QString& path, bool includeEpic, bool use32Bit)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    // EpicFix64 is x64-only - never list it for 32-bit winmm loaders (Isaac etc.).
    QByteArray body = use32Bit ? "SteamFix32.dll" : "SteamFix64.dll";
    if (includeEpic && !use32Bit)
        body += "\r\nEpicFix64.dll";
    f.write(body);
    f.commit();
}

void writeSteamAppIdFile(const QString& dir, const QString& fakeAppId = QStringLiteral("480"))
{
    if (dir.isEmpty())
        return;
    QSaveFile f(dir + QStringLiteral("/steam_appid.txt"));
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(fakeAppId.toUtf8());
    f.write("\n");
    f.commit();
}

quint16 peMachine(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    if (!file.seek(0x3C))
        return 0;
    const QByteArray peOffsetBytes = file.read(4);
    if (peOffsetBytes.size() < 4)
        return 0;
    const quint32 peOffset = quint32(quint8(peOffsetBytes[0]))
        | (quint32(quint8(peOffsetBytes[1])) << 8)
        | (quint32(quint8(peOffsetBytes[2])) << 16)
        | (quint32(quint8(peOffsetBytes[3])) << 24);
    if (!file.seek(qint64(peOffset) + 4))
        return 0;
    const QByteArray machineBytes = file.read(2);
    if (machineBytes.size() < 2)
        return 0;
    return quint16(quint8(machineBytes[0])) | (quint16(quint8(machineBytes[1])) << 8);
}

constexpr quint16 kPeI386 = 0x14c;
constexpr quint16 kPeAmd64 = 0x8664;

QString resolveKitWinmm32(const QString& kit)
{
    const QString named = kit + QStringLiteral("/winmm32.dll");
    if (QFileInfo::exists(named) && peMachine(named) == kPeI386)
        return named;
    const QString plain = kit + QStringLiteral("/winmm.dll");
    if (QFileInfo::exists(plain) && peMachine(plain) == kPeI386)
        return plain;
    const QString opt = kit + QStringLiteral("/x86_optional/winmm32.dll");
    if (QFileInfo::exists(opt) && peMachine(opt) == kPeI386)
        return opt;
    return {};
}

/** Resolve 32-bit SteamFix pair without keeping PE next to the plugin after embed. */
struct X86KitResolved {
    QString steamFix32;
    QString winmm32;
    QTemporaryDir temp;
    bool ok = false;
};

X86KitResolved resolveX86SteamFixKit(const QString& kit)
{
    X86KitResolved out;
    auto tryPair = [&](const QString& fix, const QString& winmm) -> bool {
        if (!QFileInfo::exists(fix) || !QFileInfo::exists(winmm))
            return false;
        if (peMachine(fix) != kPeI386)
            return false;
        out.steamFix32 = fix;
        out.winmm32 = winmm;
        out.ok = true;
        return true;
    };

    if (tryPair(kit + QStringLiteral("/SteamFix32.dll"), kit + QStringLiteral("/winmm32.dll")))
        return out;
    if (tryPair(kit + QStringLiteral("/x86_optional/SteamFix32.dll"),
                kit + QStringLiteral("/x86_optional/winmm32.dll")))
        return out;

    const QString zip = kit + QStringLiteral("/x86_optional.zip");
    if (!QFileInfo::exists(zip) || !out.temp.isValid())
        return out;
    if (!extractZipToDir(zip, out.temp.path()))
        return out;
    if (tryPair(out.temp.path() + QStringLiteral("/SteamFix32.dll"),
                out.temp.path() + QStringLiteral("/winmm32.dll")))
        return out;
    QDirIterator it(out.temp.path(), {QStringLiteral("SteamFix32.dll")}, QDir::Files,
                    QDirIterator::Subdirectories);
    if (it.hasNext()) {
        const QString fix = it.next();
        tryPair(fix, QFileInfo(fix).absolutePath() + QStringLiteral("/winmm32.dll"));
    }
    return out;
}

/** online-fix.me 32-bit layout (Galaxy Burger etc.): OnlineFix.dll + SteamOverlay32 + winmm. */
struct X86OnlineFixResolved {
    QString onlineFix; // OnlineFix.dll or OnlineFix32.dll
    QString overlay32;
    QString stub32;
    QString winmm32;
    QTemporaryDir temp;
    bool ok = false;
};

X86OnlineFixResolved resolveX86OnlineFixKit(const QString& kit)
{
    X86OnlineFixResolved out;
    auto accept = [&](const QString& dir) -> bool {
        const QString of32 = dir + QStringLiteral("/OnlineFix32.dll");
        const QString of = dir + QStringLiteral("/OnlineFix.dll");
        QString fix;
        if (QFileInfo::exists(of32) && peMachine(of32) == kPeI386)
            fix = of32;
        else if (QFileInfo::exists(of) && peMachine(of) == kPeI386)
            fix = of;
        else
            return false;
        const QString winmmNamed = dir + QStringLiteral("/winmm32.dll");
        const QString winmmPlain = dir + QStringLiteral("/winmm.dll");
        QString winmm;
        if (QFileInfo::exists(winmmNamed) && peMachine(winmmNamed) == kPeI386)
            winmm = winmmNamed;
        else if (QFileInfo::exists(winmmPlain) && peMachine(winmmPlain) == kPeI386)
            winmm = winmmPlain;
        if (winmm.isEmpty())
            return false;
        out.onlineFix = fix;
        out.winmm32 = winmm;
        const QString ov = dir + QStringLiteral("/SteamOverlay32.dll");
        if (QFileInfo::exists(ov) && peMachine(ov) == kPeI386)
            out.overlay32 = ov;
        const QString stub = dir + QStringLiteral("/StubDRM32.dll");
        if (QFileInfo::exists(stub) && peMachine(stub) == kPeI386)
            out.stub32 = stub;
        out.ok = true;
        return true;
    };

    if (accept(kit) || accept(kit + QStringLiteral("/x86_optional")))
        return out;

    const QString zip = kit + QStringLiteral("/x86_optional.zip");
    if (!QFileInfo::exists(zip) || !out.temp.isValid())
        return out;
    if (!extractZipToDir(zip, out.temp.path()))
        return out;
    if (accept(out.temp.path()))
        return out;
    // Nested folder from zip without -j
    QDirIterator it(out.temp.path(), {QStringLiteral("OnlineFix.dll"), QStringLiteral("OnlineFix32.dll")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (accept(QFileInfo(it.filePath()).absolutePath()))
            return out;
    }
    return out;
}

void writeOnlineFixIni(const QString& path, const QString& realAppId, const QString& fakeAppId)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    QByteArray body = "[Main]\r\n";
    if (!realAppId.isEmpty() && realAppId != QStringLiteral("0")) {
        body += "RealAppId=";
        body += realAppId.toUtf8();
        body += "\r\n";
    }
    body += "FakeAppId=";
    body += fakeAppId.toUtf8();
    body += "\r\n";
    f.write(body);
    f.commit();
}

void writeDllListTxt(const QString& path, const QStringList& dllNames)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    QByteArray body;
    for (const QString& name : dllNames) {
        if (name.isEmpty())
            continue;
        body += name.toUtf8();
        body += "\r\n";
    }
    f.write(body);
    f.commit();
}

bool copyKitFileIfExists(const QString& kit, const QString& name, const QString& targetDir)
{
    const QString src = kit + QLatin1Char('/') + name;
    if (!QFileInfo::exists(src))
        return false;
    return copyFileBestEffort(src, targetDir + QLatin1Char('/') + name);
}

bool peHasExportName(const QString& path, const char* exportName)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    if (data.size() < 0x40)
        return false;
    const auto u8 = [&](int off) -> quint8 {
        return off >= 0 && off < data.size() ? quint8(data[off]) : 0;
    };
    const auto u16 = [&](int off) -> quint16 {
        return quint16(u8(off)) | (quint16(u8(off + 1)) << 8);
    };
    const auto u32 = [&](int off) -> quint32 {
        return quint32(u16(off)) | (quint32(u16(off + 2)) << 16);
    };
    if (u16(0) != 0x5A4D)
        return false;
    const quint32 pe = u32(0x3C);
    if (int(pe) + 24 >= data.size() || u32(int(pe)) != 0x00004550)
        return false;
    const quint16 magic = u16(int(pe) + 24);
    const int ddOff = magic == 0x10B ? int(pe) + 24 + 96 : (magic == 0x20B ? int(pe) + 24 + 112 : -1);
    if (ddOff < 0)
        return false;
    const quint32 exportRva = u32(ddOff);
    if (exportRva == 0)
        return false;
    const quint16 numSections = u16(int(pe) + 6);
    const quint16 optSize = u16(int(pe) + 20);
    const int secOff = int(pe) + 24 + optSize;
    auto rvaToOff = [&](quint32 rva) -> int {
        for (int i = 0; i < int(numSections); ++i) {
            const int s = secOff + i * 40;
            if (s + 40 > data.size())
                break;
            const quint32 va = u32(s + 12);
            const quint32 rawSize = u32(s + 16);
            const quint32 rawPtr = u32(s + 20);
            const quint32 virtSize = u32(s + 8);
            const quint32 span = qMax(rawSize, virtSize);
            if (rva >= va && rva < va + span && rawPtr != 0)
                return int(rva - va + rawPtr);
        }
        return -1;
    };
    const int expOff = rvaToOff(exportRva);
    if (expOff < 0 || expOff + 40 > data.size())
        return false;
    const quint32 namesRva = u32(expOff + 32);
    const quint32 namesCount = u32(expOff + 24);
    const int namesOff = rvaToOff(namesRva);
    if (namesOff < 0)
        return false;
    const QByteArray needle(exportName);
    for (quint32 i = 0; i < namesCount && i < 4096; ++i) {
        const int entry = namesOff + int(i) * 4;
        if (entry + 4 > data.size())
            break;
        const int nameOff = rvaToOff(u32(entry));
        if (nameOff < 0 || nameOff >= data.size())
            continue;
        const int end = data.indexOf('\0', nameOff);
        if (end < 0)
            continue;
        if (data.mid(nameOff, end - nameOff) == needle)
            return true;
    }
    return false;
}

void ensureClassicSteamApiAt(const QString& api)
{
    // Old titles (Isaac etc.) need classic SteamAPI_Init / SteamUser exports.
    // Modern cracked steam_api (OF/Goldberg flat API) is ~260KB+ and lacks those
    // names → STATUS_ENTRYPOINT_NOT_FOUND. SteamFix/OnlineFix hooks the Valve DLL.
    if (!QFileInfo::exists(api))
        return;
    if (peHasExportName(api, "SteamAPI_Init"))
        return;
    const QStringList backups = {
        api + QStringLiteral(".valve-bak"),
        api + QStringLiteral(".arachnel-valve-bak"),
        api + QStringLiteral(".bak"),
        QFileInfo(api).absolutePath() + QStringLiteral("/steam_api.dll.bak"),
    };
    for (const QString& bak : backups) {
        if (!QFileInfo::exists(bak) || !peHasExportName(bak, "SteamAPI_Init"))
            continue;
        const QString modernBak = api + QStringLiteral(".arachnel-modern-bak");
        QFile::remove(modernBak);
        QFile::rename(api, modernBak);
        QFile::copy(bak, api);
        return;
    }
}

void ensureClassicSteamApi(const QString& target)
{
    ensureClassicSteamApiAt(target + QStringLiteral("/steam_api.dll"));
    // Unity often loads steam_api from Plugins/x86 (Galaxy Burger / OF.me x86 layout).
    ensureClassicSteamApiAt(target + QStringLiteral("/Plugins/x86/steam_api.dll"));
    QDirIterator dataDirs(target, {QStringLiteral("*_Data")}, QDir::Dirs);
    while (dataDirs.hasNext()) {
        const QString data = dataDirs.next();
        ensureClassicSteamApiAt(data + QStringLiteral("/Plugins/x86/steam_api.dll"));
        ensureClassicSteamApiAt(data + QStringLiteral("/Plugins/x86_64/steam_api64.dll"));
    }
}

bool peHasBindSection(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    if (data.size() < 0x40)
        return false;
    const auto u8 = [&](int off) -> quint8 {
        return off >= 0 && off < data.size() ? quint8(data[off]) : 0;
    };
    const auto u16 = [&](int off) -> quint16 {
        return quint16(u8(off)) | (quint16(u8(off + 1)) << 8);
    };
    const auto u32 = [&](int off) -> quint32 {
        return quint32(u16(off)) | (quint32(u16(off + 2)) << 16);
    };
    if (u16(0) != 0x5A4D)
        return false;
    const quint32 pe = u32(0x3C);
    if (int(pe) + 24 >= data.size() || u32(int(pe)) != 0x00004550)
        return false;
    const quint16 numSections = u16(int(pe) + 6);
    const quint16 optSize = u16(int(pe) + 20);
    const int secOff = int(pe) + 24 + optSize;
    for (int i = 0; i < int(numSections); ++i) {
        const int s = secOff + i * 40;
        if (s + 8 > data.size())
            break;
        const QByteArray name = data.mid(s, 8);
        const int z = name.indexOf('\0');
        const QByteArray stem = z >= 0 ? name.left(z) : name;
        if (stem == ".bind")
            return true;
    }
    return false;
}

bool tryUnpackSteamStub(const QString& exePath, const QString& kit)
{
#if !defined(Q_OS_WIN)
    Q_UNUSED(exePath);
    Q_UNUSED(kit);
    return false;
#else
    if (exePath.isEmpty() || !QFileInfo::exists(exePath) || !peHasBindSection(exePath))
        return false;
    const QString cli = kit + QStringLiteral("/steamless/Steamless.CLI.exe");
    if (!QFileInfo::exists(cli))
        return false;
    const QString unpacked = exePath + QStringLiteral(".unpacked.exe");
    QFile::remove(unpacked);
    QProcess proc;
    proc.setProgram(cli);
    proc.setArguments({exePath});
    proc.setWorkingDirectory(QFileInfo(exePath).absolutePath());
    proc.start();
    if (!proc.waitForFinished(180000) || proc.exitStatus() != QProcess::NormalExit)
        return false;
    if (!QFileInfo::exists(unpacked))
        return false;
    const QString bak = exePath + QStringLiteral(".steamstub-bak");
    if (!QFileInfo::exists(bak))
        QFile::copy(exePath, bak);
    QFile::remove(exePath);
    if (!QFile::copy(unpacked, exePath))
        return false;
    return true;
#endif
}

void stripWrongArchOverlay(const QString& target, bool want32)
{
    auto removeIf = [&](const QString& name, quint16 badMachine) {
        const QString path = target + QLatin1Char('/') + name;
        if (QFileInfo::exists(path) && peMachine(path) == badMachine)
            QFile::remove(path);
    };
    if (want32) {
        removeIf(QStringLiteral("winmm.dll"), kPeAmd64);
        removeIf(QStringLiteral("SteamFix64.dll"), kPeAmd64);
        removeIf(QStringLiteral("EpicFix64.dll"), kPeAmd64);
        removeIf(QStringLiteral("OnlineFix64.dll"), kPeAmd64);
        removeIf(QStringLiteral("SteamOverlay64.dll"), kPeAmd64);
        removeIf(QStringLiteral("StubDRM64.dll"), kPeAmd64);
        // Drop known x64 leftovers even if PE read fails.
        QFile::remove(target + QStringLiteral("/SteamFix64.dll"));
        QFile::remove(target + QStringLiteral("/EpicFix64.dll"));
        QFile::remove(target + QStringLiteral("/EpicFix.ini"));
        QFile::remove(target + QStringLiteral("/OnlineFix64.dll"));
        QFile::remove(target + QStringLiteral("/SteamOverlay64.dll"));
        QFile::remove(target + QStringLiteral("/StubDRM64.dll"));
    } else {
        removeIf(QStringLiteral("winmm.dll"), kPeI386);
        removeIf(QStringLiteral("SteamFix32.dll"), kPeI386);
        removeIf(QStringLiteral("OnlineFix32.dll"), kPeI386);
        removeIf(QStringLiteral("SteamOverlay32.dll"), kPeI386);
        removeIf(QStringLiteral("StubDRM32.dll"), kPeI386);
        // Unversioned OnlineFix.dll is usually the 32-bit OF.me payload.
        const QString of = target + QStringLiteral("/OnlineFix.dll");
        if (QFileInfo::exists(of) && peMachine(of) == kPeI386)
            QFile::remove(of);
        QFile::remove(target + QStringLiteral("/SteamFix32.dll"));
        QFile::remove(target + QStringLiteral("/OnlineFix32.dll"));
        QFile::remove(target + QStringLiteral("/SteamOverlay32.dll"));
        QFile::remove(target + QStringLiteral("/StubDRM32.dll"));
    }
}

void writeSteamFixIni(const QString& path, const QString& appid, bool workshop, bool overlay,
                      bool unlockAllDlc = true)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    QByteArray t;
    t += "[Main]\r\n";
    t += "RealAppId=" + appid.toUtf8() + "\r\n";
    t += "FakeAppId=480\r\n#Language=english\r\nBuildId=0\r\n\r\n";
    t += "[Misc]\r\nOverlay=";
    t += overlay ? "true" : "false";
    t += "\r\nFilterLobby=false\r\n";
    if (workshop)
        t += "FakeWorkshop=true\r\n";
    // SteamFix32/64: UnlockAllDLC is the real "own everything" switch; [DLC] 0=dlc alone
    // is not enough for Isaac Repentance+ (needs Afterbirth ownership).
    t += "UnlockAllDLC=";
    t += unlockAllDlc ? "true" : "false";
    t += "\r\n";
    t += "\r\n[Interfaces]\r\nApps=true\r\nFriends=true\r\nUser=true\r\nInventory=true\r\n";
    t += "Stats=true\r\nStorage=true\r\nUtils=true\r\n";
    t += workshop ? "Workshop=true\r\n" : "Workshop=false\r\n";
    t += "\r\n[DLC]\r\n";
    if (unlockAllDlc)
        t += "0=dlc\r\n";
    f.write(t);
    f.commit();
}

#if defined(Q_OS_WIN)
bool setRegDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                        nullptr)
        != ERROR_SUCCESS) {
        if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
            != ERROR_SUCCESS)
            return false;
    }
    const LONG ok = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                                   sizeof(value));
    RegCloseKey(key);
    return ok == ERROR_SUCCESS;
}

void seedSteamFixRegistry(const QString& realAppId)
{
    // SteamFix writes SOFTWARE\Valve\Steam (+ Apps\<id>\Installed). Prefill HKCU so
    // launch does not need elevation when the key is missing / read-only under HKLM.
    setRegDword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\Apps\\480", L"Installed", 1);
    if (!realAppId.isEmpty()) {
        const std::wstring sub =
            L"Software\\Valve\\Steam\\Apps\\" + realAppId.toStdWString();
        setRegDword(HKEY_CURRENT_USER, sub.c_str(), L"Installed", 1);
    }
    // Isaac Repentance+ checks Afterbirth via Apps\<dlc>\Installed - seed known DLC ids.
    if (realAppId == QLatin1String("250900")) {
        for (const wchar_t* dlc : {L"401920", L"570660", L"1426300", L"3353470", L"322660"}) {
            const std::wstring sub = std::wstring(L"Software\\Valve\\Steam\\Apps\\") + dlc;
            setRegDword(HKEY_CURRENT_USER, sub.c_str(), L"Installed", 1);
        }
    }
    setRegDword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"RunningAppID", 480);
}
#else
void seedSteamFixRegistry(const QString&) {}
#endif

/** SteamFix opens freetp.org via `cmd.exe /c start "link" "%s"` when FreeTP/.hash
 *  is missing or was copied from another path (4-byte path-bound ack). Overlay=false
 *  does NOT stop that. Planting a no-op cmd.exe next to the game wins CreateProcess
 *  search order (app dir before System32) on Windows and under Proton/Wine, so the
 *  promo never reaches the real shell. The game still writes a path-local .hash on
 *  first launch. Never replace Wine prefix system32/cmd.exe. */
bool plantFreetpCmdStub(const QString& target, const QString& kit)
{
    const QString src = kit + QStringLiteral("/cmd_stub.exe");
    if (!QFileInfo::exists(src) || target.isEmpty() || !QDir(target).exists())
        return false;
    return copyFileBestEffort(src, target + QStringLiteral("/cmd.exe"));
}

void ensureFreetpAckFolder(const QString& target, const QString& kit)
{
    if (target.isEmpty() || !QDir(target).exists())
        return;

    QDir().mkpath(target + QStringLiteral("/FreeTP/UserData"));
    QFile anon(target + QStringLiteral("/FreeTP/UserData/AnonFolderSave.txt"));
    if (!anon.exists() && anon.open(QIODevice::WriteOnly))
        anon.close();

    plantFreetpCmdStub(target, kit);

    // CreateProcess searches the main exe directory - plant there too when needed.
    const QString launchExe = pickOverlayExe(target);
    if (!launchExe.isEmpty()) {
        const QString exeDir = QFileInfo(launchExe).absolutePath();
        if (QFileInfo(exeDir).absoluteFilePath() != QFileInfo(target).absoluteFilePath()) {
            QDir().mkpath(exeDir + QStringLiteral("/FreeTP/UserData"));
            plantFreetpCmdStub(exeDir, kit);
        }
    }
}

quint16 resolveOverlayArch(const QString& target, const QString& launchExe)
{
    const quint16 fromExe = peMachine(launchExe);
    if (fromExe == kPeI386 || fromExe == kPeAmd64)
        return fromExe;

    const QString api64 = target + QStringLiteral("/steam_api64.dll");
    if (QFileInfo::exists(api64)) {
        const quint16 m = peMachine(api64);
        if (m == kPeI386 || m == kPeAmd64)
            return m;
        return kPeAmd64;
    }
    const QString api32 = target + QStringLiteral("/steam_api.dll");
    if (QFileInfo::exists(api32)) {
        const quint16 m = peMachine(api32);
        if (m == kPeI386 || m == kPeAmd64)
            return m;
    }
    return kPeAmd64;
}

bool skipOverlayPath(const QString& path)
{
    const QString l = path.toLower();
    static const char* skip[] = {".depotdownloader", "staging", "intermediate", "saved",
                                 "deriveddatacache", "thirdparty", "downloads", "proton", ".git"};
    for (const char* s : skip) {
        if (l.contains(QLatin1String(s)))
            return true;
    }
    return false;
}

bool gameNeedsEpicFix(const QString& root)
{
    // FreeTP policy: EpicFix only for RedpointEOS (Meccha etc.).
    // Plain EOSSDK*.dll alone (Pratfall) gets SteamFix/OnlineFix without EpicFix -
    // forcing EpicFix opens freetp.org and can black-screen.
    // EpicFix64 is x64-only; Win32 Redpoint/EOSSDK must not receive it.
    QDirIterator it(root, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString name = it.fileName();
        if (it.fileInfo().isDir() && name.compare(QStringLiteral("RedpointEOS"), Qt::CaseInsensitive) == 0)
            return true;
        if (!it.fileInfo().isFile())
            continue;
        if (name.startsWith(QStringLiteral("RedpointEOS"), Qt::CaseInsensitive)
            && name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive)) {
            const quint16 m = peMachine(it.filePath());
            if (m == kPeAmd64 || m == 0)
                return true;
        }
    }
    return false;
}

bool gameHasSteamApi(const QString& root)
{
    QDirIterator it(root,
                    {QStringLiteral("steam_api64.dll"), QStringLiteral("steam_api.dll")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!skipOverlayPath(it.filePath()))
            return true;
    }
    return false;
}

bool isElectronInstall(const QString& root)
{
    // SteamFix/winmm is for native Steamworks. Electron (greenworks/steamworks.js)
    // has no steam_api.dll - embedding SteamFix just pops the registry dialog.
    if (QFileInfo::exists(root + QStringLiteral("/LICENSE.electron.txt")))
        return true;
    if (QFileInfo::exists(root + QStringLiteral("/resources/app.asar")))
        return true;
    if (QFileInfo::exists(root + QStringLiteral("/chrome_100_percent.pak"))
        && QFileInfo::exists(root + QStringLiteral("/v8_context_snapshot.bin")))
        return true;
    return false;
}

bool hasSteamworksJs(const QString& root)
{
    const QString base = root + QStringLiteral("/resources/app.asar.unpacked/node_modules/steamworks.js");
    return QFileInfo::exists(base + QStringLiteral("/index.js"))
           || QFileInfo::exists(base + QStringLiteral("/dist/win64/steam_api64.dll"));
}

bool rewriteSteamworksJsInitAppId(const QString& installPath, const QString& realAppId)
{
    // steamworks.js init(realAppId) ignores steam_appid.txt. Force FakeAppId 480 with a
    // same-length asar patch so asar file offsets stay valid.
    if (realAppId.isEmpty() || realAppId == QLatin1String("480"))
        return false;
    for (const QChar c : realAppId) {
        if (!c.isDigit())
            return false;
    }
    const QString asarPath = installPath + QStringLiteral("/resources/app.asar");
    if (!QFileInfo::exists(asarPath))
        return false;
    if (!hasSteamworksJs(installPath))
        return false;

    QFile file(asarPath);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    QByteArray data = file.readAll();
    const QByteArray needle = QByteArrayLiteral(".init(") + realAppId.toUtf8() + ')';
    const QByteArray prefix = QByteArrayLiteral(".init(480)");
    if (needle.size() < prefix.size())
        return false;
    const QByteArray replacement = prefix + QByteArray(needle.size() - prefix.size(), ' ');

    int replaced = 0;
    int from = 0;
    while (true) {
        const int idx = data.indexOf(needle, from);
        if (idx < 0)
            break;
        for (int i = 0; i < replacement.size(); ++i)
            data[idx + i] = replacement.at(i);
        from = idx + replacement.size();
        ++replaced;
    }
    if (replaced == 0)
        return false;
    if (!file.resize(0) || !file.seek(0) || file.write(data) != data.size())
        return false;
    return true;
}

void stripSteamFixOverlay(const QString& target)
{
    for (const QString& name :
         {QStringLiteral("winmm.dll"), QStringLiteral("winmm.txt"), QStringLiteral("dlllist.txt"),
          QStringLiteral("SteamFix64.dll"), QStringLiteral("SteamFix32.dll"),
          QStringLiteral("SteamFix.ini"), QStringLiteral("OnlineFix64.dll"),
          QStringLiteral("OnlineFix.dll"), QStringLiteral("OnlineFix.ini"),
          QStringLiteral("SteamOverlay64.dll"), QStringLiteral("SteamOverlay32.dll"),
          QStringLiteral("StubDRM64.dll"), QStringLiteral("StubDRM32.dll"),
          QStringLiteral("EpicFix64.dll"), QStringLiteral("EpicFix.ini"),
          QStringLiteral("cmd.exe")}) {
        QFile::remove(target + QLatin1Char('/') + name);
    }
}

/** EpicFix lives in FreeTP windows kit; OF.me linux kit keeps it as sibling windows/. */
QString resolveEpicFixDir(const QString& kit, const QString& pluginRoot)
{
    const QStringList candidates = {
        kit,
        kit + QStringLiteral("/windows"),
        QFileInfo(kit).absolutePath() + QStringLiteral("/windows"),
        pluginRoot + QStringLiteral("/online_fix_kit/windows"),
        pluginRoot + QStringLiteral("/online_fix_kit"),
    };
    for (const QString& dir : candidates) {
        if (QFileInfo::exists(dir + QStringLiteral("/EpicFix64.dll")))
            return dir;
    }
    return {};
}

bool gameHasSteamApi(const QString& root)
{
    QDirIterator it(root,
                    {QStringLiteral("steam_api64.dll"), QStringLiteral("steam_api.dll")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!skipOverlayPath(it.filePath()))
            return true;
    }
    return false;
}

QStringList findUeShippingDirs(const QString& root)
{
    QStringList out;
    QDirIterator it(root, {QStringLiteral("*Shipping*.exe"), QStringLiteral("*-Win64-Shipping.exe")},
                    QDir::Files, QDirIterator::Subdirectories);
    QSet<QString> seen;
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString lower = it.fileName().toLower();
        if (lower.contains(QStringLiteral("crash")) || lower.contains(QStringLiteral("prereq")))
            continue;
        const QString parent = it.fileInfo().absolutePath();
        if (seen.contains(parent))
            continue;
        seen.insert(parent);
        out.append(parent);
    }
    return out;
}

QStringList findUnityRoots(const QString& root)
{
    QStringList out;
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        if (!it.fileName().endsWith(QStringLiteral("_Data")))
            continue;
        const QString parent = QFileInfo(it.filePath()).absolutePath();
        if (!out.contains(parent))
            out.append(parent);
    }
    return out;
}

QStringList findSteamApiGameDirs(const QString& root)
{
    QStringList out;
    QDirIterator it(root,
                    {QStringLiteral("steam_api64.dll"), QStringLiteral("steam_api.dll")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString parent = it.fileInfo().absolutePath();
        QDir dir(parent);
        bool hasGame = false;
        for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.exe")}, QDir::Files)) {
            const QString lower = fi.fileName().toLower();
            if (lower == QStringLiteral("cmd.exe") || lower.contains(QStringLiteral("unins"))
                || lower.contains(QStringLiteral("crash")) || lower.contains(QStringLiteral("redist")))
                continue;
            hasGame = true;
            break;
        }
        if (!hasGame && !dir.entryList({QStringLiteral("*.pck")}, QDir::Files).isEmpty())
            hasGame = true;
        if (hasGame && !out.contains(parent))
            out.append(parent);
    }
    return out;
}

QStringList findOverlayTargets(const QString& installRoot)
{
    QStringList ue = findUeShippingDirs(installRoot);
    if (!ue.isEmpty()) {
        std::sort(ue.begin(), ue.end(), [&](const QString& a, const QString& b) {
            const int sa = (gameNeedsEpicFix(a) ? 2 : 0)
                           + (QDir(a).entryList({QStringLiteral("*Shipping*.exe")}, QDir::Files).isEmpty() ? 0 : 1);
            const int sb = (gameNeedsEpicFix(b) ? 2 : 0)
                           + (QDir(b).entryList({QStringLiteral("*Shipping*.exe")}, QDir::Files).isEmpty() ? 0 : 1);
            return sa > sb;
        });
        return {ue.first()};
    }
    const QStringList unity = findUnityRoots(installRoot);
    if (!unity.isEmpty())
        return {unity.first()};
    const QStringList steamApiDirs = findSteamApiGameDirs(installRoot);
    if (!steamApiDirs.isEmpty()) {
        QStringList ranked = steamApiDirs;
        std::sort(ranked.begin(), ranked.end(), [](const QString& a, const QString& b) {
            const int sa = QDir(a).entryList({QStringLiteral("*.pck")}, QDir::Files).isEmpty() ? 0 : 1;
            const int sb = QDir(b).entryList({QStringLiteral("*.pck")}, QDir::Files).isEmpty() ? 0 : 1;
            return sa > sb;
        });
        return {ranked.first()};
    }
    return {installRoot};
}

int ensureEpicBesideEos(const QString& installRoot, const QString& epicKit)
{
    const QString epicDll = epicKit + QStringLiteral("/EpicFix64.dll");
    if (!QFileInfo::exists(epicDll))
        return 0;
    int placed = 0;
    QDirIterator it(installRoot, {QStringLiteral("EOSSDK*.dll")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString dir = it.fileInfo().absolutePath();
        if (copyFileBestEffort(epicDll, dir + QStringLiteral("/EpicFix64.dll"))) {
            writeEpicFixIni(dir + QStringLiteral("/EpicFix.ini"));
            ++placed;
        }
    }
    return placed;
}

QString pickOverlayExe(const QString& target)
{
    QDir dir(target);
    QString best;
    qint64 bestScore = -1;
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.exe")}, QDir::Files)) {
        const QString lower = fi.fileName().toLower();
        if (lower == QStringLiteral("cmd.exe") || lower.contains(QStringLiteral("crash"))
            || lower.contains(QStringLiteral("unins")) || lower.contains(QStringLiteral("redist")))
            continue;
        qint64 score = fi.size();
        if (lower.contains(QStringLiteral("launcher")) || lower.contains(QStringLiteral("bootstrap")))
            score -= 50LL * 1024 * 1024;
        if (lower.contains(QStringLiteral("shipping")) || lower.contains(QStringLiteral("win64")))
            score += 100LL * 1024 * 1024;
        if (QFileInfo::exists(target + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("_Data")))
            score += 200LL * 1024 * 1024;
        if (score > bestScore) {
            bestScore = score;
            best = fi.absoluteFilePath();
        }
    }
    return best;
}

} // namespace

QString findLaunchExeForInstall(const QString& installPath)
{
    const QStringList targets = findOverlayTargets(installPath);
    if (!targets.isEmpty()) {
        const QString exe = pickOverlayExe(targets.first());
        if (!exe.isEmpty())
            return exe;
    }
    QString best;
    qint64 bestScore = -1;
    QDirIterator it(installPath, {QStringLiteral("*.exe")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (skipOverlayPath(it.filePath()))
            continue;
        const QString lower = it.fileName().toLower();
        if (lower.contains(QStringLiteral("crash")) || lower.contains(QStringLiteral("unins"))
            || lower.contains(QStringLiteral("redist")) || lower.contains(QStringLiteral("prereq")))
            continue;
        qint64 score = it.fileInfo().size();
        const bool hasOverlay = QFileInfo::exists(it.fileInfo().absolutePath() + QStringLiteral("/SteamFix64.dll"))
                                || QFileInfo::exists(it.fileInfo().absolutePath() + QStringLiteral("/winmm.dll"))
                                || QFileInfo::exists(it.fileInfo().absolutePath() + QStringLiteral("/OnlineFix64.dll"))
                                || QFileInfo::exists(it.fileInfo().absolutePath() + QStringLiteral("/OnlineFix.dll"));
        if (hasOverlay)
            score += 500LL * 1024 * 1024;
        if (lower.contains(QStringLiteral("shipping")))
            score += 200LL * 1024 * 1024;
        if (score > bestScore) {
            bestScore = score;
            best = it.filePath();
        }
    }
    return best;
}

OnlineFixResult embedOnlineFix(const QString& installPath, const QString& appid,
                               const QString& pluginRoot, const QJsonObject& settingsJson, bool force)
{
    OnlineFixResult r;
    const bool autoEmbed = settingsJson.contains(QStringLiteral("autoEmbedOnlineFix"))
                               ? settingsJson.value(QStringLiteral("autoEmbedOnlineFix")).toBool(true)
                               : true;
    if (!autoEmbed && !force) {
        r.skipped = true;
        r.reason = QStringLiteral("disabled");
        return r;
    }
    if (!QDir(installPath).exists()) {
        r.ok = false;
        r.error = QStringLiteral("install path missing");
        return r;
    }

    const QString kit = kitDir(pluginRoot, settingsJson);
    const KitFlavor flavor = detectKitFlavor(kit);
    if (flavor == KitFlavor::None) {
        r.ok = false;
        r.error = QStringLiteral(
            "Online-Fix kit missing or quarantined (need SteamFix64+winmm or OnlineFix64). "
            "Restore the plugin kit or add a Windows Security exclusion for the Arachnel folder.");
        return r;
    }

    // Always embed when steam_api present or force — store tags often miss depot installs.
    bool needs = force || gameHasSteamApi(installPath);
    if (!needs)
        needs = true; // depot installs without license still need FakeAppId overlay

    const QStringList targets = findOverlayTargets(installPath);
    if (targets.isEmpty()) {
        r.ok = false;
        r.error = QStringLiteral("no overlay target directories found");
        return r;
    }

    const bool needEpic = gameNeedsEpicFix(installPath) || gameNeedsEpicFix(targets.first());
    const QString epicKit = resolveEpicFixDir(kit, pluginRoot);
    if (needEpic && epicKit.isEmpty()) {
        r.ok = false;
        r.error = QStringLiteral(
            "Game uses RedpointEOS but EpicFix64.dll is not in online_fix_kit "
            "(expected FreeTP windows/EpicFix64.dll beside the host kit)");
        return r;
    }

    bool usedEpic = false;
    X86KitResolved x86SteamKit;
    X86OnlineFixResolved x86OnlineKit;
    if (flavor == KitFlavor::SteamFix)
        x86SteamKit = resolveX86SteamFixKit(kit);
    else if (flavor == KitFlavor::OnlineFix)
        x86OnlineKit = resolveX86OnlineFixKit(kit);

    for (const QString& target : targets) {
        QString launchExe = findLaunchExeForInstall(target.isEmpty() ? installPath : target);
        if (launchExe.isEmpty())
            launchExe = findLaunchExeForInstall(installPath);
        const quint16 machine = peMachine(launchExe);
        const bool want32 = machine == kPeI386;
        const bool useEpic = needEpic && !want32 && !epicKit.isEmpty();

        if (flavor == KitFlavor::OnlineFix) {
            const QString fakeAppId = QStringLiteral("480");
            writeOnlineFixIni(target + QStringLiteral("/OnlineFix.ini"), appid, fakeAppId);
            QFile::remove(target + QStringLiteral("/SteamFix.ini"));

            stripWrongArchOverlay(target, want32);
            ensureClassicSteamApi(target);

            QStringList dllList;
            if (want32) {
                // Galaxy Burger / Unity x86 OF.me: winmm loads OnlineFix.dll (+ SteamOverlay32).
                if (!x86OnlineKit.ok) {
                    r.ok = false;
                    r.error = QStringLiteral(
                        "32-bit Online-Fix kit missing (x86_optional.zip with OnlineFix.dll + "
                        "winmm32 + SteamOverlay32). Kit may be quarantined.");
                    return r;
                }
                const QString fixName = QFileInfo(x86OnlineKit.onlineFix).fileName();
                copyFileBestEffort(x86OnlineKit.onlineFix, target + QLatin1Char('/') + fixName);
                copyFileBestEffort(x86OnlineKit.winmm32, target + QStringLiteral("/winmm.dll"));
                if (!x86OnlineKit.overlay32.isEmpty())
                    copyFileBestEffort(x86OnlineKit.overlay32,
                                       target + QStringLiteral("/SteamOverlay32.dll"));
                if (!x86OnlineKit.stub32.isEmpty())
                    copyFileBestEffort(x86OnlineKit.stub32,
                                       target + QStringLiteral("/StubDRM32.dll"));
                for (const QString& n : {QStringLiteral("SteamOverlay32.dll"), fixName,
                                         QStringLiteral("StubDRM32.dll")}) {
                    if (QFileInfo::exists(target + QLatin1Char('/') + n))
                        dllList.append(n);
                }
                writeDllListTxt(target + QStringLiteral("/dlllist.txt"), dllList);
            } else {
                if (!copyKitFileIfExists(kit, QStringLiteral("OnlineFix64.dll"), target)
                    && !copyKitFileIfExists(kit, QStringLiteral("OnlineFix.dll"), target)) {
                    r.ok = false;
                    r.error = QStringLiteral(
                        "Online-Fix kit missing OnlineFix64.dll (kit missing/quarantined?)");
                    return r;
                }
                copyKitFileIfExists(kit, QStringLiteral("SteamOverlay64.dll"), target);
                copyKitFileIfExists(kit, QStringLiteral("StubDRM64.dll"), target);
                for (const QString& n : {QStringLiteral("OnlineFix64.dll"),
                                         QStringLiteral("OnlineFix.dll"),
                                         QStringLiteral("SteamOverlay64.dll"),
                                         QStringLiteral("StubDRM64.dll")}) {
                    if (QFileInfo::exists(target + QLatin1Char('/') + n))
                        dllList.append(n);
                }
                if (useEpic) {
                    copyFileBestEffort(epicKit + QStringLiteral("/EpicFix64.dll"),
                                       target + QStringLiteral("/EpicFix64.dll"));
                    writeEpicFixIni(target + QStringLiteral("/EpicFix.ini"));
                    if (QFileInfo::exists(target + QStringLiteral("/EpicFix64.dll")))
                        dllList.append(QStringLiteral("EpicFix64.dll"));
                    ensureEpicBesideEos(installPath, epicKit);
                    usedEpic = true;
                }
                writeDllListTxt(target + QStringLiteral("/dlllist.txt"), dllList);
            }

            writeSteamAppIdFile(target, fakeAppId);
            r.targets.append(target);
            continue;
        }

        // FreeTP SteamFix layout (Windows kit).
        writeSteamFixIni(target + QStringLiteral("/SteamFix.ini"), appid, false);
        if (!launchExe.isEmpty())
            tryUnpackSteamStub(launchExe, kit);

        stripWrongArchOverlay(target, want32);
        ensureClassicSteamApi(target);

        if (want32) {
            if (x86SteamKit.ok) {
                copyFileBestEffort(x86SteamKit.steamFix32, target + QStringLiteral("/SteamFix32.dll"));
                copyFileBestEffort(x86SteamKit.winmm32, target + QStringLiteral("/winmm.dll"));
                writeWinmmTxt(target + QStringLiteral("/winmm.txt"), false, true);
            } else {
                QFile::remove(target + QStringLiteral("/winmm.dll"));
                QFile::remove(target + QStringLiteral("/winmm.txt"));
                r.ok = false;
                r.error = QStringLiteral(
                    "32-bit Online-Fix kit missing (x86_optional.zip / SteamFix32). "
                    "It may have been quarantined by Windows Security.");
                return r;
            }
        } else {
            copyFileBestEffort(kit + QStringLiteral("/SteamFix64.dll"),
                               target + QStringLiteral("/SteamFix64.dll"));
            copyFileBestEffort(kit + QStringLiteral("/winmm.dll"), target + QStringLiteral("/winmm.dll"));
            writeWinmmTxt(target + QStringLiteral("/winmm.txt"), useEpic, false);
        }
        writeSteamAppIdFile(target);

        if (useEpic) {
            copyFileBestEffort(epicKit + QStringLiteral("/EpicFix64.dll"),
                               target + QStringLiteral("/EpicFix64.dll"));
            writeEpicFixIni(target + QStringLiteral("/EpicFix.ini"));
            ensureEpicBesideEos(installPath, epicKit);
            usedEpic = true;
        }
        ensureFreetpAckFolder(target, kit);
        r.targets.append(target);
    }

    r.embedded = !r.targets.isEmpty();
    r.epic = usedEpic;
    const QString markerPath = installPath + QStringLiteral("/.arachnel-steamidra");
    QJsonObject payload;
    QFile marker(markerPath);
    if (marker.exists() && marker.open(QIODevice::ReadOnly)) {
        payload = QJsonDocument::fromJson(marker.readAll()).object();
        marker.close();
    }
    const QString launchExe = findLaunchExeForInstall(installPath);
    if (!launchExe.isEmpty()) {
        QJsonObject launch;
        launch.insert(QStringLiteral("type"), QStringLiteral("exe"));
        launch.insert(QStringLiteral("path"), launchExe);
        payload.insert(QStringLiteral("launch"), launch);
    }
    QJsonObject of;
    of.insert(QStringLiteral("embedded"), true);
    of.insert(QStringLiteral("epic"), usedEpic);
    of.insert(QStringLiteral("flavor"),
              flavor == KitFlavor::OnlineFix ? QStringLiteral("onlinefix-me")
                                             : QStringLiteral("steamfix"));
    of.insert(QStringLiteral("targets"), QJsonArray::fromStringList(r.targets));
    payload.insert(QStringLiteral("onlineFix"), of);
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
        marker.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    return r;
}

QString findLaunchExeHeuristic(const QString& installPath)
{
    return findLaunchExeForInstall(installPath);
}

void repairOnlineFixBeforeLaunch(const QString& installPath, const QString& appid,
                                 const QString& pluginRoot)
{
    if (installPath.isEmpty() || !QDir(installPath).exists())
        return;

    if (isElectronInstall(installPath)) {
        writeSteamAppIdFile(installPath);
        for (const QString& target : findOverlayTargets(installPath))
            stripSteamFixOverlay(target);
        stripSteamFixOverlay(installPath);
        seedSteamFixRegistry(appid);
        return;
    }

    seedSteamFixRegistry(appid);
    const QString kit = kitDir(pluginRoot, {});
    for (const QString& target : findOverlayTargets(installPath)) {
        if (!QFileInfo::exists(target + QStringLiteral("/SteamFix.ini"))
            && !QFileInfo::exists(target + QStringLiteral("/SteamFix64.dll"))
            && !QFileInfo::exists(target + QStringLiteral("/OnlineFix.ini"))
            && !QFileInfo::exists(target + QStringLiteral("/OnlineFix64.dll"))
            && !QFileInfo::exists(target + QStringLiteral("/OnlineFix.dll"))
            && !QFileInfo::exists(target + QStringLiteral("/winmm.dll")))
            continue;
        ensureFreetpAckFolder(target, kit);
        if (!QFileInfo::exists(target + QStringLiteral("/steam_appid.txt")))
            writeSteamAppIdFile(target);
    }
}

bool applyDlcUnlocks(const QString& installPath, const QString& appid,
                     const QStringList& enabledDlcAppIds)
{
    if (installPath.isEmpty() || !QDir(installPath).exists() || appid.isEmpty())
        return false;

    const bool unlockAll = !enabledDlcAppIds.isEmpty();
    bool wrote = false;
    QStringList targets = findOverlayTargets(installPath);
    if (targets.isEmpty())
        targets.append(installPath);
    for (const QString& target : targets) {
        if (QFileInfo::exists(target + QStringLiteral("/OnlineFix.ini"))
            && !QFileInfo::exists(target + QStringLiteral("/SteamFix64.dll"))
            && !QFileInfo::exists(target + QStringLiteral("/SteamFix32.dll"))) {
            QFile::remove(target + QStringLiteral("/SteamFix.ini"));
            continue;
        }
        const QString ini = target + QStringLiteral("/SteamFix.ini");
        if (!QFileInfo::exists(ini)
            && !QFileInfo::exists(target + QStringLiteral("/SteamFix64.dll"))
            && !QFileInfo::exists(target + QStringLiteral("/SteamFix32.dll"))
            && !QFileInfo::exists(target + QStringLiteral("/winmm.dll")))
            continue;
        bool workshop = true;
        bool overlay = true;
        QFile existing(ini);
        if (existing.open(QIODevice::ReadOnly)) {
            const QByteArray body = existing.readAll();
            workshop = body.contains("Workshop=true") || !body.contains("Workshop=false");
            overlay = body.contains("Overlay=true");
            existing.close();
        }
        writeSteamFixIni(ini, appid, workshop, overlay, unlockAll);
        wrote = true;
    }

    const QString markerPath = installPath + QStringLiteral("/.arachnel-steamidra");
    QJsonObject payload;
    QFile marker(markerPath);
    if (marker.exists() && marker.open(QIODevice::ReadOnly)) {
        payload = QJsonDocument::fromJson(marker.readAll()).object();
        marker.close();
    }
    payload.insert(QStringLiteral("appid"), appid);
    QJsonArray selected;
    for (const QString& id : enabledDlcAppIds) {
        if (!id.isEmpty())
            selected.append(id);
    }
    payload.insert(QStringLiteral("selectedDlc"), selected);
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        marker.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
        wrote = true;
    }
    return wrote;
}

} // namespace steamidra
