#pragma once

#include "gui/GuiTypes.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace spo {

class ParameterPanel final : public QWidget {
public:
    explicit ParameterPanel(QWidget* parent = nullptr);
    AlgorithmParameters parameters() const;

private:
    QSpinBox* angularThreshold_ = nullptr;
    QDoubleSpinBox* linearTolerance_ = nullptr;
    QDoubleSpinBox* curvatureThreshold_ = nullptr;
    QDoubleSpinBox* minEdgeLength_ = nullptr;
    QComboBox* mergeMode_ = nullptr;
    QCheckBox* preserveFeatureEdges_ = nullptr;
    QCheckBox* preserveUserLockedEdges_ = nullptr;
    QCheckBox* concatBsplines_ = nullptr;
    QCheckBox* enableRefit_ = nullptr;
};

}
