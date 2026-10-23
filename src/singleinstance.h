#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>

class QLocalSocket;

// One OmaZettel window per user. Two windows would each save their own last
// note, recent folders and sync state to the same settings, overwriting each
// other, and would sync the same notes folder twice.
//
// The first OmaZettel to start listens on a local socket that only this user
// can open. A later start hands its files to that one, which comes to the
// front and opens them, and then exits. The command-line tools and the MCP
// server are separate processes and are not affected.
class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject *parent = nullptr);

    // The socket's name, distinct per user.
    static QString serverName();

    // Hands the files (absolute paths; none just raises the window) to a
    // running OmaZettel. True when one took them, and this one should exit.
    static bool forwardToRunning(const QStringList &files, int timeoutMs = 2000);

    // Becomes the running OmaZettel. False if another one started first
    // (call forwardToRunning again) or the socket could not be made.
    bool listen();

signals:
    // Another start asked this one to open these files (none: to come to
    // the front). activationToken is the desktop's permission to take focus.
    void openRequested(const QStringList &files, const QString &activationToken);

private:
    void readRequest(QLocalSocket *socket);

    QLocalServer m_server;
};
