#include "gui/LogPanel.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace spo {

LogPanel::LogPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
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
