#pragma once

#include <QString>

namespace spo {

enum class SelectionMode {
    Face,
    Edge,
    Candidate
};

struct AlgorithmParameters {
    int angular_threshold_degrees = 25;
    double linear_tolerance = 0.001;
    double curvature_threshold = 0.1;
    double min_edge_length = 0.0;
    QString merge_mode = "同域合并";
    bool preserve_feature_edges = true;
    bool preserve_user_locked_edges = true;
    bool concat_bsplines = true;
    bool enable_refit = false;
};

}
