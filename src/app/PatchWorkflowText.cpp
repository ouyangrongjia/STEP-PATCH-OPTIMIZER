#include "app/PatchWorkflowText.h"

#include <QLatin1Char>

namespace spo::patch_workflow_text {

namespace {

QString elapsedText(const qint64 milliseconds) {
    const auto seconds = milliseconds / 1000;
    return QString("%1:%2")
        .arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString pathText(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

}

QString progressReport(
    const QString& header,
    const ProcessStatusSnapshot& lastProgress,
    const QStringList& progressLines,
    const qint64 elapsedMilliseconds) {
    QStringList lines;
    if (!header.isEmpty()) {
        lines << header;
    }

    lines << QString("心跳：running  |  elapsed %1  |  last stage %2")
        .arg(elapsedText(elapsedMilliseconds))
        .arg(QString::fromLatin1(toString(lastProgress.stage)));
    if (!lastProgress.latestMessage.empty()) {
        lines << QString("最近消息：%1").arg(
            QString::fromStdString(lastProgress.latestMessage));
    }
    if (!lastProgress.latestWarning.empty()) {
        lines << QString("最近警告：%1").arg(
            QString::fromStdString(lastProgress.latestWarning));
    }

    lines << "";
    lines << "阶段事件";
    lines << progressLines;
    return lines.join('\n');
}

QString progressLine(
    const ProcessStatusSnapshot& status,
    const qint64 elapsedMilliseconds) {
    QStringList fields;
    fields << QString("[%1]").arg(elapsedText(elapsedMilliseconds));
    fields << QString::fromLatin1(toString(status.stage));
    if (status.candidateId >= 0) {
        fields << QString("candidate=%1").arg(status.candidateId);
    }
    if (status.sourceFaceCount > 0) {
        fields << QString("faces=%1").arg(status.sourceFaceCount);
    }
    if (status.boundaryEdgeCount > 0) {
        fields << QString("boundary_edges=%1").arg(status.boundaryEdgeCount);
    }
    if (status.targetFaceCount > 0) {
        fields << QString("target_faces=%1").arg(status.targetFaceCount);
    }
    if (status.currentFaceCount > 0) {
        fields << QString("current_faces=%1").arg(status.currentFaceCount);
    }
    if (status.searchIteration > 0) {
        fields << QString("search_layer=%1").arg(status.searchIteration);
    }
    if (status.maximumFitCount > 0) {
        fields << QString("fits=%1/%2")
            .arg(status.completedFitCount)
            .arg(status.maximumFitCount);
    }
    if (status.candidateValidationCount > 0) {
        fields << QString("validation=%1/%2")
            .arg(status.candidateValidationIndex)
            .arg(status.candidateValidationCount);
    }
    if (!status.activeGroup.empty()) {
        fields << QString("group=%1").arg(QString::fromStdString(status.activeGroup));
    }
    if (!status.localStlPath.empty()) {
        fields << QString("local STL=%1").arg(pathText(status.localStlPath));
    }
    if (!status.patchStepPath.empty()) {
        fields << QString("patch STEP=%1").arg(pathText(status.patchStepPath));
    }
    if (!status.fitRegionLogPath.empty()) {
        fields << QString("fit_region log=%1").arg(pathText(status.fitRegionLogPath));
    }
    if (!status.patchPreviewRunLogPath.empty()) {
        fields << QString("run log=%1").arg(pathText(status.patchPreviewRunLogPath));
    }
    if (!status.latestMessage.empty()) {
        fields << QString("message=%1").arg(QString::fromStdString(status.latestMessage));
    }
    if (!status.latestWarning.empty()) {
        fields << QString("warning=%1").arg(QString::fromStdString(status.latestWarning));
    }
    return fields.join("  |  ");
}

QString statusText(
    const QString& header,
    const ProcessStatusSnapshot& lastProgress,
    const qint64 elapsedMilliseconds) {
    QString statusPrefix = "Patch 预览";
    if (header.startsWith("Patch Apply")) {
        statusPrefix = "Patch Apply";
    } else if (header.startsWith("STEP/STP") ||
               header.startsWith("STEP ")) {
        statusPrefix = "STEP/STP";
    } else if (header.startsWith("合并候选")) {
        statusPrefix = "合并候选";
    } else if (header.startsWith("特征边")) {
        statusPrefix = "特征边";
    } else if (header.startsWith("原生重拟合")) {
        statusPrefix = "原生重拟合";
    }

    QStringList fields;
    fields << statusPrefix;
    fields << QString::fromLatin1(toString(lastProgress.stage));
    fields << QString("elapsed %1").arg(elapsedText(elapsedMilliseconds));
    if (lastProgress.candidateId >= 0) {
        fields << QString("candidate %1").arg(lastProgress.candidateId);
    }
    if (!lastProgress.latestMessage.empty()) {
        fields << QString::fromStdString(lastProgress.latestMessage);
    } else if (!lastProgress.latestWarning.empty()) {
        fields << QString("warning: %1").arg(
            QString::fromStdString(lastProgress.latestWarning));
    }
    return fields.join(" | ");
}

}
