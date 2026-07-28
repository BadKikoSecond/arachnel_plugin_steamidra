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
#include <QtGlobal>

#include <algorithm>

namespace steamidra {
namespace {

QString kitDir(const QString& pluginRoot, const QJsonObject& settings)
{
    const QString configured = settings.value(QStringLiteral("onlineFixSourceDir")).toString().trimmed();
    if (!configured.isEmpty() && QFileInfo::exists(configured + QStringLiteral("/SteamFix64.dll")))
        return configured;
    const QString bundled = pluginRoot + QStringLiteral("/online_fix_kit");
    if (QFileInfo::exists(bundled + QStringLiteral("/SteamFix64.dll"))
        && QFileInfo::exists(bundled + QStringLiteral("/winmm.dll")))
        return bundled;
    return bundled;
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
    return {};
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

void ensureClassicSteamApi(const QString& target)
{
    // Old titles (Isaac etc.) need classic SteamAPI_Init / SteamUser exports.
    // Modern cracked steam_api (OF/Goldberg flat API) is ~260KB+ and lacks those
    // names → STATUS_ENTRYPOINT_NOT_FOUND. SteamFix hooks the Valve DLL instead.
    const QString api = target + QStringLiteral("/steam_api.dll");
    if (!QFileInfo::exists(api))
        return;
    if (peHasExportName(api, "SteamAPI_Init"))
        return;
    const QStringList backups = {
        api + QStringLiteral(".valve-bak"),
        api + QStringLiteral(".arachnel-valve-bak"),
        target + QStringLiteral("/steam_api.dll.bak"),
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
        // Also drop x64 leftovers even if PE read fails (known FreeTP names).
        QFile::remove(target + QStringLiteral("/SteamFix64.dll"));
        QFile::remove(target + QStringLiteral("/EpicFix64.dll"));
        QFile::remove(target + QStringLiteral("/EpicFix.ini"));
    }
}

void writeSteamFixIni(const QString& path, const QString& appid, bool workshop)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    QByteArray t;
    t += "[Main]\r\n";
    t += "RealAppId=" + appid.toUtf8() + "\r\n";
    t += "FakeAppId=480\r\n#Language=english\r\nBuildId=0\r\n\r\n";
    t += "[Misc]\r\nOverlay=true\r\nFilterLobby=false\r\n\r\n";
    t += "[Interfaces]\r\nApps=true\r\nFriends=true\r\nUser=true\r\nInventory=true\r\n";
    t += "Stats=true\r\nStorage=true\r\nUtils=true\r\n";
    t += workshop ? "Workshop=true\r\n\r\n[DLC]\r\n0=dlc\r\n" : "Workshop=false\r\n\r\n[DLC]\r\n";
    f.write(t);
    f.commit();
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
    // EpicFix64 only helps x64 EOS. Win32 EOSSDK (Isaac etc.) must not block embed
    // or receive an x64 EpicFix beside a 32-bit exe.
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
            continue;
        }
        if (name.startsWith(QStringLiteral("EOSSDK"), Qt::CaseInsensitive)
            && name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive)) {
            if (name.contains(QStringLiteral("Win32"), Qt::CaseInsensitive))
                continue;
            const quint16 m = peMachine(it.filePath());
            if (m == kPeI386)
                continue;
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
    return {installRoot};
}

int ensureEpicBesideEos(const QString& installRoot, const QString& kit)
{
    const QString epicDll = kit + QStringLiteral("/EpicFix64.dll");
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
                                || QFileInfo::exists(it.fileInfo().absolutePath() + QStringLiteral("/winmm.dll"));
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
    if (!QFileInfo::exists(kit + QStringLiteral("/SteamFix64.dll"))
        || !QFileInfo::exists(kit + QStringLiteral("/winmm.dll"))) {
        r.ok = false;
        r.error = QStringLiteral("Online-fix kit missing (SteamFix64.dll / winmm.dll)");
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
    if (needEpic && !QFileInfo::exists(kit + QStringLiteral("/EpicFix64.dll"))) {
        r.ok = false;
        r.error = QStringLiteral(
            "Game uses RedpointEOS/EOSSDK but EpicFix64.dll is not in online_fix_kit");
        return r;
    }

    bool usedEpic = false;
    for (const QString& target : targets) {
        writeSteamFixIni(target + QStringLiteral("/SteamFix.ini"), appid, false);

        QString launchExe = findLaunchExeForInstall(target.isEmpty() ? installPath : target);
        if (launchExe.isEmpty())
            launchExe = findLaunchExeForInstall(installPath);
        // SteamStub runs before winmm - unpack so FakeAppId/SteamFix can take over.
        if (!launchExe.isEmpty())
            tryUnpackSteamStub(launchExe, kit);
        const quint16 machine = peMachine(launchExe);
        const bool want32 = machine == kPeI386;
        const bool haveFix32 = QFileInfo::exists(kit + QStringLiteral("/SteamFix32.dll"));
        const QString kitWinmm32 = resolveKitWinmm32(kit);
        // x64 EpicFix only on x64 overlays (Win32 EOSSDK must not pull EpicFix64).
        const bool useEpic = needEpic && !want32;

        stripWrongArchOverlay(target, want32);
        ensureClassicSteamApi(target);

        if (want32) {
            // FreeTP-style x86: SteamFix32 + 32-bit winmm + winmm.txt + steam_appid.txt.
            // Never place x64 winmm beside a 32-bit exe (LoadLibrary ERROR_BAD_EXE_FORMAT).
            if (haveFix32 && !kitWinmm32.isEmpty()) {
                copyFileBestEffort(kit + QStringLiteral("/SteamFix32.dll"),
                                   target + QStringLiteral("/SteamFix32.dll"));
                copyFileBestEffort(kitWinmm32, target + QStringLiteral("/winmm.dll"));
                writeWinmmTxt(target + QStringLiteral("/winmm.txt"), false, true);
            } else if (haveFix32) {
                // SteamFix32 alone cannot load without an x86 winmm proxy.
                copyFileBestEffort(kit + QStringLiteral("/SteamFix32.dll"),
                                   target + QStringLiteral("/SteamFix32.dll"));
                QFile::remove(target + QStringLiteral("/winmm.dll"));
                QFile::remove(target + QStringLiteral("/winmm.txt"));
            } else {
                QFile::remove(target + QStringLiteral("/winmm.dll"));
                QFile::remove(target + QStringLiteral("/winmm.txt"));
            }
        } else {
            copyFileBestEffort(kit + QStringLiteral("/SteamFix64.dll"),
                               target + QStringLiteral("/SteamFix64.dll"));
            copyFileBestEffort(kit + QStringLiteral("/winmm.dll"), target + QStringLiteral("/winmm.dll"));
            if (haveFix32)
                copyFileBestEffort(kit + QStringLiteral("/SteamFix32.dll"),
                                   target + QStringLiteral("/SteamFix32.dll"));
            writeWinmmTxt(target + QStringLiteral("/winmm.txt"), useEpic, false);
        }
        // FakeAppId file - critical for Steamworks / classic steam_api when winmm hooks.
        writeSteamAppIdFile(target);

        if (useEpic) {
            copyFileBestEffort(kit + QStringLiteral("/EpicFix64.dll"),
                               target + QStringLiteral("/EpicFix64.dll"));
            writeEpicFixIni(target + QStringLiteral("/EpicFix.ini"));
            ensureEpicBesideEos(installPath, kit);
            usedEpic = true;
        }
        // FreeTP ack folder (best-effort)
        QDir().mkpath(target + QStringLiteral("/FreeTP/UserData"));
        QFile anon(target + QStringLiteral("/FreeTP/UserData/AnonFolderSave.txt"));
        if (!anon.exists() && anon.open(QIODevice::WriteOnly))
            anon.close();
        r.targets.append(target);
    }

    r.embedded = !r.targets.isEmpty();
    r.epic = usedEpic;
    // Update marker launch path
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
    of.insert(QStringLiteral("epic"), needEpic);
    of.insert(QStringLiteral("targets"), QJsonArray::fromStringList(r.targets));
    payload.insert(QStringLiteral("onlineFix"), of);
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
        marker.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    return r;
}

} // namespace steamidra
