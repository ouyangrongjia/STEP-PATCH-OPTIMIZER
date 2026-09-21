"""Measured errors and actual STEP control nets for the layout experiment."""
import argparse
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from geometry import read_step, shapes, evaluate, dump, BRep_Tool, TopAbs_FACE
from spline import from_occt


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args()
    out=Path(args.run);protocol=json.loads((out/'protocol.json').read_text(encoding='utf-8'))
    ref=np.load(out/'reference.npz');figdir=out/'figures';figdir.mkdir(exist_ok=True)
    source=Path(protocol['source_run']);base=Path(protocol['baseline'])/'trimmed_patch_faces.step'
    plt.rcParams.update({'font.sans-serif':['Microsoft YaHei'],'axes.unicode_minus':False,'font.size':10,'pdf.fonttype':42})
    variants=[('指定四片基线',base,source/'baseline_evaluation.npz'),
              ('连续性候选 · 5 面 / 4484 点',out/'five_knot_smooth/patch_faces.step',out/'five_knot_smooth/evaluation.npz'),
              ('大中央面对照 · 5 面 / 2892 点',out/'five_large_panel/patch_faces.step',out/'five_large_panel/evaluation.npz')]
    xy=((ref['test_points']-ref['center'])@ref['axes'].T)[:,:2]
    fig,axes=plt.subplots(2,3,figsize=(12.6,7.3),layout='constrained')
    for col,(title,_,path) in enumerate(variants):
        values=np.load(path)
        for row,(key,cap,label) in enumerate([('distance',.08,'偏差 / mm'),('normal_angle',65,'法向偏差 / °')]):
            ax=axes[row,col];order=np.argsort(values[key])
            im=ax.scatter(xy[order,0],xy[order,1],c=values[key][order],s=2.4,vmin=0,vmax=cap,cmap='magma',rasterized=True)
            index=int(np.argmax(values[key]));ax.scatter(*xy[index],s=70,facecolors='none',edgecolors='#28e4e1',linewidths=1.5)
            ax.set_aspect('equal');ax.set_xlabel('PCA x / mm');ax.set_ylabel('PCA y / mm')
            ax.set_title(title+f'\n最大 {values[key].max():.4g}；P95 {np.quantile(values[key],.95):.4g}')
            fig.colorbar(im,ax=ax,shrink=.75,label=label)
    fig.suptitle('相同 18,000 个原始区域样本；相同色标；青圈为最坏点。采样最大值不是连续域上界。',fontsize=12)
    fig.savefig(figdir/'error_maps.png',dpi=240);fig.savefig(figdir/'error_maps.pdf');plt.close(fig)
    fig,axes=plt.subplots(1,3,figsize=(13.4,4.7),layout='constrained')
    colors=['#4477aa','#ee6677','#228833','#ccbb44','#aa3377']
    for ax,(title,path,_) in zip(axes,variants):
        for i,face in enumerate(shapes(read_step(path),TopAbs_FACE)):
            p,*_=from_occt(BRep_Tool.Surface_s(face));q=((p-ref['center'])@ref['axes'].T)[...,:2]
            for row in q:ax.plot(row[:,0],row[:,1],color=colors[i],lw=.35,alpha=.65)
            for row in q.transpose(1,0,2):ax.plot(row[:,0],row[:,1],color=colors[i],lw=.35,alpha=.65)
        ax.set_aspect('equal');ax.set_xlim(xy[:,0].min()-.3,xy[:,0].max()+.3);ax.set_ylim(xy[:,1].min()-.3,xy[:,1].max()+.3)
        ax.set_title(title);ax.set_xlabel('PCA x / mm');ax.set_ylabel('PCA y / mm')
    fig.suptitle('实际 STEP 支撑面的控制多边形投影；每种颜色是一张 CAD 面，网格线不是额外面。',fontsize=12)
    fig.savefig(figdir/'control_nets.png',dpi=240);fig.savefig(figdir/'control_nets.pdf');plt.close(fig)
    # Audit the starting maps: Coons construction can depend on the curve basis.
    t=np.linspace(0,1,41);uv=np.array(np.meshgrid(t,t,indexing='ij')).reshape(2,-1).T
    checks=[]
    for a,b in [('five_boundary_layers','five_coherent_layers'),('five_compact_seams','five_compact_coherent'),('five_coherent_layers','five_knot_smooth')]:
        first=shapes(read_step(out/a/'initial_layout.step'),TopAbs_FACE)
        second=shapes(read_step(out/b/'initial_layout.step'),TopAbs_FACE)
        errors=[];xy_errors=[];height_errors=[]
        for f,g in zip(first,second):
            p,_=evaluate(BRep_Tool.Surface_s(f),uv);q,_=evaluate(BRep_Tool.Surface_s(g),uv)
            errors.append(float(np.linalg.norm(p-q,axis=1).max()))
            delta=(p-q)@ref['axes'].T
            xy_errors.append(float(np.linalg.norm(delta[:,:2],axis=1).max()));height_errors.append(float(np.abs(delta[:,2]).max()))
        checks.append(dict(first=a,second=b,grid_per_patch=[41,41],maximum_co_parameter_position_difference_mm=max(errors),
            maximum_xy_map_difference_mm=max(xy_errors),maximum_co_parameter_height_difference_mm=max(height_errors),patch_errors_mm=errors,
            note='Same-UV comparison, not nearest-surface distance. A changed XY map confounds seam-space attribution.'))
    dump(out/'initial_equivalence.json',checks)


if __name__=='__main__':main()
