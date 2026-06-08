#pragma once

#include "app/ProcessStatus.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class QTableWidget;

namespace spo {

class ProcessStatusPanel final : public QWidget {
public:
    explicit ProcessStatusPanel(QWidget* parent = nullptr);
    void showStatus(const ProcessStatusSnapshot& snapshot);

private:
    void setRows(const QList<QPair<QString, QString>>& rows);

    QTableWidget* table_ = nullptr;
};

}
