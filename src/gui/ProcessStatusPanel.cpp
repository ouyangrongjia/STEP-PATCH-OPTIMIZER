#include "gui/ProcessStatusPanel.h"

#include <QHeaderView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace spo {

namespace {

QString pathText(const std::filesystem::path& path) {
    return path.empty() ? QString("none") : QString::fromStdWString(path.wstring());
}

QString boolText(bool value) {
    return value ? QString("true") : QString("false");
}

}

ProcessStatusPanel::ProcessStatusPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    table_ = new QTableWidget(0, 2, this);
    table_->setHorizontalHeaderLabels({"字段", "值"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table_);

    showStatus(makeProcessStatus(ProcessStage::Idle, "Idle."));
}

void ProcessStatusPanel::showStatus(const ProcessStatusSnapshot& snapshot) {
    setRows({
        {"stage", QString::fromLatin1(toString(snapshot.stage))},
        {"candidate id", snapshot.candidateId >= 0 ? QString::number(snapshot.candidateId) : QString("none")},
        {"source face count", QString::number(snapshot.sourceFaceCount)},
        {"boundary edge count", QString::number(snapshot.boundaryEdgeCount)},
        {"local STL", pathText(snapshot.localStlPath)},
        {"patch STEP", pathText(snapshot.patchStepPath)},
        {"patch IGS", pathText(snapshot.patchIgesPath)},
        {"fit_region log", pathText(snapshot.fitRegionLogPath)},
        {"patch preview run log", pathText(snapshot.patchPreviewRunLogPath)},
        {"selected sewing tolerance", QString::number(snapshot.selectedSewingTolerance, 'g', 8)},
        {"sewing attempt index", QString::number(snapshot.sewingAttemptIndex)},
        {"sewing attempt count", QString::number(snapshot.sewingAttemptCount)},
        {"best sewing free edges", QString::number(snapshot.bestFreeEdges)},
        {"best sewing multiple edges", QString::number(snapshot.bestMultipleEdges)},
        {"best sewing face count", QString::number(snapshot.bestFaceCount)},
        {"best sewing edge count", QString::number(snapshot.bestEdgeCount)},
        {"best sewing shell count", QString::number(snapshot.bestShellCount)},
        {"best sewing solid count", QString::number(snapshot.bestSolidCount)},
        {"best sewing BRepCheck", boolText(snapshot.bestBRepCheckValid)},
        {"repair run count", QString::number(snapshot.repairRunCount)},
        {"repair applied", boolText(snapshot.repairApplied)},
        {"adaptive sewing applied", boolText(snapshot.adaptiveSewingApplied)},
        {"StrictTopologyGate evaluated", boolText(snapshot.gateEvaluated)},
        {"StrictTopologyGate passed", boolText(snapshot.gatePassed)},
        {"StrictTopologyGate failure", QString::fromStdString(snapshot.latestGateFailureReason)},
        {"Gate before BRepCheck", boolText(snapshot.gateBeforeBRepCheckValid)},
        {"Gate after BRepCheck", boolText(snapshot.gateAfterBRepCheckValid)},
        {"Gate roundtrip BRepCheck", boolText(snapshot.gateRoundtripBRepCheckValid)},
        {"Gate before free edges", QString::number(snapshot.gateBeforeFreeEdges)},
        {"Gate after free edges", QString::number(snapshot.gateAfterFreeEdges)},
        {"Gate roundtrip free edges", QString::number(snapshot.gateRoundtripFreeEdges)},
        {"Gate before multiple edges", QString::number(snapshot.gateBeforeMultipleEdges)},
        {"Gate after multiple edges", QString::number(snapshot.gateAfterMultipleEdges)},
        {"Gate roundtrip multiple edges", QString::number(snapshot.gateRoundtripMultipleEdges)},
        {"Gate before faces/edges/shells/solids", QString("%1/%2/%3/%4")
            .arg(snapshot.gateBeforeFaceCount)
            .arg(snapshot.gateBeforeEdgeCount)
            .arg(snapshot.gateBeforeShellCount)
            .arg(snapshot.gateBeforeSolidCount)},
        {"Gate after faces/edges/shells/solids", QString("%1/%2/%3/%4")
            .arg(snapshot.gateAfterFaceCount)
            .arg(snapshot.gateAfterEdgeCount)
            .arg(snapshot.gateAfterShellCount)
            .arg(snapshot.gateAfterSolidCount)},
        {"Gate roundtrip faces/edges/shells/solids", QString("%1/%2/%3/%4")
            .arg(snapshot.gateRoundtripFaceCount)
            .arg(snapshot.gateRoundtripEdgeCount)
            .arg(snapshot.gateRoundtripShellCount)
            .arg(snapshot.gateRoundtripSolidCount)},
        {"crop band evaluated", boolText(snapshot.cropBoundaryBandEvaluated)},
        {"crop band sample count", QString::number(snapshot.cropBoundaryBandSampleCount)},
        {"crop band missing points", QString::number(snapshot.cropBoundaryBandMissingPointCount)},
        {"crop band max distance", QString::number(snapshot.cropBoundaryBandMaxDistance, 'g', 8)},
        {"crop triangle audit evaluated", boolText(snapshot.cropSourceTriangleAuditEvaluated)},
        {"crop rejected near-boundary triangles", QString::number(snapshot.cropRejectedNearBoundaryTriangleCount)},
        {"crop conservative keep candidates", QString::number(snapshot.cropConservativeKeepCandidateCount)},
        {"message", QString::fromStdString(snapshot.latestMessage)},
        {"warning", QString::fromStdString(snapshot.latestWarning)}
    });
}

void ProcessStatusPanel::setRows(const QList<QPair<QString, QString>>& rows) {
    table_->setRowCount(0);
    for (const auto& rowData : rows) {
        const auto row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(rowData.first));
        table_->setItem(row, 1, new QTableWidgetItem(rowData.second));
    }
}

}
