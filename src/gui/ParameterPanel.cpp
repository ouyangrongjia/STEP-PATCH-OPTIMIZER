#include "gui/ParameterPanel.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QVBoxLayout>

namespace spo {

ParameterPanel::ParameterPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto* featureGroup = new QGroupBox("检测参数", this);
    auto* featureLayout = new QFormLayout(featureGroup);
    featureLayout->setContentsMargins(10, 14, 10, 10);
    featureLayout->setHorizontalSpacing(12);
    featureLayout->setVerticalSpacing(12);
    featureLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    featureLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    angularThreshold_ = new QSpinBox(featureGroup);
    angularThreshold_->setRange(0, 180);
    angularThreshold_->setSuffix(" 度");
    angularThreshold_->setValue(25);
    angularThreshold_->setMinimumHeight(30);
    featureLayout->addRow("角度阈值", angularThreshold_);

    linearTolerance_ = new QDoubleSpinBox(featureGroup);
    linearTolerance_->setDecimals(6);
    linearTolerance_->setRange(0.0, 1000.0);
    linearTolerance_->setSingleStep(0.001);
    linearTolerance_->setValue(0.001);
    linearTolerance_->setMinimumHeight(30);
    featureLayout->addRow("线性容差", linearTolerance_);

    curvatureThreshold_ = new QDoubleSpinBox(featureGroup);
    curvatureThreshold_->setDecimals(4);
    curvatureThreshold_->setRange(0.0, 1000.0);
    curvatureThreshold_->setSingleStep(0.01);
    curvatureThreshold_->setValue(0.1);
    curvatureThreshold_->setMinimumHeight(30);
    featureLayout->addRow("曲率阈值", curvatureThreshold_);

    minEdgeLength_ = new QDoubleSpinBox(featureGroup);
    minEdgeLength_->setDecimals(4);
    minEdgeLength_->setRange(0.0, 1000.0);
    minEdgeLength_->setSingleStep(0.1);
    minEdgeLength_->setValue(0.0);
    minEdgeLength_->setMinimumHeight(30);
    featureLayout->addRow("最小边长", minEdgeLength_);

    root->addWidget(featureGroup);
    root->addStretch();
}

AlgorithmParameters ParameterPanel::parameters() const {
    return {
        angularThreshold_->value(),
        linearTolerance_->value(),
        curvatureThreshold_->value(),
        minEdgeLength_->value(),
        QStringLiteral("同域合并"),
        true,
        true,
        true,
        false
    };
}

}
