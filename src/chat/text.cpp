#include "text.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <iostream>
#include <zstd.h>

Text::Text(Network &network) : networkManager(network) {
    connect(&networkManager, &Network::textMessageReceived, this, &Text::handleIncomingMessage);
    connect(&networkManager, &Network::historyResponseReceived, this, &Text::handleHistoryResponse);
}

QByteArray Text::compress(const QByteArray &data) {
    size_t maxSize = ZSTD_compressBound(data.size());
    QByteArray compressed(static_cast<qsizetype>(maxSize), Qt::Uninitialized);

    size_t result = ZSTD_compress(compressed.data(), maxSize, data.constData(), data.size(), 3);
    if (ZSTD_isError(result)) {
        std::cerr << "Zstd compression failed: " << ZSTD_getErrorName(result) << std::endl;
        return {};
    }

    compressed.resize(static_cast<qsizetype>(result));
    return compressed;
}

QByteArray Text::decompress(const QByteArray &data) {
    unsigned long long decompressedSize = ZSTD_getFrameContentSize(data.constData(), data.size());

    if (decompressedSize == ZSTD_CONTENTSIZE_ERROR || decompressedSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        std::cerr << "Zstd decompression: unable to determine content size" << std::endl;
        return {};
    }

    QByteArray decompressed(static_cast<qsizetype>(decompressedSize), Qt::Uninitialized);
    size_t result = ZSTD_decompress(decompressed.data(), decompressedSize, data.constData(), data.size());
    if (ZSTD_isError(result)) {
        std::cerr << "Zstd decompression failed: " << ZSTD_getErrorName(result) << std::endl;
        return {};
    }

    decompressed.resize(static_cast<qsizetype>(result));
    return decompressed;
}

QString Text::toBase64(const QByteArray &data) { return QString::fromLatin1(data.toBase64()); }

QByteArray Text::fromBase64(const QString &data) { return QByteArray::fromBase64(data.toLatin1()); }

void Text::sendMessage(const QString &channel, const QString &message) {
    if (message.isEmpty())
        return;

    QByteArray utf8Data = message.toUtf8();
    QByteArray compressed = compress(utf8Data);
    if (compressed.isEmpty()) {
        std::cerr << "Failed to compress message, sending uncompressed" << std::endl;
        // Fallback: send uncompressed
        json j;
        j["type"] = "Message";
        j["channel"] = channel.toStdString();
        j["content"] = message.toStdString();
        j["compressed"] = false;
        networkManager.sendTextMessage(QString::fromStdString(j.dump()));
        return;
    }

    QString encoded = toBase64(compressed);

    json j;
    j["type"] = "Message";
    j["channel"] = channel.toStdString();
    j["content"] = encoded.toStdString();
    j["compressed"] = true;

    networkManager.sendTextMessage(QString::fromStdString(j.dump()));
}

void Text::handleIncomingMessage(const QString &channel, const QString &sender, const QString &content, bool compressed) {
    QString decodedContent;

    if (compressed) {
        QByteArray compressedData = fromBase64(content);
        QByteArray decompressed = decompress(compressedData);
        if (decompressed.isEmpty()) {
            std::cerr << "Failed to decompress incoming message" << std::endl;
            return;
        }
        decodedContent = QString::fromUtf8(decompressed);
    } else {
        decodedContent = content;
    }

    emit messageReceived(channel, sender, decodedContent, QDateTime::currentDateTime());
}

void Text::requestHistory(const QString &channel, int limit, int before) {
    json j;
    j["type"] = "history_request";
    j["channel"] = channel.toStdString();
    j["limit"] = limit;
    j["before"] = before;
    networkManager.sendTextMessage(QString::fromStdString(j.dump()));
}

void Text::handleHistoryResponse(const QString &channel, const QJsonArray &messages) {
    QList<Message> history;

    for (const auto &msgVal : messages) {
        QJsonObject obj = msgVal.toObject();
        QString content = obj["content"].toString();
        bool compressed = obj["compressed"].toBool(false);

        QString decodedContent;
        if (compressed) {
            QByteArray compressedData = fromBase64(content);
            QByteArray decompressed = decompress(compressedData);
            if (decompressed.isEmpty()) {
                std::cerr << "Failed to decompress history message" << std::endl;
                continue;
            }
            decodedContent = QString::fromUtf8(decompressed);
        } else {
            decodedContent = content;
        }

        Message msg;
        msg.id = obj["id"].toInt();
        msg.sender = obj["user"].toString();
        msg.content = decodedContent;
        msg.timestamp = QDateTime::fromString(obj["timestamp"].toString(), Qt::ISODate).toLocalTime();
        history.append(msg);
    }

    emit historyReceived(channel, history);
}
