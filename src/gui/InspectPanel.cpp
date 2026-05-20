#include "gui/InspectPanel.h"

#include <QHeaderView>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QVBoxLayout>

namespace spo {

InspectPanel::InspectPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    properties_ = new QTableWidget(0, 2, this);
    properties_->setHorizontalHeaderLabels({"属性", "值"});
    properties_->horizontalHeader()->setStretchLastSection(true);
    properties_->verticalHeader()->setVisible(false);
    properties_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(properties_);

    validation_ = new QPlainTextEdit(this);
    validation_->setReadOnly(true);
    validation_->setPlainText("尚未运行合法性检查。");

    report_ = new QPlainTextEdit(this);
    report_->setReadOnly(true);
    report_->setPlainText("尚无报告。");

    showProperties("选择", {{"当前对象", "无"}});
}

QWidget* InspectPanel::validationWidget() const {
    return validation_;
}

QWidget* InspectPanel::reportWidget() const {
    return report_;
}

void InspectPanel::showProperties(const QString& title, const QList<QPair<QString, QString>>& rows) {
    properties_->setRowCount(0);
    properties_->setWindowTitle(title);
    for (const auto& rowData : rows) {
        const auto row = properties_->rowCount();
        properties_->insertRow(row);
        properties_->setItem(row, 0, new QTableWidgetItem(rowData.first));
        properties_->setItem(row, 1, new QTableWidgetItem(rowData.second));
    }
}

void InspectPanel::showValidation(const QString& text) {
    validation_->setPlainText(text);
}

void InspectPanel::showReport(const QString& text) {
    report_->setPlainText(text);
}

}
