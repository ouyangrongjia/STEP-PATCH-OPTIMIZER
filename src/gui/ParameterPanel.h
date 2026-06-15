#pragma once

#include "gui/GuiTypes.h"

#include <QWidget>

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
};

}
