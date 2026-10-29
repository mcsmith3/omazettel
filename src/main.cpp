#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QUrl>
#include <QWindow>
#include <QElapsedTimer>
#include <QFileOpenEvent>
#include <QFile>
#include <QSettings>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <unistd.h>

#include "backend.h"
#include "cli.h"
#include "systemtheme.h"
#include "citations.h"
#include "dashboard.h"
#include "exporter.h"
#include "noteindex.h"
#include "notetemplates.h"
#include "pdfdocument.h"
#include "singleinstance.h"
#include "sync.h"
#include "vault.h"

namespace {
// macOS hands over files opened from Finder (double-click, Open With,
// dropping on the Dock icon) as events, not arguments.
class FileOpenFilter : public QObject {
public:
    FileOpenFilter(QObject *window, QObject *parent) : QObject(parent), m_window(window) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::FileOpen)
            return QObject::eventFilter(watched, event);
        const QString file = static_cast<QFileOpenEvent *>(event)->file();
        if (!file.isEmpty())
            QMetaObject::invokeMethod(m_window, "openFromLaunch", Q_ARG(QVariant, QUrl::fromLocalFile(file)));
        return true;
    }

private:
    QObject *m_window;
};
}

#define OMAZETTEL_VERSION "0.12.0"

int main(int argc, char *argv[]) {
    // Answered before the application exists, so they need no display.
    if (const std::optional<int> exitCode =
            Cli::handleArguments(argc, argv, QStringLiteral(OMAZETTEL_VERSION)))
        return *exitCode;

    // Omarchy asks Qt for GTK 3 dialogs, which run inside this process, and a
    // long-standing GTK 3 file chooser assertion aborts the whole app, unsaved
    // text and all. The desktop portal shows the same chooser from a process
    // of its own.
    if (qgetenv("QT_QPA_PLATFORMTHEME") == "gtk3")
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("omazettel"));
    app.setApplicationVersion(QStringLiteral(OMAZETTEL_VERSION));
    app.setDesktopFileName(QStringLiteral("omazettel"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omazettel")));

    app.setOrganizationName(QStringLiteral("omazettel"));

    // One window per user: a second start hands its file to the first and
    // exits. OMAZETTEL_NEW_INSTANCE=1 opens another window anyway.
    QStringList launchFiles;
    if (app.arguments().size() > 1)
        launchFiles.append(QFileInfo(app.arguments().at(1)).absoluteFilePath());
    const bool separate = qEnvironmentVariableIntValue("OMAZETTEL_NEW_INSTANCE") == 1;
    SingleInstance instance(&app);
    if (!separate) {
        if (SingleInstance::forwardToRunning(launchFiles))
            return 0;
        // Another start may have claimed it a moment ago.
        if (!instance.listen() && SingleInstance::forwardToRunning(launchFiles))
            return 0;
    }

    QQuickStyle::setStyle(QStringLiteral("Material"));

    Backend backend(&app);
    // OmaZettel: colours follow the Omarchy theme unless the Oma palette is on.
    backend.setOmaPalette(QSettings().value(QStringLiteral("appearance/omaPalette"), false).toBool());
    SystemTheme systemTheme(&app);
    backend.setDarkMode(systemTheme.darkMode());
    QObject::connect(&systemTheme, &SystemTheme::darkModeChanged, &backend,
                     &Backend::setDarkMode);

    // Carry the text scale into the default font, so the chrome that inherits
    // it (dialog titles, buttons) grows along with the writing area.
    // OmaZettel: the interface uses the desktop's monospace font too.
    const QFont interfaceFont(Backend::defaultEditorFont());
    const qreal basePointSize = interfaceFont.pointSizeF() > 0
        ? interfaceFont.pointSizeF()
        : app.font().pointSizeF();
    const auto applyInterfaceFont = [&app, interfaceFont, basePointSize](qreal textScale) {
        QFont scaled = interfaceFont;
        scaled.setPointSizeF(basePointSize * textScale);
        app.setFont(scaled);
    };

    // The desktop sets the baseline; the backend folds the user's own text
    // size into it and reports the total.
    backend.setDesktopTextScale(systemTheme.textScale());
    applyInterfaceFont(backend.textScale());
    QObject::connect(&systemTheme, &SystemTheme::textScaleChanged, &backend,
                     &Backend::setDesktopTextScale);
    QObject::connect(&backend, &Backend::textScaleChanged, &app,
                     [&backend, applyInterfaceFont]() {
        applyInterfaceFont(backend.textScale());
    });

    // OmaZettel: the notes folder. Created after the organization and
    // application names are set, since it reads its settings on creation,
    // and before the engine, so it outlives the interface that uses it.
    Vault vault(&app);
    // The note index follows the notes folder, rescans after the sidebar
    // changes files and whenever the app comes back to the front (to pick up
    // edits made elsewhere, such as in VS Code).
    NoteIndex noteIndex(&app);
    noteIndex.setRoot(vault.rootPath());
    QObject::connect(&vault, &Vault::rootPathChanged, &noteIndex,
                     [&vault, &noteIndex]() { noteIndex.setRoot(vault.rootPath()); });
    QObject::connect(&vault, &Vault::contentsChanged, &noteIndex, &NoteIndex::scheduleRescan);
    // Note templates live in the notes folder, in .omazettel/note-templates.
    NoteTemplates noteTemplates(&app);
    noteTemplates.setRoot(vault.rootPath());
    QObject::connect(&vault, &Vault::rootPathChanged, &noteTemplates,
                     [&vault, &noteTemplates]() { noteTemplates.setRoot(vault.rootPath()); });
    // Export profiles and templates live in .omazettel/export and /templates.
    Exporter exporter(&app);
    exporter.setRoot(vault.rootPath());
    QObject::connect(&vault, &Vault::rootPathChanged, &exporter,
                     [&vault, &exporter]() { exporter.setRoot(vault.rootPath()); });
    // Citations: the bibliography and style named in .omazettel/citations.yaml.
    Citations citations(&app);
    citations.setRoot(vault.rootPath());
    QObject::connect(&vault, &Vault::rootPathChanged, &citations,
                     [&vault, &citations]() { citations.setRoot(vault.rootPath()); });
    // Sync and version history (git), and synced-folder conflict copies.
    Sync sync(&app);
    sync.setRoot(vault.rootPath());
    QObject::connect(&vault, &Vault::rootPathChanged, &sync,
                     [&vault, &sync]() { sync.setRoot(vault.rootPath()); });
    QObject::connect(&noteIndex, &NoteIndex::changed, &sync, [&sync]() {
        // Conflict copies arrive with the files a sync tool writes.
        static QElapsedTimer last;
        if (!last.isValid() || last.elapsed() > 3000) {
            last.restart();
            sync.refresh();
        }
    });
    // The dashboard: activity, open tasks and notes that need a look.
    Dashboard dashboard(&noteIndex, &app);
    dashboard.setRoot(vault.rootPath());
    QObject::connect(&vault, &Vault::rootPathChanged, &dashboard,
                     [&vault, &dashboard]() { dashboard.setRoot(vault.rootPath()); });
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &noteIndex,
                     [&noteIndex](Qt::ApplicationState state) {
                         if (state == Qt::ApplicationActive)
                             noteIndex.scheduleRescan();
                     });

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("vault"), &vault);
    engine.rootContext()->setContextProperty(QStringLiteral("noteIndex"), &noteIndex);
    engine.rootContext()->setContextProperty(QStringLiteral("noteTemplates"), &noteTemplates);
    engine.rootContext()->setContextProperty(QStringLiteral("exporter"), &exporter);
    engine.rootContext()->setContextProperty(QStringLiteral("citations"), &citations);
    engine.rootContext()->setContextProperty(QStringLiteral("sync"), &sync);
    engine.rootContext()->setContextProperty(QStringLiteral("dashboard"), &dashboard);

    PdfDocument::install(&engine);
    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Could not load the OmaZettel interface; resource available:"
                    << QFile::exists(QStringLiteral(":/Main.qml"));
        return -1;
    }

    backend.setParentWindow(qobject_cast<QWindow *>(engine.rootObjects().constFirst()));

    // A later start of OmaZettel: come to the front, and open its file the
    // way opening a note from the sidebar does (saving or asking first).
    QObject *window = engine.rootObjects().constFirst();
    QObject::connect(&instance, &SingleInstance::openRequested, window,
                     [window](const QStringList &files, const QString &activationToken) {
        if (auto *quickWindow = qobject_cast<QWindow *>(window)) {
            // Wayland lets a window take focus with the token the desktop
            // gave the start that asked.
            if (!activationToken.isEmpty())
                qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
            quickWindow->show();
            quickWindow->raise();
            quickWindow->requestActivate();
        }
        // Hyprland (Omarchy) keeps focus where it is without a token, so ask
        // it directly.
        if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
            const QString hyprctl = QStandardPaths::findExecutable(QStringLiteral("hyprctl"));
            if (!hyprctl.isEmpty()) {
                // Hyprland's Lua dispatcher first, as Omarchy's own scripts
                // do; the older "focuswindow" form for older Hyprland.
                const QString target = QStringLiteral("pid:%1").arg(getpid());
                auto *focus = new QProcess(window);
                QObject::connect(focus, &QProcess::finished, focus, [focus, hyprctl, target](int code) {
                    if (code != 0)
                        QProcess::startDetached(hyprctl, {QStringLiteral("dispatch"), QStringLiteral("focuswindow"), target});
                    focus->deleteLater();
                });
                focus->start(hyprctl, {QStringLiteral("dispatch"),
                                       QStringLiteral("hl.dsp.focus({ window = \"%1\" })").arg(target)});
            }
        }
        if (!files.isEmpty())
            QMetaObject::invokeMethod(window, "openFromLaunch", Q_ARG(QVariant, QUrl::fromLocalFile(files.first())));
    });

    app.installEventFilter(new FileOpenFilter(window, &app));

    // A PDF shows in the PDF view, with the last note open behind it.
    const bool launchPdf = !launchFiles.isEmpty() && launchFiles.first().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
    if (!launchFiles.isEmpty() && !launchPdf)
        backend.openOrStartFile(QUrl::fromLocalFile(launchFiles.first()));
    else if (const QString lastNote = vault.lastNotePath(); !lastNote.isEmpty())
        backend.open(QUrl::fromLocalFile(lastNote));
    if (launchPdf)
        QMetaObject::invokeMethod(window, "openFromLaunch", Q_ARG(QVariant, QUrl::fromLocalFile(launchFiles.first())));

    return app.exec();
}
