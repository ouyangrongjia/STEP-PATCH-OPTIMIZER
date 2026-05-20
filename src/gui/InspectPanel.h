#pragma once

#include <QWidget>

class QPlainTextEdit;
class QTableWidget;

namespace spo {

class InspectPanel final : public QWidget {
public:
    explicit InspectPanel(QWidget* parent = nullptr);
    QWidget* validationWidget() const;
    QWidget* reportWidget() const;
    void showProperties(const QString& title, const QList<QPair<QString, QString>>& rows);
    void showValidation(const QString& text);
    void showReport(const QString& text);

private:
    QTableWidget* properties_ = nullptr;
    QPlainTextEdit* validation_ = nullptr;
    QPlainTextEdit* report_ = nullptr;
};

}
