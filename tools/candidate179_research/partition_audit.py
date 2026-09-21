"""Projected trim-loop intersections, separated from mesh and surface folds."""
import argparse
from pathlib import Path
import numpy as np
from shapely.geometry import Polygon
from shapely.ops import unary_union
from geometry import *
from OCP.TopAbs import TopAbs_WIRE
from OCP.BRepTools import BRepTools_WireExplorer


def polygons(shape,center,axes):
    result=[]
    for face in shapes(shape,TopAbs_FACE):
        wire=TopoDS.Wire_s(shapes(face,TopAbs_WIRE)[0]);exp=BRepTools_WireExplorer(wire,face);loop=[]
        while exp.More():
            edge=exp.Current();_,points=edge_samples(edge,385)
            if edge.Orientation()==TopAbs_REVERSED:points=points[::-1]
            loop.extend(points[:-1]);exp.Next()
        xy=((np.asarray(loop)-center)@axes.T)[:,:2];result.append(Polygon(xy))
    return result


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);args=parser.parse_args();out=Path(args.run);ref=np.load(out/'reference.npz')
    source=polygons(read_step(out/'original_region.step'),ref['center'],ref['axes']);source_union=unary_union(source)
    results={}
    for name in ['g17_data','g17_outer_cropped']:
        regions=polygons(read_step(out/name/'four_patch_diagnostic.step'),ref['center'],ref['axes']);union=unary_union(regions)
        results[name]=dict(samples_per_curve=385,all_polygons_valid=all(p.is_valid for p in regions),
                           pairwise_interior_overlap_mm2=[dict(a=i,b=j,area=regions[i].intersection(regions[j]).area) for i in range(4) for j in range(i+1,4)],
                           symmetric_footprint_difference_mm2=source_union.symmetric_difference(union).area,
                           note='Polyline approximation of exact trim curves; footprint discrepancy includes curve sampling error. Graph injectivity prevents off-plane intersections.')
    dump(out/'partition_audit.json',results);print(results)


if __name__=='__main__':main()
