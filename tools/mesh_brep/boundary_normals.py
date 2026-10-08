"""Shared tangent-plane targets on the actual faired boundary positions."""
import numpy as np
from scipy.interpolate import BSpline
from .mesh import Locator

VERSION = 3


def targets(vertices, triangles, corner_normals, assignment, edges, owners, curves, pairs):
    if corner_normals.ndim != 3:
        raise ValueError('Faired boundary targets require crease-separated corner normals')
    edge_keys = edges[:, 0]*len(vertices)+edges[:, 1]
    locator = Locator(vertices, triangles)
    result = []
    for c, pair in zip(curves, pairs):
        ch, t, knot = c['chain'], c['parameters'], c['knots']
        cuts = np.unique(np.r_[t, knot])
        q = np.unique((cuts[:-1, None]+np.diff(cuts)[:, None]*np.linspace(0, 1, 5)).ravel())
        segment = np.clip(np.searchsorted(t, q, side='right')-1, 0, len(t)-2)
        alpha = (q-t[segment])/(t[segment+1]-t[segment])
        key = np.minimum(ch[:-1], ch[1:])*len(vertices)+np.maximum(ch[:-1], ch[1:])
        source_owners = owners[np.searchsorted(edge_keys, key)][segment]
        tangent = BSpline(knot, c['poles'], 3)(q, 1)
        speed = np.linalg.norm(tangent, axis=1)
        if np.any(speed < 1e-12):
            raise ValueError('Stationary faired boundary cannot define a tangent plane')
        tangent /= speed[:, None]
        sides = []
        for di in pair:
            side = np.argmax(assignment[source_owners] == di, axis=1)
            triangle_ids = source_owners[np.arange(len(q)), side]
            if not np.all(assignment[triangle_ids] == di):
                raise ValueError('Boundary normal owner differs from the supplied chart')
            first = np.argmax(triangles[triangle_ids] == ch[segment, None], axis=1)
            last = np.argmax(triangles[triangle_ids] == ch[segment+1, None], axis=1)
            normal = ((1-alpha[:, None])*corner_normals[triangle_ids, first]
                      +alpha[:, None]*corner_normals[triangle_ids, last])
            # Classify source creases before projecting onto the new tangent.
            # Projection must not turn two distinct crease fans into a false
            # common normal merely because the boundary has moved.
            length = np.linalg.norm(normal, axis=1)
            normal /= np.maximum(length[:, None], 1e-30)
            normal[length < .1] = 0.
            sides.append(normal)
        raw_agreement = np.sum(sides[0]*sides[1], axis=1)
        xyz = BSpline(knot, c['poles'], 3)(q)
        _, _, closest, bary = locator.closest(xyz)
        common = np.einsum('ij,ijk->ik', bary, corner_normals[closest])
        common /= np.maximum(np.linalg.norm(common, axis=1)[:, None], 1e-30)
        # Sample the source field at the moved position, not at the old
        # raster vertex. A large tangential slide on a curved transition can
        # otherwise prescribe an oscillating, physically incorrect normal.
        oriented = np.sum(common*(sides[0]+sides[1]), axis=1) > 1.
        common -= np.sum(common*tangent, axis=1)[:, None]*tangent
        length = np.linalg.norm(common, axis=1)
        common /= np.maximum(length[:, None], 1e-30)
        usable = (raw_agreement > np.cos(np.radians(5.))) & oriented & (length > .1)
        common[~usable] = 0.
        # Only source-smooth joins receive a common tangent plane. Crease
        # sides remain governed by their own source fitting observations.
        result.append([common, common.copy()])
    return result
