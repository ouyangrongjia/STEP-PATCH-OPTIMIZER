#include "gui/LogPanel.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QFontDatabase>

namespace spo {

LogPanel::LogPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(0);
    log_ = new QPlainTextEdit(this);
    log_->setObjectName("logText");
    log_->setReadOnly(true);
    log_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(log_);
}

void LogPanel::appendInfo(const QString& message) {
    log_->appendPlainText(QString("[信息] %1").arg(message));
}

void LogPanel::appendWarning(const QString& message) {
    log_->appendPlainText(QString("[警告] %1").arg(message));
}

void LogPanel::appendError(const QString& message) {
    log_->appendPlainText(QString("[错误] %1").arg(message));
}

}
