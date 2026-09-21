"""Plots of measured errors only; shared colour scales and original XYZ samples."""
import argparse
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run)
    plt.rcParams.update({'font.sans-serif':['Microsoft YaHei'],'axes.unicode_minus':False,'font.size':10,'pdf.fonttype':42})
    ref=np.load(out/'reference.npz');xy=((ref['test_points']-ref['center'])@ref['axes'].T)[:,:2]
    boundary=((ref['boundary_points']-ref['center'])@ref['axes'].T)[:,:2]
    variants=[('baseline','指定四片基线',out/'baseline_evaluation.npz'),
              ('g17_data','形状优先诊断（支撑外域异常）',out/'g17_data/trim_evaluation.npz'),
              ('g17_outer','平衡诊断（仍未通过边界门）',out/'g17_outer/trim_evaluation.npz')]
    fig,axes=plt.subplots(2,3,figsize=(12.4,7.1),layout='constrained')
    for col,(name,title,path) in enumerate(variants):
        values=np.load(path)
        for row,(key,cap,label) in enumerate([('distance',.08,'偏差 / mm'),('normal_angle',65,'法向偏差 / °')]):
            ax=axes[row,col];order=np.argsort(values[key]);image=ax.scatter(xy[order,0],xy[order,1],c=values[key][order],s=2.2,vmin=0,vmax=cap,cmap='magma',rasterized=True)
            ax.plot(boundary[:,0],boundary[:,1],'.',color='#576274',ms=.45,alpha=.5)
            index=int(np.argmax(values[key]));ax.scatter(*xy[index],s=62,facecolors='none',edgecolors='#30dbe1',linewidths=1.4)
            ax.set_aspect('equal');ax.set_xlabel('PCA 平面 x / mm');ax.set_ylabel('PCA 平面 y / mm')
            ax.set_title(title+f"\n最大 {values[key].max():.4g}；P95 {np.quantile(values[key],.95):.4g}")
            fig.colorbar(image,ax=ax,shrink=.75,label=label)
    fig.suptitle('相同的 18,000 个原始 STEP 采样点；青圈标出各指标最坏点',fontsize=13)
    fig.savefig(out/'figures/error_maps.png',dpi=300);fig.savefig(out/'figures/error_maps.pdf');plt.close(fig)
    profile=np.genfromtxt(out/'boundary_profile.csv',delimiter=',',names=True)
    fig,axes=plt.subplots(1,2,figsize=(10,3.6),layout='constrained')
    for ax,y,label,gate in [(axes[0],profile['normal_error_deg'],'法向偏差 / °',15),
                           (axes[1],profile['graph_distance_mm'],'投影对应点偏差 / mm',.03)]:
        ax.plot(profile['distance_from_edge_mm'],y,lw=2,color='#1869a4');ax.axhline(gate,color='#bd4135',ls='--',label='项目质量门')
        ax.set_xlabel('沿原始曲面离开边界的弦长 / mm');ax.set_ylabel(label);ax.grid(alpha=.2);ax.legend()
    fig.suptitle('形状优先候选的局部最坏区：原始 FaceId 1706，靠近 v = 0 边界')
    fig.savefig(out/'figures/boundary_profile.png',dpi=300);fig.savefig(out/'figures/boundary_profile.pdf');plt.close(fig)


if __name__=='__main__':main()
