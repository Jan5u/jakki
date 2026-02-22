#include "emote_manager.hpp"
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <iostream>

EmoteManager::EmoteManager(Network &network) : networkManager(network) {
    connect(&networkManager, &Network::emoteListReceived, this, &EmoteManager::handleEmoteListResponse);
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
        QPixmap pixmap(dir.filePath(file));
        if (!pixmap.isNull()) {
            customEmotes[name] = pixmap.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }

    if (!customEmotes.isEmpty()) {
        std::cout << "Loaded " << customEmotes.size() << " cached emotes" << std::endl;
    }
}

void EmoteManager::saveCachedEmote(const QString &name, const QPixmap &pixmap) {
    QDir dir(emotesCacheDir());
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    pixmap.save(dir.filePath(name + ".png"), "PNG");
}

void EmoteManager::requestEmotes() {
    json j;
    j["type"] = "emote_list_request";
    networkManager.sendTextMessage(QString::fromStdString(j.dump()));
}

void EmoteManager::handleEmoteListResponse(const QJsonArray &emotes) {
    for (const auto &emoteVal : emotes) {
        QJsonObject obj = emoteVal.toObject();
        QString name = obj["name"].toString();
        QString data = obj["data"].toString();

        if (name.isEmpty() || data.isEmpty())
            continue;

        QByteArray imageData = QByteArray::fromBase64(data.toLatin1());
        QPixmap pixmap;
        if (pixmap.loadFromData(imageData)) {
            QPixmap scaled = pixmap.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            customEmotes[name] = scaled;
            saveCachedEmote(name, pixmap);
        }
    }

    std::cout << "Loaded " << emotes.size() << " emotes from server" << std::endl;
    emit emoteListReady();
}

QList<EmoteSearchResult> EmoteManager::search(const QString &query, int maxResults) const {
    QList<EmoteSearchResult> results;
    QString lowerQuery = query.toLower();

    if (lowerQuery.isEmpty())
        return results;

    // Search custom emotes first (prefix matches first, then substring)
    QList<EmoteSearchResult> prefixMatches;
    QList<EmoteSearchResult> substringMatches;

    for (auto it = customEmotes.constBegin(); it != customEmotes.constEnd(); ++it) {
        EmoteSearchResult result;
        result.name = it.key();
        result.displayText = ":" + it.key() + ":";
        result.icon = it.value();
        result.isCustom = true;

        if (it.key().toLower().startsWith(lowerQuery)) {
            prefixMatches.append(result);
        } else if (it.key().toLower().contains(lowerQuery)) {
            substringMatches.append(result);
        }
    }

    results.append(prefixMatches);
    results.append(substringMatches);

    // Search unicode emoji database
    prefixMatches.clear();
    substringMatches.clear();

    const auto &emojiDb = getEmojiDatabase();
    for (const auto &emoji : emojiDb) {
        EmoteSearchResult result;
        result.name = emoji.name;
        result.displayText = emoji.unicode;
        result.icon = QPixmap(); // No icon for unicode emoji
        result.isCustom = false;

        if (emoji.name.toLower().startsWith(lowerQuery)) {
            prefixMatches.append(result);
        } else if (emoji.name.toLower().contains(lowerQuery)) {
            substringMatches.append(result);
        }
    }

    results.append(prefixMatches);
    results.append(substringMatches);

    // Limit results
    if (results.size() > maxResults) {
        results = results.mid(0, maxResults);
    }

    return results;
}

QPixmap EmoteManager::getEmoteImage(const QString &name) const { return customEmotes.value(name, QPixmap()); }

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

void EmoteManager::registerEmotesInDocument(QTextDocument *doc) const {
    if (!doc)
        return;
    for (auto it = customEmotes.constBegin(); it != customEmotes.constEnd(); ++it) {
        QUrl resourceUrl("emote://" + it.key());
        doc->addResource(QTextDocument::ImageResource, resourceUrl, it.value());
    }
}

QString EmoteManager::processEmoteCodes(const QString &text) const {
    static QRegularExpression emoteRegex(R"(:([a-zA-Z0-9_]+):)");
    QString result = text;

    auto matchIt = emoteRegex.globalMatch(result);
    // Process matches in reverse to preserve positions
    QList<QRegularExpressionMatch> matches;
    while (matchIt.hasNext()) {
        matches.append(matchIt.next());
    }

    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto &match = matches[i];
        QString emoteName = match.captured(1);

        if (isCustomEmote(emoteName)) {
            // Replace with <img> tag for custom emotes
            QString imgTag = QString(R"(<img src="emote://%1" width="24" height="24" alt=":%1:">)").arg(emoteName);
            result.replace(match.capturedStart(), match.capturedLength(), imgTag);
        } else if (isEmoji(emoteName)) {
            // Replace with unicode character
            QString unicode = getEmojiUnicode(emoteName);
            result.replace(match.capturedStart(), match.capturedLength(), unicode);
        }
        // Leave unknown :codes: as-is
    }

    return result;
}
