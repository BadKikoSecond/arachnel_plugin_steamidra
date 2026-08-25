#include "steam_launch.h"

#include "http_client.h"
#include "online_fix_embed.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace steamidra {
namespace {

bool isBannedLaunchFileName(const QString& fileName)
{
    const QString lower = fileName.toLower();
    if (lower == QStringLiteral("cmd.exe") || lower == QStringLiteral("cmd_stub.exe")
        || lower == QStringLiteral("powershell.exe") || lower == QStringLiteral("conhost.exe"))
        return true;
    if (lower.contains(QStringLiteral("crash")) || lower.contains(QStringLiteral("unins"))
        || lower.contains(QStringLiteral("setup")) || lower.contains(QStringLiteral("redist"))
        || lower.contains(QStringLiteral("prereq")) || lower.contains(QStringLiteral("vcredist"))
        || lower.contains(QStringLiteral("dxsetup")) || lower.contains(QStringLiteral("oalinst")))
        return true;
    if (lower == QStringLiteral("ffmpeg.exe") || lower == QStringLiteral("ffprobe.exe")
        || lower == QStringLiteral("ffplay.exe") || lower == QStringLiteral("yt-dlp.exe")
        || lower == QStringLiteral("youtube-dl.exe") || lower == QStringLiteral("texconv.exe")
        || lower.startsWith(QStringLiteral("easyhook")) || lower.contains(QStringLiteral("easyhook"))
        || lower == QStringLiteral("7z.exe") || lower == QStringLiteral("7za.exe")
        || lower == QStringLiteral("curl.exe") || lower == QStringLiteral("wget.exe")
        || lower == QStringLiteral("quickhash.exe") || lower == QStringLiteral("openvr_api.exe")
        || lower == QStringLiteral("steam.exe") || lower.startsWith(QStringLiteral("steamless")))
        return true;
    return false;
}

QString normalizeRelPath(QString rel)
{
    rel = rel.trimmed();
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (rel.startsWith(QLatin1Char('/')))
        rel.remove(0, 1);
    return rel;
}

QString resolveUnderInstall(const QString& installPath, const QString& relative)
{
    const QString rel = normalizeRelPath(relative);
    if (rel.isEmpty() || installPath.isEmpty())
        return {};

    const QString direct = QFileInfo(QDir(installPath).filePath(rel)).absoluteFilePath();
    if (QFileInfo::exists(direct) && QFileInfo(direct).isFile())
        return direct;

    const QString needle = QFileInfo(rel).fileName().toLower();
    if (needle.isEmpty())
        return {};

    const QString wantTail = QDir::cleanPath(rel).toLower();
    QDirIterator it(installPath, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileName().toLower() != needle)
            continue;
        const QString abs = it.fileInfo().absoluteFilePath();
        const QString foundRel =
            QDir::cleanPath(QDir(installPath).relativeFilePath(abs)).replace(QLatin1Char('\\'),
                                                                            QLatin1Char('/'))
                .toLower();
        if (foundRel == wantTail || foundRel.endsWith(QLatin1Char('/') + wantTail))
            return abs;
    }
    // Last resort: unique basename match.
    QString only;
    int hits = 0;
    QDirIterator it2(installPath, QDir::Files, QDirIterator::Subdirectories);
    while (it2.hasNext()) {
        it2.next();
        if (it2.fileName().toLower() != needle)
            continue;
        ++hits;
        only = it2.fileInfo().absoluteFilePath();
        if (hits > 1)
            return {};
    }
    return hits == 1 ? only : QString();
}

QJsonObject fetchSteamcmdApp(const QString& appid)
{
    const QString id = appid.trimmed();
    if (id.isEmpty())
        return {};
    QString err;
    const QByteArray raw = httpGetJsonBytes(
        QStringLiteral("https://api.steamcmd.net/v1/info/%1").arg(id), 45000, {}, &err);
    if (raw.isEmpty())
        return {};
    return QJsonDocument::fromJson(raw)
        .object()
        .value(QStringLiteral("data"))
        .toObject()
        .value(id)
        .toObject();
}

int scoreSteamLaunchEntry(const QJsonObject& entry)
{
    const QString executable = entry.value(QStringLiteral("executable")).toString().trimmed();
    if (executable.isEmpty() || isBannedLaunchFileName(QFileInfo(executable).fileName()))
        return -1;

    const QJsonObject cfg = entry.value(QStringLiteral("config")).toObject();
    const QString oslist = cfg.value(QStringLiteral("oslist")).toString().toLower();
    // Windows PE only (native or Proton).
    if (!oslist.isEmpty() && !oslist.contains(QStringLiteral("windows")))
        return -1;

    int score = 10;
    if (oslist.isEmpty() || oslist.contains(QStringLiteral("windows")))
        score += 100;

    const QString type = entry.value(QStringLiteral("type")).toString().trimmed().toLower();
    if (type.isEmpty() || type == QStringLiteral("default"))
        score += 50;
    else if (type.contains(QStringLiteral("option")) || type.contains(QStringLiteral("config")))
        score -= 20;

    const QString exeLower = executable.toLower();
    if (exeLower.endsWith(QStringLiteral(".exe")))
        score += 30;
    if (exeLower.contains(QStringLiteral("launcher")))
        score -= 25;
    return score;
}

ResolvedLaunchExe launchFromSteamApp(const QString& installPath, const QJsonObject& app)
{
    ResolvedLaunchExe out;
    const QJsonObject launchMap =
        app.value(QStringLiteral("config")).toObject().value(QStringLiteral("launch")).toObject();
    if (launchMap.isEmpty())
        return out;

    int bestScore = -1;
    QJsonObject best;
    for (auto it = launchMap.begin(); it != launchMap.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject entry = it.value().toObject();
        const int score = scoreSteamLaunchEntry(entry);
        if (score > bestScore) {
            bestScore = score;
            best = entry;
        }
    }
    if (bestScore < 0)
        return out;

    const QString relExe = best.value(QStringLiteral("executable")).toString();
    const QString relWork = best.value(QStringLiteral("workingdir")).toString();

    QString absExe;
    if (!relWork.trimmed().isEmpty()) {
        absExe = resolveUnderInstall(installPath,
                                     normalizeRelPath(relWork) + QLatin1Char('/')
                                         + normalizeRelPath(relExe));
    }
    if (absExe.isEmpty())
        absExe = resolveUnderInstall(installPath, relExe);
    if (absExe.isEmpty())
        return out;

    out.path = absExe;
    out.source = QStringLiteral("steam");
    if (!relWork.trimmed().isEmpty()) {
        const QString workAbs =
            QFileInfo(QDir(installPath).filePath(normalizeRelPath(relWork))).absoluteFilePath();
        if (QDir(workAbs).exists())
            out.workingDirectory = workAbs;
    }
    if (out.workingDirectory.isEmpty())
        out.workingDirectory = QFileInfo(absExe).absolutePath();

    const QJsonValue argsVal = best.value(QStringLiteral("arguments"));
    if (argsVal.isArray()) {
        for (const QJsonValue& v : argsVal.toArray()) {
            const QString a = v.toString().trimmed();
            if (!a.isEmpty())
                out.arguments.append(a);
        }
    } else {
        const QString args = argsVal.toString().trimmed();
        if (!args.isEmpty()) {
            out.arguments =
                args.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        }
    }
    return out;
}

} // namespace

ResolvedLaunchExe resolveInstallLaunch(const QString& installPath, const QString& appid)
{
    ResolvedLaunchExe out;
    if (installPath.isEmpty() || !QDir(installPath).exists())
        return out;

    if (!appid.trimmed().isEmpty()) {
        out = launchFromSteamApp(installPath, fetchSteamcmdApp(appid));
        if (!out.path.isEmpty())
            return out;
    }

    const QString path = findLaunchExeHeuristic(installPath);
    if (path.isEmpty() || isBannedLaunchFileName(QFileInfo(path).fileName()))
        return {};
    out.path = path;
    out.workingDirectory = QFileInfo(path).absolutePath();
    out.source = QStringLiteral("heuristic");
    return out;
}

QVector<SteamLaunchOption> collectLaunchOptions(const QString& installPath, const QString& appid)
{
    if (installPath.isEmpty() || !QDir(installPath).exists())
        return {};

    if (!appid.trimmed().isEmpty()) {
        const QJsonObject app = fetchSteamcmdApp(appid);
        const QJsonObject launchMap =
            app.value(QStringLiteral("config")).toObject().value(QStringLiteral("launch")).toObject();
        if (!launchMap.isEmpty()) {
            QVector<SteamLaunchOption> options;
            int bestScore = -1;
            int bestIndex = -1;

            for (auto it = launchMap.begin(); it != launchMap.end(); ++it) {
                if (!it.value().isObject())
                    continue;
                const QString optId = it.key();
                const QJsonObject entry = it.value().toObject();
                const int score = scoreSteamLaunchEntry(entry);
                if (score < 0)
                    continue;

                const QString relExe = entry.value(QStringLiteral("executable")).toString();
                const QString relWork = entry.value(QStringLiteral("workingdir")).toString();

                QString absExe;
                if (!relWork.trimmed().isEmpty()) {
                    absExe = resolveUnderInstall(installPath,
                                                 normalizeRelPath(relWork) + QLatin1Char('/')
                                                     + normalizeRelPath(relExe));
                }
                if (absExe.isEmpty())
                    absExe = resolveUnderInstall(installPath, relExe);
                if (absExe.isEmpty() || !QFileInfo::exists(absExe))
                    continue;

                SteamLaunchOption opt;
                opt.id = optId;
                opt.path = absExe;
                opt.type = entry.value(QStringLiteral("type")).toString();

                if (!relWork.trimmed().isEmpty()) {
                    const QString workAbs =
                        QFileInfo(QDir(installPath).filePath(normalizeRelPath(relWork))).absoluteFilePath();
                    if (QDir(workAbs).exists())
                        opt.workingDirectory = workAbs;
                }
                if (opt.workingDirectory.isEmpty())
                    opt.workingDirectory = QFileInfo(absExe).absolutePath();

                const QJsonValue argsVal = entry.value(QStringLiteral("arguments"));
                if (argsVal.isArray()) {
                    for (const QJsonValue& v : argsVal.toArray()) {
                        const QString a = v.toString().trimmed();
                        if (!a.isEmpty())
                            opt.arguments.append(a);
                    }
                } else {
                    const QString args = argsVal.toString().trimmed();
                    if (!args.isEmpty()) {
                        opt.arguments =
                            args.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                    }
                }

                // Title / Description
                QString title;
                const QJsonObject loc = entry.value(QStringLiteral("description_loc")).toObject();
                if (loc.contains(QStringLiteral("russian")))
                    title = loc.value(QStringLiteral("russian")).toString().trimmed();
                if (title.isEmpty() && loc.contains(QStringLiteral("english")))
                    title = loc.value(QStringLiteral("english")).toString().trimmed();
                if (title.isEmpty())
                    title = entry.value(QStringLiteral("description")).toString().trimmed();
                if (title.isEmpty() || title.compare(QStringLiteral("launch"), Qt::CaseInsensitive) == 0
                    || title.compare(QStringLiteral("play"), Qt::CaseInsensitive) == 0) {
                    const QString stem = QFileInfo(absExe).completeBaseName();
                    if (!opt.arguments.isEmpty())
                        title = QStringLiteral("%1 (%2)").arg(stem, opt.arguments.join(QLatin1Char(' ')));
                    else if (!opt.type.isEmpty() && opt.type != QStringLiteral("default") && opt.type != QStringLiteral("none"))
                        title = QStringLiteral("%1 (%2)").arg(stem, opt.type.toUpper());
                    else
                        title = stem;
                }
                opt.title = title;

                bool duplicate = false;
                for (const auto& existing : options) {
                    if (existing.path.compare(opt.path, Qt::CaseInsensitive) == 0
                        && existing.arguments == opt.arguments) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                    continue;

                if (score > bestScore) {
                    bestScore = score;
                    bestIndex = options.size();
                }
                options.append(opt);
            }

            if (bestIndex >= 0 && bestIndex < options.size())
                options[bestIndex].isDefault = true;
            else if (!options.isEmpty())
                options[0].isDefault = true;

            if (!options.isEmpty())
                return options;
        }
    }

    const QString path = findLaunchExeHeuristic(installPath);
    if (!path.isEmpty() && !isBannedLaunchFileName(QFileInfo(path).fileName())) {
        SteamLaunchOption opt;
        opt.id = QStringLiteral("default");
        opt.title = QFileInfo(path).completeBaseName();
        opt.path = path;
        opt.workingDirectory = QFileInfo(path).absolutePath();
        opt.isDefault = true;
        return {opt};
    }
    return {};
}

QString findLaunchExeForInstall(const QString& installPath, const QString& appid)
{
    return resolveInstallLaunch(installPath, appid).path;
}

} // namespace steamidra
