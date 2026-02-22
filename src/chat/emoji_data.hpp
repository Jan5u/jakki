#pragma once

#include <QList>
#include <QString>

struct EmojiEntry {
    QString name;
    QString unicode;
    QString category;
};

const QList<EmojiEntry> &getEmojiDatabase();
