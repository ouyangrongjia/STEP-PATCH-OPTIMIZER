#pragma once

#include <QWidget>

class QPlainTextEdit;

namespace spo {

class LogPanel final : public QWidget {
public:
    explicit LogPanel(QWidget* parent = nullptr);
    void appendInfo(const QString& message);
    void appendWarning(const QString& message);
    void appendError(const QString& message);

private:
    QPlainTextEdit* log_ = nullptr;
};

}
