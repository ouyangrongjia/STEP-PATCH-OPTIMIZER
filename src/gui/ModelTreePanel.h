#pragma once

#include "brep/ShapeDocument.h"

#include <QWidget>

class QTreeWidget;

namespace spo {

class ModelTreePanel final : public QWidget {
public:
    explicit ModelTreePanel(QWidget* parent = nullptr);
    void showEmpty();
    void showDocument(const ShapeDocument& document, int lockedEdgeCount = 0);

private:
    QTreeWidget* tree_ = nullptr;
};

}
