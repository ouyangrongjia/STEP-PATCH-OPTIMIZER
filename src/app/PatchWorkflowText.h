#pragma once

#include "app/ProcessStatus.h"

#include <QString>
#include <QStringList>

namespace spo::patch_workflow_text {

QString progressReport(
    const QString& header,
    const ProcessStatusSnapshot& lastProgress,
    const QStringList& progressLines,
    qint64 elapsedMilliseconds);

QString progressLine(
    const ProcessStatusSnapshot& status,
    qint64 elapsedMilliseconds);

QString statusText(
    const QString& header,
    const ProcessStatusSnapshot& lastProgress,
    qint64 elapsedMilliseconds);

}
