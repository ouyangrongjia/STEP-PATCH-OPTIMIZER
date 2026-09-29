"""Tensor B-spline evaluation and graph fitting, independent of product solver."""
import numpy as np
from scipy.interpolate import BSpline
from scipy import sparse
from scipy.sparse.linalg import splu
from OCP.Geom import Geom_BSplineSurface
from OCP.TColgp import TColgp_Array2OfPnt
from OCP.TColStd import TColStd_Array1OfReal, TColStd_Array1OfInteger
from OCP.gp import gp_Pnt


def knots(n, degree=3):
    return np.r_[np.zeros(degree), np.linspace(0, 1, n-degree+1), np.ones(degree)]


def basis(t, degree, x, derivative=0):
    return BSpline(t, np.eye(len(t)-degree-1), degree, extrapolate=False)(x, nu=derivative)


def tensor(uv, tu, tv, du=0, dv=0, degrees=(3, 3)):
    bu = sparse.coo_matrix(basis(tu, degrees[0], uv[:, 0], du))
    bv = basis(tv, degrees[1], uv[:, 1], dv)
    rows, cols, values = [], [], []
    for j in range(bv.shape[1]):
        v = bu.data*bv[bu.row,j]
        mask = v != 0
        rows.append(bu.row[mask]); cols.append(bu.col[mask]*bv.shape[1]+j); values.append(v[mask])
    return sparse.csr_matrix((np.concatenate(values), (np.concatenate(rows), np.concatenate(cols))),
                             shape=(len(uv),(len(tu)-degrees[0]-1)*bv.shape[1]))


def array1(values, integer=False):
    arr = (TColStd_Array1OfInteger if integer else TColStd_Array1OfReal)(1,len(values))
    for i, value in enumerate(values):
        arr.SetValue(i+1, int(value) if integer else float(value))
    return arr


def to_occt(poles, tu, tv, degrees=(3,3)):
    p = TColgp_Array2OfPnt(1,poles.shape[0],1,poles.shape[1])
    for i in range(poles.shape[0]):
        for j in range(poles.shape[1]):
            p.SetValue(i+1,j+1,gp_Pnt(*poles[i,j]))
    ku, mu = np.unique(tu, return_counts=True)
    kv, mv = np.unique(tv, return_counts=True)
    return Geom_BSplineSurface(p,array1(ku),array1(kv),array1(mu,True),array1(mv,True),*degrees)


def from_occt(surface):
    if surface.IsURational() or surface.IsVRational():
        raise ValueError('This experiment supports non-rational spline surfaces only')
    if surface.IsUPeriodic() or surface.IsVPeriodic():
        # STEP readers may periodize a closed support. SciPy needs a full
        # clamped knot sequence; make an exact evaluation copy, leaving the
        # native STEP representation (and its reported pole count) unchanged.
        bounds=surface.Bounds();surface=surface.Copy()
        if surface.IsUPeriodic():surface.SetUNotPeriodic()
        if surface.IsVPeriodic():surface.SetVNotPeriodic()
        surface.Segment(*bounds)
    poles = np.array([[[surface.Pole(i,j).X(),surface.Pole(i,j).Y(),surface.Pole(i,j).Z()]
                       for j in range(1,surface.NbVPoles()+1)] for i in range(1,surface.NbUPoles()+1)])
    tu = np.repeat([surface.UKnot(i) for i in range(1,surface.NbUKnots()+1)],
                   [surface.UMultiplicity(i) for i in range(1,surface.NbUKnots()+1)])
    tv = np.repeat([surface.VKnot(i) for i in range(1,surface.NbVKnots()+1)],
                   [surface.VMultiplicity(i) for i in range(1,surface.NbVKnots()+1)])
    return poles, tu, tv, (surface.UDegree(),surface.VDegree())


def evaluate(poles, tu, tv, uv, du=0, dv=0, degrees=(3,3)):
    return tensor(uv,tu,tv,du,dv,degrees)@poles.reshape(-1,3)


def fit_graph(points, boundaries, internal, center, axes, domain, n, fairness, boundary_weight, seam_weight,
              fairness_uv=None, solver_method='normal'):
    lo, size = domain
    tu = tv = knots(n)
    def coordinates(p):
        local = (p-center)@axes.T
        return (local[:,:2]-lo)/size, local[:,2]
    uv, z = coordinates(points)
    a = tensor(uv,tu,tv)/np.sqrt(len(uv))
    rhs = z/np.sqrt(len(z))
    blocks, targets = [a], [rhs]
    for samples, weight in [(boundaries,boundary_weight),(internal,seam_weight)]:
        if weight:
            q, h = coordinates(samples)
            factor = np.sqrt(weight/len(h))
            blocks.append(tensor(q,tu,tv)*factor)
            targets.append(h*factor)
    q = (np.arange(61)+.5)/61
    uvq = np.stack(np.meshgrid(q,q,indexing='ij'),axis=-1).reshape(-1,2)
    if fairness_uv is not None:
        uvq=np.asarray(fairness_uv)
    if fairness:
        for du,dv,factor in [(2,0,1/size[0]**2),(1,1,np.sqrt(2)/np.prod(size)),(0,2,1/size[1]**2)]:
            block = tensor(uvq,tu,tv,du,dv)*factor*np.sqrt(fairness/len(uvq))
            blocks.append(block); targets.append(np.zeros(len(uvq)))
    matrix = sparse.vstack(blocks).tocsc()
    target = np.concatenate(targets)
    # Tiny ridge stabilizes unobserved support outside the trimmed domain only.
    normal = (matrix.T@matrix).tocsc()+sparse.eye(n*n,format='csc')*1e-12
    if solver_method=='svd':
        from scipy.linalg import lstsq
        augmented=np.vstack([matrix.toarray(),np.eye(n*n)*1e-6])
        coeff,_,rank,singular=lstsq(augmented,np.r_[target,np.zeros(n*n)],lapack_driver='gelsd')
    else:
        solver = splu(normal)
        coeff = solver.solve(matrix.T@target)
    gradient = np.asarray(normal@coeff-matrix.T@target)
    greville = np.array([tu[i+1:i+4].mean() for i in range(n)])
    xx,yy = np.meshgrid(lo[0]+size[0]*greville,lo[1]+size[1]*greville,indexing='ij')
    local_poles = np.stack([xx,yy,coeff.reshape(n,n)],axis=-1)
    poles = local_poles@axes+center
    report=dict(normal_equation_relative_residual=float(np.linalg.norm(gradient)/max(np.linalg.norm(matrix.T@target),1e-30)),
                solver_method=solver_method,rows=matrix.shape[0],unknowns=n*n,nnz=matrix.nnz)
    if solver_method=='svd':
        report.update(augmented_rank=int(rank),augmented_condition_number=float(singular[0]/singular[-1]))
    return poles,tu,tv,report
