#pragma once

#include "../network.hpp"
#include "emoji_data.hpp"
#include <QDir>
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <thread>

class Network;

struct EmoteSearchResult {
    QString name;
    QString displayText;
    QString iconPath;
    bool isCustom;
};

class EmoteManager : public QObject {
    Q_OBJECT

  public:
    EmoteManager(Network &network);
    ~EmoteManager() = default;

    void requestEmotes();
    QList<EmoteSearchResult> search(const QString &query, int maxResults = 8) const;
    QString getEmotePath(const QString &name) const;
    QString getEmojiUnicode(const QString &name) const;
    bool isCustomEmote(const QString &name) const;
    bool isEmoji(const QString &name) const;
    QString processEmoteCodes(const QString &text) const;

  signals:
    void emoteListReady();
    void emotesProcessed(QStringList names);

  private slots:
    void handleEmoteListResponse(const QJsonArray &emotes);

  private:
    Network &networkManager;
    QSet<QString> customEmotes;
    std::jthread emoteProcessThread;
    QString emotesCacheDir() const;
    void loadCachedEmotes();
};
