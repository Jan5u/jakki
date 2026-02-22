#pragma once

#include "../network.hpp"
#include "emoji_data.hpp"
#include <QDir>
#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <QTextDocument>

class Network;

struct EmoteSearchResult {
    QString name;
    QString displayText; // Unicode char for emoji, ":name:" for custom
    QPixmap icon;        // Preview icon (empty for unicode emoji)
    bool isCustom;
};

class EmoteManager : public QObject {
    Q_OBJECT

  public:
    EmoteManager(Network &network);
    ~EmoteManager() = default;

    void requestEmotes();
    QList<EmoteSearchResult> search(const QString &query, int maxResults = 8) const;
    QPixmap getEmoteImage(const QString &name) const;
    QString getEmojiUnicode(const QString &name) const;
    bool isCustomEmote(const QString &name) const;
    bool isEmoji(const QString &name) const;
    void registerEmotesInDocument(QTextDocument *doc) const;
    QString processEmoteCodes(const QString &text) const;

  signals:
    void emoteListReady();

  private slots:
    void handleEmoteListResponse(const QJsonArray &emotes);

  private:
    Network &networkManager;
    QMap<QString, QPixmap> customEmotes;
    QString emotesCacheDir() const;
    void loadCachedEmotes();
    void saveCachedEmote(const QString &name, const QPixmap &pixmap);
};
