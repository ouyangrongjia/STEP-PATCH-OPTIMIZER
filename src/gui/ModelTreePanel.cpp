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

void ModelTreePanel::showDocument(const ShapeDocument& document, int lockedEdgeCount) {
    tree_->clear();
    const auto& stats = document.stats();

    auto* root = new QTreeWidgetItem(tree_, QStringList() << QString::fromStdString(document.displayName()));
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
    new QTreeWidgetItem(features, QStringList() << "锐边：未检测");
    new QTreeWidgetItem(features, QStringList() << QString("用户锁定边：%1").arg(lockedEdgeCount));

    auto* candidates = new QTreeWidgetItem(root, QStringList() << "合并候选区域");
    new QTreeWidgetItem(candidates, QStringList() << "尚未生成候选区域");

    tree_->expandToDepth(1);
}

}
