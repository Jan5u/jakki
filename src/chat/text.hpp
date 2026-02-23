#pragma once

#include "../network.hpp"
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

class Network;

struct Message {
    int id = 0;
    QString sender;
    QString content;
    QDateTime timestamp;
};

class Text : public QObject {
    Q_OBJECT

  public:
    Text(Network &network);
    ~Text() = default;

    void sendMessage(const QString &channel, const QString &message);
    void sendTypingIndicator(const QString &channel);
    void requestHistory(const QString &channel, int limit = 50, int before = 0);

  signals:
    void messageReceived(const QString &channel, const QString &sender, const QString &content, const QDateTime &timestamp);
    void historyReceived(const QString &channel, const QList<Message> &messages);
    void typingIndicatorReceived(const QString &channel, const QString &user);

  private slots:
    void handleIncomingMessage(const QString &channel, const QString &sender, const QString &content, bool compressed);
    void handleHistoryResponse(const QString &channel, const QJsonArray &messages);

  private:
    Network &networkManager;

    QByteArray compress(const QByteArray &data);
    QByteArray decompress(const QByteArray &data);
    QString toBase64(const QByteArray &data);
    QByteArray fromBase64(const QString &data);
};
