#pragma once

#include "emote_manager.hpp"
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QTextEdit>
#include <QVBoxLayout>

class EmoteCompleter : public QFrame {
    Q_OBJECT

  public:
    EmoteCompleter(EmoteManager &emoteManager, QWidget *parent = nullptr);
    ~EmoteCompleter() = default;

    void attachToInput(QTextEdit *input);

  protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

  private slots:
    void onTextChanged();
    void onItemClicked(QListWidgetItem *item);

  private:
    EmoteManager &emoteManager;
    QTextEdit *inputWidget = nullptr;
    QListWidget *listWidget;
    QVBoxLayout *layout;

    void updateCompletions(const QString &query);
    void insertCompletion(int index);
    QString extractEmoteQuery() const;
    int findColonStart() const;
    void positionPopup();
};
