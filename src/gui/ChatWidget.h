#pragma once

#include <qcheckbox.h>
#include <qpushbutton.h>
#include <QWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScrollArea>
#include <QLabel>
#include <QCheckBox>
#include <functional>

class ChatWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ChatWidget(QWidget *parent = nullptr);
  ~ChatWidget() override = default;

  // callbacks supplied by MainWindow
  void setContextProviders(std::function<QString()> getFileName,
                           std::function<QString()> getSelection,
                           std::function<QString()> getDocumentText,
                           std::function<void()> triggerPreview);

private slots:
  void sendMessage();
  void onApiResponse();
  void onApiError(QNetworkReply::NetworkError error);

private:
  void setupUI();
  void addMessage(const QString& message, bool isUser);
  void scrollToBottom();
  QString formatMessage(const QString& text, bool isUser);

  QVBoxLayout *mainLayout;
  QScrollArea *scrollArea;
  QWidget *messagesWidget;
  QVBoxLayout *messagesLayout;
  QLineEdit *inputField;
  QPushButton *sendButton;
  QNetworkAccessManager *networkManager;

  QJsonArray conversationHistory;

  // context providers
  std::function<QString()> getFileName_;
  std::function<QString()> getSelection_;
  std::function<QString()> getDocumentText_;
  std::function<void()> triggerPreview_;

  QCheckBox *includeScriptCheck = nullptr;
  QCheckBox *selectionOnlyCheck = nullptr;
  QPushButton *previewButton = nullptr;

  QString buildContextBlock() const;
  static QString clampText(const QString &s, int maxChars);
};
