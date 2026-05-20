#include "gui/ParameterPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QVBoxLayout>

namespace spo {

ParameterPanel::ParameterPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    auto* featureGroup = new QGroupBox("特征检测", this);
    auto* featureLayout = new QFormLayout(featureGroup);

    angularThreshold_ = new QSpinBox(featureGroup);
    angularThreshold_->setRange(0, 180);
    angularThreshold_->setSuffix(" 度");
    angularThreshold_->setValue(25);
    featureLayout->addRow("角度阈值", angularThreshold_);

    linearTolerance_ = new QDoubleSpinBox(featureGroup);
    linearTolerance_->setDecimals(6);
    linearTolerance_->setRange(0.0, 1000.0);
    linearTolerance_->setSingleStep(0.001);
    linearTolerance_->setValue(0.001);
    featureLayout->addRow("线性容差", linearTolerance_);

    curvatureThreshold_ = new QDoubleSpinBox(featureGroup);
    curvatureThreshold_->setDecimals(4);
    curvatureThreshold_->setRange(0.0, 1000.0);
    curvatureThreshold_->setSingleStep(0.01);
    curvatureThreshold_->setValue(0.1);
    featureLayout->addRow("曲率阈值", curvatureThreshold_);

    minEdgeLength_ = new QDoubleSpinBox(featureGroup);
    minEdgeLength_->setDecimals(4);
    minEdgeLength_->setRange(0.0, 1000.0);
    minEdgeLength_->setSingleStep(0.1);
    minEdgeLength_->setValue(0.0);
    featureLayout->addRow("最小边长", minEdgeLength_);

    auto* mergeGroup = new QGroupBox("合并", this);
    auto* mergeLayout = new QFormLayout(mergeGroup);

    mergeMode_ = new QComboBox(mergeGroup);
    mergeMode_->addItems({"同域合并", "区域生长", "手动分组"});
    mergeLayout->addRow("合并模式", mergeMode_);

    preserveFeatureEdges_ = new QCheckBox("保留特征边", mergeGroup);
    preserveFeatureEdges_->setChecked(true);
    mergeLayout->addRow(preserveFeatureEdges_);

    preserveUserLockedEdges_ = new QCheckBox("保留用户锁定边", mergeGroup);
    preserveUserLockedEdges_->setChecked(true);
    mergeLayout->addRow(preserveUserLockedEdges_);

    enableRefit_ = new QCheckBox("启用重拟合", mergeGroup);
    enableRefit_->setChecked(false);
    mergeLayout->addRow(enableRefit_);

    root->addWidget(featureGroup);
    root->addWidget(mergeGroup);
    root->addStretch();
}

AlgorithmParameters ParameterPanel::parameters() const {
    return {
        angularThreshold_->value(),
        linearTolerance_->value(),
        curvatureThreshold_->value(),
        minEdgeLength_->value(),
        mergeMode_->currentText(),
        preserveFeatureEdges_->isChecked(),
        preserveUserLockedEdges_->isChecked(),
        enableRefit_->isChecked()
    };
}

}
