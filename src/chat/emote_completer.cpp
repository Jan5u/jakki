#include "emote_completer.hpp"
#include <QKeyEvent>
#include <QScrollBar>
#include <QTextCursor>
#include <iostream>

EmoteCompleter::EmoteCompleter(EmoteManager &emoteManager, QWidget *parent) : QFrame(parent), emoteManager(emoteManager) {
    setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, true);

    layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    listWidget = new QListWidget(this);
    listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    listWidget->setMaximumHeight(250);
    listWidget->setMinimumWidth(250);
    listWidget->setIconSize(QSize(20, 20));
    listWidget->setUniformItemSizes(true);
    listWidget->setFocusPolicy(Qt::NoFocus);

    layout->addWidget(listWidget);

    connect(listWidget, &QListWidget::itemClicked, this, &EmoteCompleter::onItemClicked);

    hide();
}

void EmoteCompleter::attachToInput(QTextEdit *input) {
    if (inputWidget) {
        inputWidget->removeEventFilter(this);
        QObject::disconnect(inputWidget, &QTextEdit::textChanged, this, &EmoteCompleter::onTextChanged);
    }

    inputWidget = input;
    if (inputWidget) {
        inputWidget->installEventFilter(this);
        connect(inputWidget, &QTextEdit::textChanged, this, &EmoteCompleter::onTextChanged);
    }
}

int EmoteCompleter::findColonStart() const {
    if (!inputWidget)
        return -1;

    QTextCursor cursor = inputWidget->textCursor();
    int cursorPos = cursor.position();
    QString text = inputWidget->toPlainText();

    for (int i = cursorPos - 1; i >= 0; --i) {
        if (text[i] == ':') {
            bool isMatched = false;
            for (int j = i + 1; j < cursorPos; ++j) {
                if (text[j] == ':') {
                    isMatched = true;
                    break;
                }
            }
            if (!isMatched) {
                return i;
            }
            return -1;
        }
        if (text[i] == ' ' || text[i] == '\n' || text[i] == '\t') {
            return -1;
        }
    }
    return -1;
}

QString EmoteCompleter::extractEmoteQuery() const {
    if (!inputWidget)
        return {};

    int colonPos = findColonStart();
    if (colonPos < 0)
        return {};

    QTextCursor cursor = inputWidget->textCursor();
    int cursorPos = cursor.position();
    QString text = inputWidget->toPlainText();

    // Extract the text after the colon
    QString query = text.mid(colonPos + 1, cursorPos - colonPos - 1);

    // Must have at least 1 character after the colon
    if (query.isEmpty())
        return {};

    // Only allow alphanumeric and underscore
    static QRegularExpression validChars("^[a-zA-Z0-9_]+$");
    if (!validChars.match(query).hasMatch())
        return {};

    return query;
}

void EmoteCompleter::onTextChanged() {
    QString query = extractEmoteQuery();
    if (query.isEmpty()) {
        hide();
        return;
    }
    updateCompletions(query);
}

void EmoteCompleter::updateCompletions(const QString &query) {
    auto results = emoteManager.search(query, 8);

    listWidget->clear();

    if (results.isEmpty()) {
        hide();
        return;
    }

    for (const auto &result : results) {
        auto *item = new QListWidgetItem();
        if (result.isCustom && !result.iconPath.isEmpty()) {
            item->setIcon(QIcon(result.iconPath));
            item->setText(":" + result.name + ":");
        } else {
            // Unicode emoji: show the emoji character + name
            item->setText(result.displayText + "  :" + result.name + ":");
        }
        item->setData(Qt::UserRole, result.name);
        item->setData(Qt::UserRole + 1, result.isCustom);
        item->setData(Qt::UserRole + 2, result.displayText);
        listWidget->addItem(item);
    }

    listWidget->setCurrentRow(0);

    // Size the widget to fit content
    int itemHeight = listWidget->sizeHintForRow(0);
    int visibleItems = qMin(results.size(), 8);
    int listHeight = itemHeight * visibleItems + 4;
    listWidget->setFixedHeight(listHeight);
    adjustSize();

    positionPopup();
    show();
    raise();
}

void EmoteCompleter::positionPopup() {
    if (!inputWidget || !parentWidget())
        return;

    QPoint inputPos = inputWidget->mapTo(parentWidget(), QPoint(0, 0));
    int popupX = inputPos.x();
    int popupY = inputPos.y() - height();

    if (popupY < 0) {
        popupY = inputPos.y() + inputWidget->height();
    }

    move(popupX, popupY);
}

void EmoteCompleter::insertCompletion(int index) {
    if (!inputWidget || index < 0 || index >= listWidget->count())
        return;

    auto *item = listWidget->item(index);
    QString name = item->data(Qt::UserRole).toString();
    bool isCustom = item->data(Qt::UserRole + 1).toBool();
    QString displayText = item->data(Qt::UserRole + 2).toString();

    int colonPos = findColonStart();
    if (colonPos < 0)
        return;

    QTextCursor cursor = inputWidget->textCursor();
    int cursorPos = cursor.position();

    cursor.setPosition(colonPos);
    cursor.setPosition(cursorPos, QTextCursor::KeepAnchor);

    if (isCustom) {
        cursor.insertText(":" + name + ": ");
    } else {
        cursor.insertText(displayText + " ");
    }

    hide();
}

void EmoteCompleter::onItemClicked(QListWidgetItem *item) {
    int row = listWidget->row(item);
    insertCompletion(row);
}

bool EmoteCompleter::eventFilter(QObject *obj, QEvent *event) {
    if (obj != inputWidget)
        return QFrame::eventFilter(obj, event);

    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);

        if (isVisible()) {
            switch (keyEvent->key()) {
            case Qt::Key_Up: {
                int current = listWidget->currentRow();
                if (current > 0) {
                    listWidget->setCurrentRow(current - 1);
                }
                return true;
            }
            case Qt::Key_Down: {
                int current = listWidget->currentRow();
                if (current < listWidget->count() - 1) {
                    listWidget->setCurrentRow(current + 1);
                }
                return true;
            }
            case Qt::Key_Tab:
            case Qt::Key_Return:
            case Qt::Key_Enter: {
                if (listWidget->currentRow() >= 0) {
                    insertCompletion(listWidget->currentRow());
                    return true;
                }
                break;
            }
            case Qt::Key_Escape:
                hide();
                return true;
            default:
                break;
            }
        }

        if (!isVisible()) {
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                if (!(keyEvent->modifiers() & Qt::ShiftModifier)) {
                    return false;
                }
            }
        }
    }

    if (event->type() == QEvent::FocusOut && isVisible()) {
        hide();
    }

    return QFrame::eventFilter(obj, event);
}
