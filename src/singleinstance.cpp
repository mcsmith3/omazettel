#include "singleinstance.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <unistd.h>

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { readRequest(socket); });
            if (socket->bytesAvailable() > 0)
                readRequest(socket);
        }
    });
}

QString SingleInstance::serverName() {
    // The tests use a name of their own, away from a real running window.
    const QString name = qEnvironmentVariable("OMAZETTEL_INSTANCE_NAME");
    return name.isEmpty() ? QStringLiteral("omazettel-%1").arg(getuid()) : name;
}

bool SingleInstance::forwardToRunning(const QStringList &files, int timeoutMs) {
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(timeoutMs / 4))
        return false;
    const QJsonObject request{{QStringLiteral("files"), QJsonArray::fromStringList(files)},
                              {QStringLiteral("activationToken"), qEnvironmentVariable("XDG_ACTIVATION_TOKEN")}};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(timeoutMs / 2))
        return false;
    // The running one answers once it has the request. A window stuck so
    // badly it cannot answer still has it, so this one exits either way.
    socket.waitForReadyRead(timeoutMs / 2);
    socket.disconnectFromServer();
    return true;
}

bool SingleInstance::listen() {
    // With UserAccessOption, Qt makes the socket elsewhere and moves it into
    // place, replacing one that is there. So check first that no OmaZettel
    // answers on it; one that does not was left behind by a crash.
    QLocalSocket probe;
    probe.connectToServer(serverName());
    if (probe.waitForConnected(300))
        return false;
    QLocalServer::removeServer(serverName());
    return m_server.listen(serverName());
}

void SingleInstance::readRequest(QLocalSocket *socket) {
    if (!socket->canReadLine())
        return;
    const QJsonObject request = QJsonDocument::fromJson(socket->readLine()).object();
    socket->write("ok\n");
    socket->flush();
    QStringList files;
    for (const QJsonValue &file : request.value(QStringLiteral("files")).toArray()) {
        if (!file.toString().isEmpty())
            files.append(file.toString());
    }
    emit openRequested(files, request.value(QStringLiteral("activationToken")).toString());
}
