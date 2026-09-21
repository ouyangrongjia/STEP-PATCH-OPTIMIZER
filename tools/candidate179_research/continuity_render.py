"""Same-camera STEP renders and measured normal-error point overlays."""
import argparse
from pathlib import Path
import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk
from PIL import Image,ImageDraw,ImageFont
from geometry import *
from render import render,polydata


def error_render(shape,points,values,center,axes,path,scale=4.5):
    p,t,_,_=tessellate(shape,.001);mapper=vtk.vtkPolyDataMapper();mapper.SetInputData(polydata(p,t))
    actor=vtk.vtkActor();actor.SetMapper(mapper);actor.GetProperty().SetColor(.83,.85,.88);actor.GetProperty().SetOpacity(.2)
    renderer=vtk.vtkRenderer();renderer.SetBackground(.98,.98,.985);renderer.AddActor(actor)
    cloud=vtk.vtkPolyData();vp=vtk.vtkPoints();vp.SetData(numpy_to_vtk(points,deep=True));cloud.SetPoints(vp)
    cloud.GetPointData().SetScalars(numpy_to_vtk(values,deep=True));vertices=vtk.vtkCellArray()
    for i in range(len(points)):vertices.InsertNextCell(1);vertices.InsertCellPoint(i)
    cloud.SetVerts(vertices)
    lut=vtk.vtkLookupTable();lut.SetNumberOfTableValues(256);lut.SetHueRange(.67,0.);lut.SetSaturationRange(.85,.85);lut.SetValueRange(.85,.85);lut.SetRange(0,35);lut.Build()
    m=vtk.vtkPolyDataMapper();m.SetInputData(cloud);m.SetLookupTable(lut);m.SetScalarRange(0,35);m.SetScalarModeToUsePointData();m.SetColorModeToMapScalars();m.ScalarVisibilityOn()
    a=vtk.vtkActor();a.SetMapper(m);a.GetProperty().SetPointSize(4);a.GetProperty().LightingOff();renderer.AddActor(a)
    bar=vtk.vtkScalarBarActor();bar.SetLookupTable(lut);bar.SetTitle('Normal error (deg)');bar.SetNumberOfLabels(6)
    bar.SetPosition(.84,.15);bar.SetWidth(.10);bar.SetHeight(.65);bar.GetLabelTextProperty().SetColor(.1,.1,.1);bar.GetTitleTextProperty().SetColor(.1,.1,.1);bar.GetTitleTextProperty().SetFontSize(15);renderer.AddViewProp(bar)
    focus=center+.65*axes[0]
    cam=renderer.GetActiveCamera();cam.SetPosition(*(focus+20*axes[2]));cam.SetFocalPoint(*focus);cam.SetViewUp(*axes[1]);cam.ParallelProjectionOn();cam.SetParallelScale(scale)
    win=vtk.vtkRenderWindow();win.SetOffScreenRendering(True);win.SetSize(1100,900);win.AddRenderer(renderer);win.SetMultiSamples(4);win.Render()
    capture=vtk.vtkWindowToImageFilter();capture.SetInput(win);capture.SetInputBufferTypeToRGB();capture.ReadFrontBufferOff();capture.Update()
    writer=vtk.vtkPNGWriter();writer.SetFileName(str(path));writer.SetInputConnection(capture.GetOutputPort());writer.Write();win.Finalize()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--candidate',default='aligned_inner_cubic');args=parser.parse_args()
    out=Path(args.run);fig=out/'figures';fig.mkdir(exist_ok=True);data=np.load(out/'reference.npz');ind=np.load(out/'confirmation_reference.npz')
    control=np.load(out/'control/evaluation.npz');new=np.load(out/args.candidate/'confirmation/evaluation.npz')
    oldpoint=data['test_points'][np.argmax(control['normal_angle'])];newpoint=ind['test_points'][np.argmax(new['normal_angle'])]
    titles=['原始区域 · 49 面','直接对照 · 5 面 / 4484 控制点','本轮候选 · 5 面 / 4324 控制点']
    inputs=[out/'original_region.step',out/'control/patch_faces.step',out/args.candidate/'patch_faces.step']
    font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',31);small=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',25)
    canvas=Image.new('RGB',(3300,2930),'white');draw=ImageDraw.Draw(canvas)
    rows=[('整体',data['center'],3.5,True),('上一轮最坏区域',oldpoint,.45,True),('剩余最坏区域',newpoint,.45,True)]
    for col,(title,path) in enumerate(zip(titles,inputs)):
        shape=read_step(path);p,t,_,_=tessellate(shape,.0005)
        draw.text((col*1100+25,12),title,font=font,fill='#172c44')
        for row,(label,center,scale,tilt) in enumerate(rows):
            name=fig/f'geometry_{col}_{row}.png';render(p,t,shape,center,data['axes'],name,tilt,parallel_scale=scale)
            canvas.paste(Image.open(name),(col*1100,75+row*940));draw.text((col*1100+20,65+row*940),label,font=small,fill='#172c44')
    draw.text((25,2890),'真实 STEP 三角化；同排相机和尺度一致。光照图用于定位，连续性与法向结论以数值检查为准。',font=small,fill='#873422')
    canvas.save(fig/'geometry_comparison.png')
    error=Image.new('RGB',(2200,1020),'white');draw=ImageDraw.Draw(error);source=read_step(out/'original_region.step')
    for i,(name,title) in enumerate([('control','直接对照'),(args.candidate,'本轮候选')]):
        values=np.load(out/name/'confirmation/evaluation.npz')['normal_angle'];path=fig/f'normal_samples_{i}.png'
        error_render(source,ind['test_points'],values,data['center'],data['axes'],path)
        error.paste(Image.open(path),(i*1100,70));draw.text((i*1100+25,15),f'{title} · 独立 30000 点 · 最大 {values.max():.2f}°',font=font,fill='#172c44')
    draw.text((25,978),'同一原始区域、相机与色标；颜色为实际投影测得的法向差，无插值补点。候选未通过 15° 门。',font=small,fill='#873422')
    error.save(fig/'normal_error_comparison.png')
    dump(fig/'views.json',dict(source='Actual STEP tessellation; normal overlays use exact original sample positions.',
        axes=data['axes'].tolist(),views=[dict(label=label,center=center.tolist(),parallel_scale=scale,tilt=tilt) for label,center,scale,tilt in rows],
        deflection_mm=.0005,normal_color_range_degrees=[0,35],independent_sample_count=30000,error_view_parallel_scale=4.5,error_view_center=(data['center']+.65*data['axes'][0]).tolist()))
    print(fig/'geometry_comparison.png',fig/'normal_error_comparison.png',flush=True)


if __name__=='__main__':main()
