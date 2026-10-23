#include "exporter.h"

#include "notestree.h"

#include "vault.h"
#include "citations.h"
#include "zettel.h"

#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace {
const QString exportSubfolder = QStringLiteral(".omazettel/export");
const QString templatesSubfolder = QStringLiteral(".omazettel/templates");
const QString lastProfileSetting = QStringLiteral("export/lastProfile");
const QStringList textTemplateSuffixes{QStringLiteral("tex"), QStringLiteral("latex"),
                                       QStringLiteral("html"), QStringLiteral("htm"),
                                       QStringLiteral("typ"), QStringLiteral("txt"),
                                       QStringLiteral("md"), QStringLiteral("css"),
                                       QStringLiteral("csl"), QStringLiteral("lua")};
const QStringList referenceSuffixes{QStringLiteral("docx"), QStringLiteral("odt"),
                                    QStringLiteral("pptx")};

QString readText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

bool writeText(const QString &path, const QString &text) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    file.write(text.toUtf8());
    return file.commit();
}

QString pandocPath() {
    return QStandardPaths::findExecutable(QStringLiteral("pandoc"));
}
}

Exporter::Exporter(QObject *parent) : QObject(parent) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(180000);
    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        if (m_process)
            m_process->kill();
    });
}

Exporter::~Exporter() {
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

bool Exporter::pandocAvailable() const {
    return !pandocPath().isEmpty();
}

QString Exporter::profilesFolder() const {
    return m_root.isEmpty() ? QString() : QDir(m_root).filePath(exportSubfolder);
}

QString Exporter::templatesFolder() const {
    return m_root.isEmpty() ? QString() : QDir(m_root).filePath(templatesSubfolder);
}

QString Exporter::lastProfile() const {
    return QSettings().value(lastProfileSetting).toString();
}

void Exporter::setLastProfile(const QString &path) {
    if (path == lastProfile())
        return;
    QSettings().setValue(lastProfileSetting, path);
    reload();
}

QString Exporter::exportLocation() const {
    return QSettings().value(QStringLiteral("export/location"), QStringLiteral("beside")).toString();
}

QString Exporter::exportsFolder() const {
    return m_root.isEmpty() ? QString() : QDir(m_root).filePath(QStringLiteral("Exports"));
}

void Exporter::setExportLocation(const QString &location) {
    const QString value = location == QLatin1String("folder") ? location : QStringLiteral("beside");
    if (value == exportLocation())
        return;
    QSettings().setValue(QStringLiteral("export/location"), value);
    // Exports are made from the notes; keep them out of git sync.
    if (value == QLatin1String("folder") && !m_root.isEmpty()) {
        const QString ignore = QDir(m_root).filePath(QStringLiteral(".gitignore"));
        QString text = readText(ignore);
        if (!text.split(QLatin1Char('\n')).contains(QStringLiteral("/Exports/"))) {
            if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
                text += QLatin1Char('\n');
            writeText(ignore, text + QStringLiteral("# OmaZettel exports\n/Exports/\n"));
        }
    }
    reload();
}

void Exporter::setRoot(const QString &notesRoot) {
    if (m_root == notesRoot)
        return;
    m_root = notesRoot;
    reload();
}

// XeLaTeX handles any Unicode text and system fonts, so it is the first
// choice, but only when its format file is built (texlive-xetex installed).
QString Exporter::detectPdfEngine() {
    const auto works = [](const QString &engine, const QString &format, const QString &kpseEngine) {
        if (QStandardPaths::findExecutable(engine).isEmpty())
            return false;
        const QString kpsewhich = QStandardPaths::findExecutable(QStringLiteral("kpsewhich"));
        if (kpsewhich.isEmpty())
            return true;
        QProcess probe;
        probe.start(kpsewhich, {QStringLiteral("-engine=") + kpseEngine, format});
        return probe.waitForFinished(5000) && !probe.readAllStandardOutput().trimmed().isEmpty();
    };
    if (works(QStringLiteral("xelatex"), QStringLiteral("xelatex.fmt"), QStringLiteral("xetex")))
        return QStringLiteral("xelatex");
    if (works(QStringLiteral("pdflatex"), QStringLiteral("pdflatex.fmt"), QStringLiteral("pdftex")))
        return QStringLiteral("pdflatex");
    if (!QStandardPaths::findExecutable(QStringLiteral("typst")).isEmpty())
        return QStringLiteral("typst");
    if (!QStandardPaths::findExecutable(QStringLiteral("weasyprint")).isEmpty())
        return QStringLiteral("weasyprint");
    return QString();
}

QList<QPair<QString, QString>> Exporter::starterProfiles(const QString &pdfEngine) {
    const QString header = QStringLiteral(
        "# An OmaZettel export profile: a pandoc defaults file.\n"
        "# Any pandoc option works here: https://pandoc.org/MANUAL.html#defaults-files\n"
        "# ${.} is this folder, so ${.}/../templates/<file> is a file in the templates folder.\n"
        "# A note's own YAML header (title, author, date, ...) fills the template.\n");
    QString pdf = header
        + QStringLiteral("\nto: pdf\n");
    if (pdfEngine.isEmpty()) {
        pdf += QStringLiteral(
            "# No PDF engine was found. Install one, then set it here, for example:\n"
            "#   sudo pacman -S texlive-xetex    then    pdf-engine: xelatex\n"
            "pdf-engine: xelatex\n");
    } else {
        pdf += QStringLiteral("pdf-engine: %1\n").arg(pdfEngine);
        if (pdfEngine == QLatin1String("pdflatex"))
            pdf += QStringLiteral(
                "# pdflatex cannot use system fonts or every Unicode character. For those,\n"
                "# install texlive-xetex and use pdf-engine: xelatex.\n");
    }
    pdf += QStringLiteral(
        "# template: ${.}/../templates/Article.tex\n"
        "variables:\n"
        "  geometry: margin=2.5cm\n"
        "  colorlinks: true\n");

    return {
        {QStringLiteral("PDF"), pdf},
        {QStringLiteral("Word"), header + QStringLiteral(
             "\nto: docx\n"
             "# Styles come from a reference document; make one under Templates.\n"
             "# reference-doc: ${.}/../templates/Reference.docx\n")},
        {QStringLiteral("OpenDocument"), header + QStringLiteral(
             "\nto: odt\n"
             "# reference-doc: ${.}/../templates/Reference.odt\n")},
        {QStringLiteral("HTML"), header + QStringLiteral(
             "\nto: html5\n"
             "standalone: true\n"
             "embed-resources: true\n"
             "# template: ${.}/../templates/Page.html\n")},
        {QStringLiteral("LaTeX source"), header + QStringLiteral(
             "\nto: latex\n"
             "standalone: true\n"
             "# template: ${.}/../templates/Article.tex\n")},
    };
}

void Exporter::ensureDefaults() {
    const QString profiles = profilesFolder();
    if (profiles.isEmpty() || QFileInfo(profiles).exists())
        return;
    QDir().mkpath(profiles);
    QDir().mkpath(templatesFolder());
    for (const auto &[name, text] : starterProfiles(detectPdfEngine()))
        writeText(QDir(profiles).filePath(name + QStringLiteral(".yaml")), text);
    reload();
}

QString Exporter::formatOf(const QString &profileText) {
    static const QRegularExpression toRe(QStringLiteral("^(?:to|writer)\\s*:\\s*[\"']?([\\w+\\-]+)"),
                                         QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = toRe.match(profileText);
    // pandoc's own default output is HTML.
    return match.hasMatch() ? match.captured(1).section(QLatin1Char('+'), 0, 0).toLower()
                            : QStringLiteral("html");
}

QString Exporter::extensionFor(const QString &format) {
    static const QHash<QString, QString> extensions{
        {QStringLiteral("pdf"), QStringLiteral("pdf")},
        {QStringLiteral("docx"), QStringLiteral("docx")},
        {QStringLiteral("odt"), QStringLiteral("odt")},
        {QStringLiteral("pptx"), QStringLiteral("pptx")},
        {QStringLiteral("html"), QStringLiteral("html")},
        {QStringLiteral("html5"), QStringLiteral("html")},
        {QStringLiteral("html4"), QStringLiteral("html")},
        {QStringLiteral("latex"), QStringLiteral("tex")},
        {QStringLiteral("beamer"), QStringLiteral("tex")},
        {QStringLiteral("context"), QStringLiteral("tex")},
        {QStringLiteral("typst"), QStringLiteral("typ")},
        {QStringLiteral("epub"), QStringLiteral("epub")},
        {QStringLiteral("epub3"), QStringLiteral("epub")},
        {QStringLiteral("rtf"), QStringLiteral("rtf")},
        {QStringLiteral("plain"), QStringLiteral("txt")},
        {QStringLiteral("markdown"), QStringLiteral("md")},
        {QStringLiteral("gfm"), QStringLiteral("md")},
        {QStringLiteral("commonmark"), QStringLiteral("md")},
        {QStringLiteral("revealjs"), QStringLiteral("html")},
        {QStringLiteral("docbook"), QStringLiteral("xml")},
        {QStringLiteral("jats"), QStringLiteral("xml")},
        {QStringLiteral("asciidoc"), QStringLiteral("adoc")},
        {QStringLiteral("rst"), QStringLiteral("rst")},
        {QStringLiteral("man"), QStringLiteral("1")},
        {QStringLiteral("ms"), QStringLiteral("pdf")},
    };
    return extensions.value(format.toLower(), format.toLower());
}

QVariantList Exporter::profiles() const {
    QVariantList result;
    const QString folder = profilesFolder();
    if (folder.isEmpty())
        return result;
    const QFileInfoList files = QDir(folder).entryInfoList(
        {QStringLiteral("*.yaml"), QStringLiteral("*.yml")}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : files) {
        const QString format = formatOf(readText(info.absoluteFilePath()));
        result.append(QVariantMap{{QStringLiteral("name"), info.completeBaseName()},
                                  {QStringLiteral("path"), info.absoluteFilePath()},
                                  {QStringLiteral("format"), format},
                                  {QStringLiteral("extension"), extensionFor(format)}});
    }
    return result;
}

QVariantList Exporter::templates() const {
    QVariantList result;
    const QString folder = templatesFolder();
    if (folder.isEmpty())
        return result;
    const QFileInfoList files = QDir(folder).entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : files) {
        const QString suffix = info.suffix().toLower();
        result.append(QVariantMap{
            {QStringLiteral("name"), info.fileName()},
            {QStringLiteral("path"), info.absoluteFilePath()},
            {QStringLiteral("kind"), referenceSuffixes.contains(suffix) ? QStringLiteral("reference")
                                                                         : QStringLiteral("template")},
            {QStringLiteral("text"), textTemplateSuffixes.contains(suffix)}});
    }
    return result;
}

QString Exporter::newTemplate(const QString &kind, const QString &name) {
    m_error.clear();
    if (!pandocAvailable()) {
        m_error = QStringLiteral("pandoc is not installed.");
        return QString();
    }
    const QString folder = templatesFolder();
    if (folder.isEmpty()) {
        m_error = QStringLiteral("Open a notes folder first.");
        return QString();
    }
    QDir().mkpath(folder);

    QString extension;
    QStringList args;
    const QString k = kind.toLower();
    if (k == QLatin1String("latex")) {
        extension = QStringLiteral("tex");
        args = {QStringLiteral("--print-default-template=latex")};
    } else if (k == QLatin1String("html")) {
        extension = QStringLiteral("html");
        args = {QStringLiteral("--print-default-template=html5")};
    } else if (k == QLatin1String("docx") || k == QLatin1String("odt") || k == QLatin1String("pptx")) {
        extension = k;
    } else {
        m_error = QStringLiteral("Unknown kind of template: %1.").arg(kind);
        return QString();
    }

    QString base = Vault::cleanName(name);
    if (base.endsWith(QLatin1Char('.') + extension, Qt::CaseInsensitive))
        base.chop(extension.size() + 1);
    if (base.isEmpty())
        base = extension == QLatin1String("tex") ? QStringLiteral("Article")
             : extension == QLatin1String("html") ? QStringLiteral("Page")
                                                   : QStringLiteral("Reference");
    const QString path = Vault::uniquePath(folder, base, extension);
    if (args.isEmpty())
        args = {QStringLiteral("-o"), path, QStringLiteral("--print-default-data-file"),
                QStringLiteral("reference.") + extension};

    QProcess pandoc;
    pandoc.start(pandocPath(), args);
    if (!pandoc.waitForFinished(30000) || pandoc.exitCode() != 0) {
        m_error = QStringLiteral("pandoc could not make the %1 file: %2")
                      .arg(extension, QString::fromUtf8(pandoc.readAllStandardError()).trimmed());
        QFile::remove(path);
        return QString();
    }
    if (extension == QLatin1String("tex") || extension == QLatin1String("html")) {
        if (!writeText(path, QString::fromUtf8(pandoc.readAllStandardOutput()))) {
            m_error = QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName());
            return QString();
        }
    }
    reload();
    return path;
}

// Sets key (template or reference-doc) in a profile, replacing an existing
// line for it, commented out or not.
QString Exporter::withTemplate(const QString &profileText, const QString &key, const QString &value) {
    QStringList lines = profileText.split(QLatin1Char('\n'));
    const QRegularExpression keyRe(QStringLiteral("^#?\\s*%1\\s*:").arg(QRegularExpression::escape(key)));
    const QString line = QStringLiteral("%1: %2").arg(key, value);
    for (QString &existing : lines) {
        if (keyRe.match(existing).hasMatch()) {
            existing = line;
            return lines.join(QLatin1Char('\n'));
        }
    }
    QString text = profileText;
    if (!text.endsWith(QLatin1Char('\n')))
        text += QLatin1Char('\n');
    return text + line + QLatin1Char('\n');
}

QString Exporter::newProfile(const QString &name, const QString &baseProfile, const QString &templatePath) {
    m_error.clear();
    const QString folder = profilesFolder();
    QString base = Vault::cleanName(name);
    if (folder.isEmpty() || base.isEmpty()) {
        m_error = QStringLiteral("A profile needs a name.");
        return QString();
    }
    QString text = readText(baseProfile);
    if (text.isEmpty())
        text = QStringLiteral("to: pdf\n");
    if (!templatePath.isEmpty()) {
        const QString suffix = QFileInfo(templatePath).suffix().toLower();
        const QString key = referenceSuffixes.contains(suffix) ? QStringLiteral("reference-doc")
                                                               : QStringLiteral("template");
        text = withTemplate(text, key, QStringLiteral("${.}/../templates/") + QFileInfo(templatePath).fileName());
        // A reference document only fits its own format.
        if (key == QLatin1String("reference-doc") && formatOf(text) != suffix) {
            static const QRegularExpression toRe(QStringLiteral("^(to|writer)\\s*:.*$"),
                                                 QRegularExpression::MultilineOption);
            if (toRe.match(text).hasMatch())
                text.replace(toRe, QStringLiteral("to: ") + suffix);
            else
                text.prepend(QStringLiteral("to: ") + suffix + QLatin1Char('\n'));
        }
    }
    QDir().mkpath(folder);
    const QString path = Vault::uniquePath(folder, base, QStringLiteral("yaml"));
    if (!writeText(path, text)) {
        m_error = QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName());
        return QString();
    }
    reload();
    return path;
}

QString Exporter::defaultOutputPath(const QString &notePath, const QString &profilePath) const {
    const QFileInfo note(notePath);
    const QString extension = extensionFor(formatOf(readText(profilePath)));
    const QDir folder = exportLocation() == QLatin1String("folder") && !m_root.isEmpty()
        ? QDir(exportsFolder()) : note.absoluteDir();
    return folder.filePath(note.completeBaseName() + QLatin1Char('.') + extension);
}

// What pandoc reads: [[links]] become their text, and an OmaZettel template
// settings line is dropped.
QString Exporter::prepareText(const QString &text) {
    static const QRegularExpression settingsRe(QStringLiteral("^\\s*<!--\\s*omazettel:.*?-->[ \\t]*\\n?"));
    QString prepared = text;
    prepared.remove(settingsRe);
    return Zettel::plainWikiLinks(prepared);
}

bool Exporter::exportNote(const QString &notePath, const QString &profilePath,
                          const QString &outputPath) {
    return exportText(notePath, profilePath, outputPath, readText(notePath));
}

bool Exporter::exportText(const QString &notePath, const QString &profilePath,
                          const QString &outputPath, const QString &text) {
    const QFileInfo note(notePath);
    return runPandoc(text, note.absolutePath(), {note.absolutePath()}, profilePath, outputPath,
                     note.completeBaseName());
}

bool Exporter::runPandoc(const QString &text, const QString &workingDirectory,
                         const QStringList &resourceFolders, const QString &profilePath,
                         const QString &outputPath, const QString &fallbackTitle) {
    m_error.clear();
    if (m_process) {
        m_error = QStringLiteral("An export is already running.");
        return false;
    }
    const QString pandoc = pandocPath();
    if (pandoc.isEmpty()) {
        m_error = QStringLiteral("pandoc is not installed. Install it with: sudo pacman -S pandoc-cli");
        return false;
    }
    if (!QFileInfo(profilePath).isFile()) {
        m_error = QStringLiteral("The export profile is missing.");
        return false;
    }
    QDir().mkpath(QFileInfo(outputPath).absolutePath());

    QStringList resources = resourceFolders;
    if (!m_root.isEmpty())
        resources.append(m_root);
    resources.removeDuplicates();
    // No -f: a profile may set its own reader (from: markdown+...), and
    // pandoc reads Markdown from stdin by default.
    QStringList args{QStringLiteral("-d"), profilePath,
                     QStringLiteral("-o"), outputPath,
                     QStringLiteral("--resource-path=") + resources.join(QLatin1Char(':')),
                     // Only used when there is no title, so HTML is valid.
                     QStringLiteral("--metadata=pagetitle:%1").arg(fallbackTitle)};
    args += citationArguments(text);

    m_output = outputPath;
    m_process = new QProcess(this);
    m_process->setWorkingDirectory(workingDirectory);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const QString messages = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        const bool ok = status == QProcess::NormalExit && exitCode == 0 && QFileInfo::exists(m_output);
        QString message = messages;
        if (!ok && message.isEmpty())
            message = status == QProcess::CrashExit ? QStringLiteral("pandoc stopped (took too long or crashed).")
                                                    : QStringLiteral("pandoc failed with exit code %1.").arg(exitCode);
        finish(ok, message);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false, QStringLiteral("pandoc could not be started."));
    });
    m_process->start(pandoc, args);
    m_process->write(prepareText(text).toUtf8());
    m_process->closeWriteChannel();
    m_timeout.start();
    emit busyChanged();
    return true;
}

void Exporter::cancel() {
    if (m_process)
        m_process->kill();
}

void Exporter::finish(bool ok, const QString &message) {
    if (!m_process)
        return;
    m_timeout.stop();
    m_process->deleteLater();
    m_process = nullptr;
    // Keep only the end of a long LaTeX log.
    QStringList lines = message.split(QLatin1Char('\n'));
    if (lines.size() > 15)
        lines = lines.mid(lines.size() - 15);
    emit busyChanged();
    emit finished(ok, m_output, lines.join(QLatin1Char('\n')));
}

// ── Folder export ─────────────────────────────────────────────────────────

namespace {
QString projectFile(const QString &folder) {
    return QDir(folder).filePath(QStringLiteral(".omazettel-project.yaml"));
}

QString yamlQuoted(const QString &value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString yamlUnquoted(QString value) {
    value = value.trimmed();
    if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
        value = value.mid(1, value.size() - 2);
        value.replace(QLatin1String("\\\""), QLatin1String("\""));
        value.replace(QLatin1String("\\\\"), QLatin1String("\\"));
    }
    return value;
}

struct ProjectSettings {
    QString title;
    QString author;
    QString profile;
    QStringList files;
    QStringList excluded;
};

ProjectSettings readProject(const QString &folder) {
    ProjectSettings project;
    QString section;
    const QStringList lines = readText(projectFile(folder)).split(QLatin1Char('\n'));
    static const QRegularExpression keyRe(QStringLiteral("^([\\w-]+)\\s*:\\s*(.*)$"));
    static const QRegularExpression itemRe(QStringLiteral("^\\s+-\\s*(.+)$"));
    for (const QString &line : lines) {
        const QRegularExpressionMatch item = itemRe.match(line);
        if (item.hasMatch()) {
            if (section == QLatin1String("files"))
                project.files.append(yamlUnquoted(item.captured(1)));
            else if (section == QLatin1String("excluded"))
                project.excluded.append(yamlUnquoted(item.captured(1)));
            continue;
        }
        const QRegularExpressionMatch key = keyRe.match(line);
        if (!key.hasMatch())
            continue;
        section = key.captured(1);
        const QString value = yamlUnquoted(key.captured(2));
        if (section == QLatin1String("title"))
            project.title = value;
        else if (section == QLatin1String("author"))
            project.author = value;
        else if (section == QLatin1String("profile"))
            project.profile = value;
    }
    return project;
}

QStringList notesUnder(const QString &folder) {
    QStringList relative;
    QDirIterator it(folder, {QStringLiteral("*.md"), QStringLiteral("*.markdown")},
                    QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString rel = path.mid(folder.size() + 1);
        if (rel.startsWith(QLatin1Char('.')) || rel.contains(QLatin1String("/.")))
            continue;
        relative.append(rel);
    }
    // Folder by folder, in natural order (2 before 10), a folder's own notes
    // before its subfolders.
    std::sort(relative.begin(), relative.end(), [](const QString &a, const QString &b) {
        const QStringList ap = a.split(QLatin1Char('/'));
        const QStringList bp = b.split(QLatin1Char('/'));
        for (int i = 0;; ++i) {
            const bool aFile = i == ap.size() - 1;
            const bool bFile = i == bp.size() - 1;
            if (aFile != bFile)
                return aFile;
            const int order = NotesTree::naturalCompare(ap.at(i), bp.at(i));
            if (order != 0)
                return order < 0;
            if (aFile)
                return false;
        }
    });
    return relative;
}

QString stripNoteSuffix(QString name) {
    if (name.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        name.chop(3);
    else if (name.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive))
        name.chop(9);
    return name;
}
}

QVariantList Exporter::folderNotes(const QString &folder) const {
    const ProjectSettings project = readProject(folder);
    const QStringList present = notesUnder(folder);
    QStringList ordered;
    for (const QString &file : project.files) {
        if (present.contains(file) && !ordered.contains(file))
            ordered.append(file);
    }
    for (const QString &file : project.excluded) {
        if (present.contains(file) && !ordered.contains(file))
            ordered.append(file);
    }
    // Notes the project does not mention yet go at the end, in natural order.
    for (const QString &file : present) {
        if (!ordered.contains(file))
            ordered.append(file);
    }
    QVariantList result;
    for (const QString &file : ordered) {
        result.append(QVariantMap{{QStringLiteral("path"), QDir(folder).filePath(file)},
                                  {QStringLiteral("name"), stripNoteSuffix(file)},
                                  {QStringLiteral("included"), !project.excluded.contains(file)}});
    }
    return result;
}

QVariantMap Exporter::folderSettings(const QString &folder) const {
    const ProjectSettings project = readProject(folder);
    QString profile;
    if (!project.profile.isEmpty()) {
        const QString path = QDir(profilesFolder()).filePath(project.profile + QStringLiteral(".yaml"));
        if (QFileInfo(path).isFile())
            profile = path;
    }
    return {{QStringLiteral("title"), project.title.isEmpty() ? QFileInfo(folder).fileName() : project.title},
            {QStringLiteral("author"), project.author},
            {QStringLiteral("profile"), profile}};
}

bool Exporter::saveFolderSettings(const QString &folder, const QVariantList &entries,
                                  const QString &title, const QString &author,
                                  const QString &profilePath) {
    QString text = QStringLiteral(
        "# OmaZettel folder export: the notes in order, and which to leave out.\n");
    text += QStringLiteral("title: %1\n").arg(yamlQuoted(title));
    text += QStringLiteral("author: %1\n").arg(yamlQuoted(author));
    if (!profilePath.isEmpty())
        text += QStringLiteral("profile: %1\n").arg(yamlQuoted(QFileInfo(profilePath).completeBaseName()));
    // Every note in order, and separately the ones left out, so leaving a
    // note out keeps its place.
    QStringList ordered;
    QStringList excluded;
    for (const QVariant &entry : entries) {
        const QVariantMap map = entry.toMap();
        const QString relative = map.value(QStringLiteral("path")).toString().mid(folder.size() + 1);
        ordered.append(relative);
        if (!map.value(QStringLiteral("included")).toBool())
            excluded.append(relative);
    }
    text += QStringLiteral("files:\n");
    for (const QString &file : ordered)
        text += QStringLiteral("  - %1\n").arg(yamlQuoted(file));
    text += QStringLiteral("excluded:\n");
    for (const QString &file : excluded)
        text += QStringLiteral("  - %1\n").arg(yamlQuoted(file));
    return writeText(projectFile(folder), text);
}

QString Exporter::defaultFolderOutputPath(const QString &folder, const QString &profilePath) const {
    const QString extension = extensionFor(formatOf(readText(profilePath)));
    const QDir target = exportLocation() == QLatin1String("folder") && !m_root.isEmpty()
        ? QDir(exportsFolder()) : QDir(folder);
    return target.filePath(QFileInfo(folder).fileName() + QLatin1Char('.') + extension);
}

QString Exporter::combineNotes(const QStringList &notePaths, const QString &title,
                               const QString &author, const QDate &date) {
    QString combined = QStringLiteral("---\n");
    if (!title.isEmpty())
        combined += QStringLiteral("title: %1\n").arg(yamlQuoted(title));
    if (!author.isEmpty())
        combined += QStringLiteral("author: %1\n").arg(yamlQuoted(author));
    combined += QStringLiteral("date: %1\n---\n\n").arg(yamlQuoted(date.toString(Qt::ISODate)));

    static const QRegularExpression settingsRe(QStringLiteral("^\\s*<!--\\s*omazettel:.*?-->[ \\t]*\\n?"));
    static const QRegularExpression frontRe(QStringLiteral("^---[ \\t]*\\n(.*?)\\n(?:---|\\.\\.\\.)[ \\t]*(?:\\n|$)"),
                                            QRegularExpression::DotMatchesEverythingOption);
    for (const QString &path : notePaths) {
        QString text = readText(path);
        text.remove(settingsRe);
        const Zettel::ParsedNote parsed = Zettel::parse(text);
        const QRegularExpressionMatch front = frontRe.match(text);
        const bool hadFrontMatter = front.hasMatch();
        if (hadFrontMatter)
            text.remove(0, front.capturedLength());
        text = text.trimmed();
        // A note titled only in its header gets the title as its heading.
        if (hadFrontMatter && !parsed.title.isEmpty() && !text.startsWith(QLatin1String("# ")))
            text.prepend(QStringLiteral("# ") + parsed.title + QStringLiteral("\n\n"));
        combined += text + QStringLiteral("\n\n");
    }
    return combined;
}

bool Exporter::exportFolder(const QString &folder, const QStringList &notePaths,
                            const QString &title, const QString &author,
                            const QString &profilePath, const QString &outputPath) {
    if (notePaths.isEmpty()) {
        m_error = QStringLiteral("Choose at least one note to export.");
        return false;
    }
    QStringList folders;
    for (const QString &path : notePaths)
        folders.append(QFileInfo(path).absolutePath());
    folders.removeDuplicates();
    return runPandoc(combineNotes(notePaths, title, author, QDate::currentDate()), folder,
                     folders, profilePath, outputPath, QFileInfo(folder).fileName());
}

QStringList Exporter::citationArguments(const QString &text) const {
    static const QRegularExpression citeRe(QStringLiteral("(^|[\\s\\[;-])@[\\w{]"));
    const bool cites = citeRe.match(text).hasMatch();
    static const QRegularExpression ownRe(QStringLiteral("^bibliography\\s*:"),
                                          QRegularExpression::MultilineOption);
    const bool ownBibliography = ownRe.match(text.left(4000)).hasMatch();
    if (!cites && !ownBibliography)
        return {};
    QStringList args{QStringLiteral("--citeproc")};
    const Citations::Settings settings = Citations::settingsFor(m_root);
    if (!ownBibliography && QFileInfo(settings.bibliography).isFile())
        args.append(QStringLiteral("--bibliography=") + settings.bibliography);
    if (QFileInfo(settings.style).isFile())
        args.append(QStringLiteral("--csl=") + settings.style);
    // Without any bibliography, citeproc only warns about every citation.
    if (!ownBibliography && args.size() == 1 + (QFileInfo(settings.style).isFile() ? 1 : 0)
            && !QFileInfo(settings.bibliography).isFile())
        return {};
    return args;
}
