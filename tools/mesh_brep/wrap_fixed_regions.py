"""Run inside installed Wrap's Python, with explicitly supplied regional seams.

This optional commercial backend consumes an STL and a coordinate-only seam
sidecar. It does not read the source CAD or invoke automatic segmentation.
"""
import json
import os
import struct
import time
import traceback


def main():
    root=os.environ['REGIONAL_WRAP_OUTPUT'];start=time.time()
    config=json.load(open(os.path.join(root,'input.json'),encoding='utf-8'))
    report=dict(status='running',backend='Geomagic Wrap fixed regional edges',source_CAD_read=False,config=config)
    def record(stage,**values):
        report.update(stage=stage,seconds=time.time()-start,**values)
        with open(os.path.join(root,'run.json'),'w',encoding='utf-8') as fp:json.dump(report,fp,indent=2)
        with open(os.path.join(root,'progress.log'),'a',encoding='utf-8') as fp:fp.write(json.dumps(dict(stage=stage,seconds=time.time()-start,**values))+'\n')
    record('load_api')
    try:
        import geomagic.api.v3 as geo
        reader=geo.ReadFile();reader.filename=os.path.join(root,'input.stl');reader.type='stl'
        options=geo.FileOpenOptions();options.units=geo.Length.Millimeters;reader.options=options;reader.run();mesh=reader.mesh
        if mesh.numTriangles!=config['triangles']:raise ValueError('STL import changed the triangle count')
        low=mesh.minBoxCoord;high=mesh.maxBoxCoord
        native_bounds=[[low.x(),low.y(),low.z()],[high.x(),high.y(),high.z()]]
        ratios=[(native_bounds[1][k]-native_bounds[0][k])/(config['bounds'][1][k]-config['bounds'][0][k]) for k in range(3) if config['bounds'][1][k]>config['bounds'][0][k]]
        scale=sum(ratios)/len(ratios)
        if max(abs(r/scale-1) for r in ratios)>1e-5:raise ValueError('Native STL coordinate scale is not uniform')
        record('select_fixed_edges',loaded_triangles=mesh.numTriangles,loaded_points=mesh.numPoints,native_bounds=native_bounds,input_mm_to_native_scale=scale)
        def coordinate_key(point):return struct.pack('<fff',*[0. if x==0 else x for x in point])
        def key(a,b):return tuple(sorted((a,b)))
        constraints=json.load(open(os.path.join(root,'constraints.json'),encoding='utf-8'))
        wanted={key(coordinate_key(e[:3]),coordinate_key(e[3:])) for e in constraints['edges']}
        corners={coordinate_key(p) for p in constraints['junctions']}
        edges=geo.EdgeSelection(mesh);vertices=geo.VertexSelection(mesh);seen=set();found_corners=set();coordinates={}
        for fi in range(mesh.maxTriangleIndex+1):
            if not mesh.isValidTriangle(fi):continue
            ids=[mesh.getVertexIndex(fi,0,k) for k in range(3)]
            for corner,vid in enumerate(ids):
                if vid not in coordinates:
                    point=mesh.getCoordinate(vid);value=coordinate_key([point.x()/scale,point.y()/scale,point.z()/scale]);coordinates[vid]=value
                    if value in corners:vertices.setSelected(fi,corner*2,True);found_corners.add(value)
            for side in range(3):
                edge=key(coordinates[ids[side]],coordinates[ids[(side+1)%3]])
                if edge in wanted and edge not in seen:
                    rep=geo.OrientedTriangle(fi,side*2)
                    actual=key(coordinates[mesh.getOrigVertex(rep)],coordinates[mesh.getDestVertex(rep)])
                    if actual!=edge:raise ValueError('Oriented edge convention mismatch')
                    edges.select(rep)
                    if not edges.isSelected(rep):raise ValueError('Native selection rejected a fixed region edge')
                    seen.add(edge)
            if fi%100000==0:record('select_fixed_edges',triangles_visited=fi,matched_edges=len(seen))
        if seen!=wanted or found_corners!=corners:raise ValueError('Native STL indexing did not preserve all supplied seams and junctions')
        record('autosurface',matched_edges=len(seen),matched_junctions=len(found_corners))
        fit=geo.AutoSurface();fit.mesh=mesh;fit.fixedEdges=edges;fit.fixedVertices=vertices
        fit.fileName=os.path.join(root,'surfaces.igs');fit.units=geo.Length.Millimeters
        fit.geometry=getattr(geo.AutoSurface,config.get('geometry','Organic'));fit.adaptiveFit=config.get('adaptive_fit',True);fit.autoMerge=config.get('engine_merge',False)
        fit.numPatches=config.get('num_patches',0);fit.tolerance=config['distance_mm']*scale;fit.duplicateMesh=True
        fit.extendContours=False;fit.sharpenConstrainedContours=config.get('sharp_boundaries',False);fit.run()
        if not os.path.isfile(fit.fileName):raise RuntimeError(str(fit.errorMsg))
        if not config.get('export_native_step',True):
            record('complete',status='DiagnosticOnly',can_apply=False,iges_bytes=os.path.getsize(fit.fileName))
            return
        record('convert_iges')
        reader=geo.ReadFile();reader.filename=fit.fileName;reader.options=options;reader.run();model=reader.cadModel
        record('export_step',bodies=model.numBodies,open_edge_loops=model.numCADOpenEdgeLoops)
        writer=geo.WriteFile();writer.cadModel=model;writer.filename=os.path.join(root,'model.step')
        save=geo.FileSaveOptions();save.units=geo.Length.Millimeters;writer.options=save;writer.filterId=5;writer.run()
        if not os.path.isfile(writer.filename):raise RuntimeError('STEP export did not create a file')
        record('complete',status='DiagnosticOnly',can_apply=False,step_bytes=os.path.getsize(writer.filename))
    except Exception as error:
        record('failed',status='failed',error=str(error),traceback=traceback.format_exc())
        raise


if __name__=='__main__':main()
