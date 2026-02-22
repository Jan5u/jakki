#include "emote_manager.hpp"
#include <QImage>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <iostream>

EmoteManager::EmoteManager(Network &network) : networkManager(network) {
    connect(&networkManager, &Network::emoteListReceived, this, &EmoteManager::handleEmoteListResponse);
    connect(this, &EmoteManager::emotesProcessed, this, [this](const QStringList &names) {
        for (const auto &name : names) {
            customEmotes.insert(name);
        }
        std::cout << "Loaded " << names.size() << " emotes from server" << std::endl;
        emit emoteListReady();
    });
    loadCachedEmotes();
}

QString EmoteManager::emotesCacheDir() const {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return configDir + "/emotes";
}

void EmoteManager::loadCachedEmotes() {
    QDir dir(emotesCacheDir());
    if (!dir.exists())
        return;

    QStringList filters = {"*.png"};
    for (const auto &file : dir.entryList(filters, QDir::Files)) {
        QString name = QFileInfo(file).baseName();
        if (QFile::exists(dir.filePath(file))) {
            customEmotes.insert(name);
        }
    }

    if (!customEmotes.isEmpty()) {
        std::cout << "Loaded " << customEmotes.size() << " cached emotes" << std::endl;
    }
}

void EmoteManager::requestEmotes() {
    json j;
    j["type"] = "emote_list_request";
    networkManager.sendTextMessage(QString::fromStdString(j.dump()));
}

void EmoteManager::handleEmoteListResponse(const QJsonArray &emotes) {
    struct RawEmote {
        QString name;
        QByteArray imageData;
    };
    QVector<RawEmote> rawEmotes;
    for (const auto &emoteVal : emotes) {
        QJsonObject obj = emoteVal.toObject();
        QString name = obj["name"].toString();
        QString data = obj["data"].toString();

        if (name.isEmpty() || data.isEmpty())
            continue;

        rawEmotes.append({name, QByteArray::fromBase64(data.toLatin1())});
    }

    QString cacheDir = emotesCacheDir();

    emoteProcessThread = std::jthread([this, rawEmotes, cacheDir]() {
        QStringList names;

        QDir dir(cacheDir);
        if (!dir.exists())
            dir.mkpath(".");

        for (const auto &raw : rawEmotes) {
            QImage image;
            if (image.loadFromData(raw.imageData)) {
                QImage scaled = image.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                scaled.save(dir.filePath(raw.name + ".png"), "PNG");
                names.append(raw.name);
            }
        }

        emit emotesProcessed(names);
    });
}

QList<EmoteSearchResult> EmoteManager::search(const QString &query, int maxResults) const {
    QList<EmoteSearchResult> results;
    QString lowerQuery = query.toLower();

    if (lowerQuery.isEmpty())
        return results;

    QList<EmoteSearchResult> prefixMatches;
    QList<EmoteSearchResult> substringMatches;

    for (const auto &name : customEmotes) {
        EmoteSearchResult result;
        result.name = name;
        result.displayText = ":" + name + ":";
        result.iconPath = getEmotePath(name);
        result.isCustom = true;

        if (name.toLower().startsWith(lowerQuery)) {
            prefixMatches.append(result);
        } else if (name.toLower().contains(lowerQuery)) {
            substringMatches.append(result);
        }
    }

    results.append(prefixMatches);
    results.append(substringMatches);

    prefixMatches.clear();
    substringMatches.clear();

    const auto &emojiDb = getEmojiDatabase();
    for (const auto &emoji : emojiDb) {
        EmoteSearchResult result;
        result.name = emoji.name;
        result.displayText = emoji.unicode;
        result.iconPath.clear();
        result.isCustom = false;

        if (emoji.name.toLower().startsWith(lowerQuery)) {
            prefixMatches.append(result);
        } else if (emoji.name.toLower().contains(lowerQuery)) {
            substringMatches.append(result);
        }
    }

    results.append(prefixMatches);
    results.append(substringMatches);

    if (results.size() > maxResults) {
        results = results.mid(0, maxResults);
    }

    return results;
}

QString EmoteManager::getEmotePath(const QString &name) const {
    if (!customEmotes.contains(name))
        return {};
    return emotesCacheDir() + "/" + name + ".png";
}

QString EmoteManager::getEmojiUnicode(const QString &name) const {
    const auto &db = getEmojiDatabase();
    for (const auto &emoji : db) {
        if (emoji.name == name)
            return emoji.unicode;
    }
    return {};
}

bool EmoteManager::isCustomEmote(const QString &name) const { return customEmotes.contains(name); }

bool EmoteManager::isEmoji(const QString &name) const {
    const auto &db = getEmojiDatabase();
    for (const auto &emoji : db) {
        if (emoji.name == name)
            return true;
    }
    return false;
}

QString EmoteManager::processEmoteCodes(const QString &text) const {
    static QRegularExpression emoteRegex(R"(:([a-zA-Z0-9_]+):)");
    QString result = text;

    auto matchIt = emoteRegex.globalMatch(result);
    QList<QRegularExpressionMatch> matches;
    while (matchIt.hasNext()) {
        matches.append(matchIt.next());
    }

    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto &match = matches[i];
        QString emoteName = match.captured(1);

        if (isCustomEmote(emoteName)) {
            QString path = QUrl::fromLocalFile(getEmotePath(emoteName)).toString();
            QString imgTag = QString(R"(<img src="%1" width="24" height="24" alt=":%2:">)").arg(path, emoteName);
            result.replace(match.capturedStart(), match.capturedLength(), imgTag);
        } else if (isEmoji(emoteName)) {
            QString unicode = getEmojiUnicode(emoteName);
            result.replace(match.capturedStart(), match.capturedLength(), unicode);
        }
    }

    return result;
}
