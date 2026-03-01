#include "ChatWidget.h"
#include <qpushbutton.h>

#include <utility>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QApplication>
#include <QRegularExpression>

#include "core/Settings.h"

using S = Settings::Settings;

ChatWidget::ChatWidget(QWidget *parent)
  : QWidget(parent), networkManager(new QNetworkAccessManager(this))
{
  setupUI();

  QJsonObject systemMessage;
  systemMessage["role"] = "system";
  systemMessage["content"] =
    "You are a helpful and friendly assistant with knowledge about the OpenSCAD application "
    "and it's scripting language. You always answer truthfully and concise. If possible you "
    "provide short example scripts the user can run. If you do not find clear information "
    "about the question you have been asked, then you answer that you don't know the answer "
    "to the question.";
  conversationHistory.append(systemMessage);
}

void ChatWidget::setContextProviders(std::function<QString()> getFileName,
                                     std::function<QString()> getSelection,
                                     std::function<QString()> getDocumentText,
                                     std::function<void()> triggerPreview,
                                     std::function<void(const QString&, bool)> applyCode)
{
  getFileName_ = std::move(getFileName);
  getSelection_ = std::move(getSelection);
  getDocumentText_ = std::move(getDocumentText);
  triggerPreview_ = std::move(triggerPreview);
  applyCode_ = std::move(applyCode);
}

void ChatWidget::setupUI()
{
  mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);

  // Chat messages area
  scrollArea = new QScrollArea(this);
  scrollArea->setWidgetResizable(true);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  messagesWidget = new QWidget();
  messagesLayout = new QVBoxLayout(messagesWidget);
  messagesLayout->setContentsMargins(5, 5, 5, 5);
  messagesLayout->setSpacing(10);
  messagesLayout->addStretch();

  scrollArea->setWidget(messagesWidget);
  mainLayout->addWidget(scrollArea);

  auto *ctxLayout = new QHBoxLayout();
  includeScriptCheck = new QCheckBox("Include script", this);
  selectionOnlyCheck = new QCheckBox("Selection only", this);
  previewButton = new QPushButton("Preview", this);
  previewButton->setMaximumWidth(80);
  applyButton = new QPushButton("Apply", this);
  applyButton->setMaximumWidth(80);
  applyButton->setEnabled(false); // enable only when we have code

  includeScriptCheck->setChecked(true);

  ctxLayout->addWidget(includeScriptCheck);
  ctxLayout->addWidget(selectionOnlyCheck);
  ctxLayout->addStretch(1);
  ctxLayout->addWidget(applyButton);
  ctxLayout->addWidget(previewButton);
  mainLayout->addLayout(ctxLayout);

  connect(previewButton, &QPushButton::clicked, this, [this]() {
    if (triggerPreview_) {
      triggerPreview_();
      addMessage("Preview requested.", false);
    } else {
      addMessage("Preview not available.", false)   ;
    }
  });

  connect(applyButton, &QPushButton::clicked, this, [this]() {
    if (lastSuggestedCode_.trimmed().isEmpty()) {
      addMessage("❌ No code to apply (ask the assistant to return a ```scad``` code block).", false);
      return;
    }
    if (!applyCode_) {
      addMessage("❌ Apply is not wired to the editor.", false);
      return;
    }

    const bool wantSelection = (selectionOnlyCheck && selectionOnlyCheck->isChecked());

    // If user wants selection, but selection is empty, ask what to do
    if (wantSelection && getSelection_ && getSelection_().trimmed().isEmpty()) {
      auto ret = QMessageBox::question(
        this,
        "No selection",
        "Selection-only is enabled, but nothing is selected.\n\nApply to the entire document instead?",
        QMessageBox::Yes | QMessageBox::No
      );
      if (ret != QMessageBox::Yes) return;
      // fall back to full document apply
      applyCode_(lastSuggestedCode_, false);
      addMessage("✅ Applied to entire document. Click Preview to see the result.", false);
      return;
    }

    const QString target = wantSelection ? "the selected text" : "the entire document";
    auto ret = QMessageBox::question(
      this,
      "Apply AI suggestion",
      "Replace " + target + " with the AI suggested code?",
      QMessageBox::Yes | QMessageBox::No
    );
    if (ret != QMessageBox::Yes) return;

    applyCode_(lastSuggestedCode_, wantSelection);
    addMessage("✅ Applied to editor. Click Preview to see the result.", false);
  });

  // Input area
  auto *inputLayout = new QHBoxLayout();
  inputField = new QLineEdit(this);
  inputField->setPlaceholderText("Ask GPT about OpenSCAD...");

  sendButton = new QPushButton("Send", this);
  sendButton->setMaximumWidth(60);

  inputLayout->addWidget(inputField);
  inputLayout->addWidget(sendButton);
  mainLayout->addLayout(inputLayout);

  // Connect signals
  connect(sendButton, &QPushButton::clicked, this, &ChatWidget::sendMessage);
  connect(inputField, &QLineEdit::returnPressed, this, &ChatWidget::sendMessage);

  // Welcome message
  addMessage(
    "💬 Welcome to OpenSCAD Chat! Ask me anything about 3D modeling, OpenSCAD syntax, or get help with "
    "your designs.",
    false);
}

void ChatWidget::sendMessage()
{
  QString message = inputField->text().trimmed();
  if (message.isEmpty()) return;

  inputField->clear();
  addMessage(message, true);

  // reset Apply state for this new round
  lastSuggestedCode_.clear();
  if (applyButton) applyButton->setEnabled(false);

  // Persist ONLY the user's typed message in conversation history
  QJsonObject userMessage;
  userMessage["role"] = "user";
  userMessage["content"] = message;
  conversationHistory.append(userMessage);

  // Build request messages: history WITHOUT the last user message, then context, then last user message
  QJsonArray messages = conversationHistory;
  messages.removeLast(); // remove the just-appended userMessage from the request temporarily

  const QString ctx = buildContextBlock();
  if (!ctx.isEmpty()) {
    QJsonObject ctxMsg;
    ctxMsg["role"] = "user";
    ctxMsg["content"] = ctx;
    messages.append(ctxMsg);    // ephemeral
  }

  messages.append(userMessage); // actual user question goes last

  // Prepare API request
  QJsonObject requestData;
  requestData["model"] = QString::fromStdString(S::aiModel.value());
  requestData["messages"] = messages;
  requestData["temperature"] = 0.7;

  QJsonDocument doc(requestData);
  QByteArray jsonData = doc.toJson();

  // Robust URL join (handles trailing slash)
  QUrl base(QString::fromStdString(S::aiApiUrl.value()));
  if (base.scheme().isEmpty()) {
    addMessage("❌ AI API URL is invalid (must start with http:// or https://)", false);
    return;
  }
  if (!base.path().endsWith('/')) base.setPath(base.path() + '/');
  QUrl url = base.resolved(QUrl("chat/completions"));

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  const auto apiKey = QString::fromStdString(S::aiApiKey.value());
  if (!apiKey.isEmpty()) {
    request.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
  }

  QNetworkReply *reply = networkManager->post(request, jsonData);
  connect(reply, &QNetworkReply::finished, this, &ChatWidget::onApiResponse);
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
  connect(reply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error), this,
          &ChatWidget::onApiError);
#else
  connect(reply, &QNetworkReply::errorOccurred, this, &ChatWidget::onApiError);
#endif

  addMessage("🤔 Thinking...", false);
  sendButton->setEnabled(false);
}

void ChatWidget::onApiResponse()
{
  auto *reply = qobject_cast<QNetworkReply *>(sender());
  if (!reply) return;

  sendButton->setEnabled(true);

  // Remove "Thinking..." message
  if (messagesLayout->count() > 1) {
    QLayoutItem *item = messagesLayout->takeAt(messagesLayout->count() - 2);
    if (item && item->widget()) {
      item->widget()->deleteLater();
    }
    delete item;
  }

  if (reply->error() == QNetworkReply::NoError) {
    QByteArray response = reply->readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);

    if (parseError.error == QJsonParseError::NoError) {
      QJsonObject obj = doc.object();
      QJsonArray choices = obj["choices"].toArray();

      if (!choices.isEmpty()) {
        QJsonObject choice = choices[0].toObject();
        QJsonObject message = choice["message"].toObject();
        QString content = message["content"].toString();

        addMessage(content, false);

        // Extract code block (```scad ... ```) from assistant and enable Apply if found
        lastSuggestedCode_ = extractSuggestedCode(content);
        if (applyButton) applyButton->setEnabled(!lastSuggestedCode_.trimmed().isEmpty());

        // Add assistant response to conversation history
        QJsonObject assistantMessage;
        assistantMessage["role"] = "assistant";
        assistantMessage["content"] = content;
        conversationHistory.append(assistantMessage);
      } else {
        addMessage("❌ No response from API", false);
      }
    } else {
      addMessage("❌ Failed to parse API response", false);
    }
  } else {
    addMessage(QString("❌ API Error: %1").arg(reply->errorString()), false);
  }

  reply->deleteLater();
}

void ChatWidget::onApiError(QNetworkReply::NetworkError error)
{
  Q_UNUSED(error)
  sendButton->setEnabled(true);

  // Remove "Thinking..." message
  if (messagesLayout->count() > 1) {
    QLayoutItem *item = messagesLayout->takeAt(messagesLayout->count() - 2);
    if (item && item->widget()) {
      item->widget()->deleteLater();
    }
    delete item;
  }

  lastSuggestedCode_.clear();
  if (applyButton) applyButton->setEnabled(false);
  addMessage("❌ Network error occurred", false);
}

void ChatWidget::addMessage(const QString& message, bool isUser)
{
  auto *messageLabel = new QLabel(formatMessage(message, isUser));
  messageLabel->setWordWrap(true);
  messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  messageLabel->setMargin(8);

  if (isUser) {
    messageLabel->setStyleSheet(
      "QLabel { "
      "background-color: #007acc; "
      "color: white; "
      "border-radius: 10px; "
      "padding: 8px; "
      "margin-left: 50px; "
      "}");
    messageLabel->setAlignment(Qt::AlignRight);
  } else {
    messageLabel->setStyleSheet(
      "QLabel { "
      "background-color: #f0f0f0; "
      "color: black; "
      "border-radius: 10px; "
      "padding: 8px; "
      "margin-right: 50px; "
      "}");
    messageLabel->setAlignment(Qt::AlignLeft);
  }

  // Insert before the stretch
  messagesLayout->insertWidget(messagesLayout->count() - 1, messageLabel);
  scrollToBottom();
}

void ChatWidget::scrollToBottom()
{
  QApplication::processEvents();
  scrollArea->verticalScrollBar()->setValue(scrollArea->verticalScrollBar()->maximum());
}

QString ChatWidget::clampText(const QString &s, int maxChars) {
  if (s.size() <= maxChars) return s;
  return s.left(maxChars) + "\n\n[...truncated...]\n";
}

QString ChatWidget::buildContextBlock() const
{
  if (!includeScriptCheck || !includeScriptCheck->isChecked()) return {};
  if (!getDocumentText_) return {};

  QString code;
  if (selectionOnlyCheck && selectionOnlyCheck->isChecked() && getSelection_) {
    code = getSelection_().trimmed();
  }
  if (code.isEmpty()) code = getDocumentText_().trimmed();
  if (code.isEmpty()) return {};

  code = clampText(code, 12000);

  QString fileName = getFileName_ ? getFileName_() : "Untitled.scad";

  return QString("[OpenSCAD Context]\nFile: %1\n\n```scad\n%2\n```\n")
    .arg(fileName, code);
}

QString ChatWidget::extractSuggestedCode(const QString& assistantText)
{
  // 1) Prefer ```scad ... ```
  {
    QRegularExpression re(R"(```\s*scad\s*\n(.*?)\n```)",
                          QRegularExpression::DotMatchesEverythingOption |
                          QRegularExpression::CaseInsensitiveOption);
    auto m = re.match(assistantText);
    if (m.hasMatch()) return m.captured(1).trimmed();
  }

  // 2) Fallback: any ```lang? ... ```
  {
    QRegularExpression re(R"(```\s*[a-zA-Z0-9_-]*\s*\n(.*?)\n```)",
                          QRegularExpression::DotMatchesEverythingOption);
    auto m = re.match(assistantText);
    if (m.hasMatch()) return m.captured(1).trimmed();
  }

  return {};
}

QString ChatWidget::formatMessage(const QString& text, bool isUser)
{
  QString prefix = isUser ? "You: " : "GPT: ";
  return prefix + text;
}
