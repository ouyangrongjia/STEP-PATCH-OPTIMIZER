#include "gui/ModelTreePanel.h"

#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace spo {

ModelTreePanel::ModelTreePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabel("模型");
    tree_->header()->setStretchLastSection(true);
    layout->addWidget(tree_);

    showEmpty();
}

void ModelTreePanel::showEmpty() {
    tree_->clear();
    auto* root = new QTreeWidgetItem(tree_, QStringList() << "未加载 STEP/STP");
    new QTreeWidgetItem(root, QStringList() << "请通过 文件 > 打开 STEP/STP 导入模型");
    tree_->expandAll();
}

void ModelTreePanel::showDocument(
    const ShapeDocument& document,
    int lockedEdgeCount,
    int mergeCandidateCount,
    int visibleMergeCandidateCount,
    int pendingCandidateCount,
    int acceptedCandidateCount,
    int rejectedCandidateCount,
    int hiddenCandidateCount,
    int currentMergeCandidateId,
    const CandidateTypeCounts* candidateTypeCounts,
    const FeatureEdgeDetectionResult* featureEdges) {
    tree_->clear();
    const auto& stats = document.stats();

    auto* root = new QTreeWidgetItem(tree_, QStringList() << QString::fromUtf8(document.displayName().c_str()));
    auto* solids = new QTreeWidgetItem(root, QStringList() << QString("实体 (%1)").arg(stats.solids));
    for (int i = 0; i < stats.solids; ++i) {
        new QTreeWidgetItem(solids, QStringList() << QString("实体 %1").arg(i));
    }

    auto* shells = new QTreeWidgetItem(root, QStringList() << QString("壳 (%1)").arg(stats.shells));
    for (int i = 0; i < stats.shells; ++i) {
        new QTreeWidgetItem(shells, QStringList() << QString("壳 %1").arg(i));
    }

    auto* faces = new QTreeWidgetItem(root, QStringList() << QString("面 (%1)").arg(stats.faces));
    new QTreeWidgetItem(faces, QStringList() << "面索引已就绪");

    auto* edges = new QTreeWidgetItem(root, QStringList() << QString("边 (%1)").arg(stats.edges));
    new QTreeWidgetItem(edges, QStringList() << "边索引已就绪");

    auto* features = new QTreeWidgetItem(root, QStringList() << "特征边");
    if (featureEdges != nullptr) {
        new QTreeWidgetItem(features, QStringList() << QString("特征边总数：%1").arg(featureEdges->edges.size()));
        new QTreeWidgetItem(features, QStringList() << QString("锐边：%1").arg(featureEdges->sharp_edges));
        new QTreeWidgetItem(features, QStringList() << QString("自由边：%1").arg(featureEdges->free_edges));
        new QTreeWidgetItem(features, QStringList() << QString("多重边：%1").arg(featureEdges->multiple_edges));
    } else {
        new QTreeWidgetItem(features, QStringList() << "锐边：未检测");
    }
    new QTreeWidgetItem(features, QStringList() << QString("用户锁定边：%1").arg(lockedEdgeCount));

    auto* candidates = new QTreeWidgetItem(root, QStringList() << "合并候选区域");
    if (mergeCandidateCount > 0) {
        new QTreeWidgetItem(candidates, QStringList() << QString("候选总数：%1").arg(mergeCandidateCount));
        if (candidateTypeCounts != nullptr) {
            new QTreeWidgetItem(candidates, QStringList() << QString("PlaneLike：%1").arg(candidateTypeCounts->plane_like));
            new QTreeWidgetItem(candidates, QStringList() << QString("CylinderLike：%1").arg(candidateTypeCounts->cylinder_like));
            new QTreeWidgetItem(candidates, QStringList() << QString("SphereLike：%1").arg(candidateTypeCounts->sphere_like));
            new QTreeWidgetItem(candidates, QStringList() << QString("ConeLike：%1").arg(candidateTypeCounts->cone_like));
            new QTreeWidgetItem(candidates, QStringList() << QString("TorusLike：%1").arg(candidateTypeCounts->torus_like));
            new QTreeWidgetItem(candidates, QStringList() << QString("FreeformG1：%1").arg(candidateTypeCounts->freeform_g1));
            new QTreeWidgetItem(candidates, QStringList() << QString("FreeformG2：%1").arg(candidateTypeCounts->freeform_g2));
            new QTreeWidgetItem(candidates, QStringList() << QString("Unknown：%1").arg(candidateTypeCounts->unknown));
        }
        new QTreeWidgetItem(candidates, QStringList() << QString("待处理：%1").arg(pendingCandidateCount));
        new QTreeWidgetItem(candidates, QStringList() << QString("已接受：%1").arg(acceptedCandidateCount));
        new QTreeWidgetItem(candidates, QStringList() << QString("已拒绝：%1").arg(rejectedCandidateCount));
        new QTreeWidgetItem(candidates, QStringList() << QString("已隐藏：%1").arg(hiddenCandidateCount));
        new QTreeWidgetItem(candidates, QStringList() << QString("当前高亮：%1").arg(visibleMergeCandidateCount));
        new QTreeWidgetItem(candidates, QStringList() << QString("当前候选 ID：%1").arg(currentMergeCandidateId >= 0 ? QString::number(currentMergeCandidateId) : "无"));
    } else {
        new QTreeWidgetItem(candidates, QStringList() << "尚未生成候选区域");
    }

    tree_->expandToDepth(1);
}

}
