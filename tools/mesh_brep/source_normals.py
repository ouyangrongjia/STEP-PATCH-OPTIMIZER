"""Label-independent, area-weighted corner normals separated by mesh creases."""
import numpy as np
from scipy import sparse
from scipy.sparse.csgraph import connected_components
from .partition_fit import incidence


def crease_fan_normals(vertices, triangles, feature_angle=30., edge_incidence=None, filter_steps=0):
    """Join triangle corners only across non-crease edges at the same vertex.

    A vertex may carry several normal fans. Region labels do not participate,
    so a smooth edge cannot receive different targets solely due to its label.
    """
    if not np.isfinite(filter_steps) or filter_steps < 0 or int(filter_steps) != filter_steps:
        raise ValueError('Normal filter steps must be a nonnegative integer')
    edges, owners = incidence(triangles) if edge_incidence is None else edge_incidence
    cross = np.cross(vertices[triangles[:, 1]]-vertices[triangles[:, 0]],
                     vertices[triangles[:, 2]]-vertices[triangles[:, 0]])
    lengths = np.linalg.norm(cross, axis=1)
    if np.any(lengths <= 0):
        raise ValueError('Corner normal field requires nondegenerate triangles')
    unit = cross/lengths[:, None]
    smooth = np.sum(unit[owners[:, 0]]*unit[owners[:, 1]], axis=1) >= np.cos(np.radians(feature_angle))
    edges, owners = edges[smooth], owners[smooth]
    if filter_steps:
        # Bilateral face-normal diffusion on the original smooth-edge graph.
        # No source coordinates move, and feature cuts stay fixed throughout.
        centers = vertices[triangles].mean(axis=1)
        squared = np.sum((centers[owners[:, 0]]-centers[owners[:, 1]])**2, axis=1)
        radius2 = np.median(squared) if len(squared) else 1.
        spatial = np.exp(-squared/(2*max(radius2, np.finfo(float).tiny)))
        sigma = 2*np.sin(np.radians(feature_angle/2)/2)
        if not 0 < sigma:
            raise ValueError('Filtered normals require a positive feature angle')
        rows, cols = owners.ravel(), owners[:, ::-1].ravel()
        for _ in range(int(filter_steps)):
            signal = np.sum((unit[owners[:, 0]]-unit[owners[:, 1]])**2, axis=1)
            pair_weight = spatial*np.exp(-signal/(2*sigma*sigma))
            weights = np.repeat(pair_weight, 2)*lengths[cols]
            graph = sparse.csr_matrix((weights, (rows, cols)), shape=(len(triangles),)*2)
            filtered = graph@unit+lengths[:, None]*unit
            unit = filtered/np.linalg.norm(filtered, axis=1)[:, None]
        cross = unit*lengths[:, None]
    local = np.argmax(triangles[owners, None] == edges[:, None, :, None], axis=-1)
    a = (3*owners[:, 0, None]+local[:, 0]).ravel()
    b = (3*owners[:, 1, None]+local[:, 1]).ravel()
    graph = sparse.csr_matrix((np.ones(len(a), dtype=bool), (a, b)), shape=(3*len(triangles),)*2)
    count, component = connected_components(graph, directed=False)
    sums = np.column_stack([np.bincount(component, weights=np.repeat(cross[:, k], 3), minlength=count)
                            for k in range(3)])
    magnitude = np.linalg.norm(sums, axis=1)
    if np.any(magnitude <= 1e-30):
        raise ValueError('Degenerate normal fan')
    return (sums/magnitude[:, None])[component].reshape(-1, 3, 3)
