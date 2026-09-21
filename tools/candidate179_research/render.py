"""Same-camera offscreen VTK renders of actual STEP tessellations."""
import argparse
from pathlib import Path
import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk, numpy_to_vtkIdTypeArray
from PIL import Image, ImageDraw, ImageFont
from geometry import *


def polydata(points, triangles):
    obj=vtk.vtkPolyData();p=vtk.vtkPoints();p.SetData(numpy_to_vtk(points,deep=True));obj.SetPoints(p)
    cells=vtk.vtkCellArray();cells.ImportLegacyFormat(numpy_to_vtkIdTypeArray(np.c_[np.full(len(triangles),3),triangles].astype(np.int64).ravel(),deep=True))
    obj.SetPolys(cells);return obj


def render(points,triangles,shape,center,axes,path,tilt=False,color=(.70,.76,.84),parallel_scale=3.5):
    poly=polydata(points,triangles)
    normals=vtk.vtkPolyDataNormals();normals.SetInputData(poly);normals.SetFeatureAngle(180);normals.SplittingOff();normals.ConsistencyOn()
    mapper=vtk.vtkPolyDataMapper();mapper.SetInputConnection(normals.GetOutputPort())
    actor=vtk.vtkActor();actor.SetMapper(mapper);actor.GetProperty().SetColor(*color)
    actor.GetProperty().SetSpecular(.18);actor.GetProperty().SetSpecularPower(35);actor.GetProperty().SetAmbient(.25);actor.GetProperty().SetDiffuse(.65)
    renderer=vtk.vtkRenderer();renderer.SetBackground(.97,.975,.985);renderer.AddActor(actor)
    for e in shapes(shape,TopAbs_EDGE):
        _,ps=edge_samples(e,97);line=vtk.vtkPolyData();vp=vtk.vtkPoints();vp.SetData(numpy_to_vtk(ps,deep=True));line.SetPoints(vp)
        cells=vtk.vtkCellArray();cells.InsertNextCell(len(ps))
        for i in range(len(ps)):cells.InsertCellPoint(i)
        line.SetLines(cells);lm=vtk.vtkPolyDataMapper();lm.SetInputData(line);la=vtk.vtkActor();la.SetMapper(lm)
        la.GetProperty().SetColor(.10,.17,.26);la.GetProperty().SetLineWidth(1.1);renderer.AddActor(la)
    cam=renderer.GetActiveCamera();direction=axes[2]+(.62*axes[0]-.35*axes[1] if tilt else 0)
    cam.SetPosition(*(center+direction*20));cam.SetFocalPoint(*center);cam.SetViewUp(*axes[1]);cam.ParallelProjectionOn();cam.SetParallelScale(parallel_scale)
    win=vtk.vtkRenderWindow();win.SetOffScreenRendering(1);win.SetSize(1100,900);win.AddRenderer(renderer);win.SetMultiSamples(8);win.Render()
    capture=vtk.vtkWindowToImageFilter();capture.SetInput(win);capture.SetInputBufferTypeToRGB();capture.ReadFrontBufferOff();capture.Update()
    writer=vtk.vtkPNGWriter();writer.SetFileName(str(path));writer.SetInputConnection(capture.GetOutputPort());writer.Write();win.Finalize()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run',required=True);parser.add_argument('--baseline',required=True)
    parser.add_argument('--candidate',default='g17_data');parser.add_argument('--label',default='形状候选 · 4 面 / 289 独立控制点')
    parser.add_argument('--footer',default='同一尺度与相机；真实 STEP 几何渲染。候选为移动外边界的自由重建诊断，尚不可局部替换。')
    parser.add_argument('--patch-file',default='four_patch_diagnostic.step');args=parser.parse_args()
    out=Path(args.run);ref=np.load(out/'reference.npz');fig=out/'figures';fig.mkdir(exist_ok=True)
    items=[('original','原始区域 · 49 面',out/'original_region.step'),('baseline','指定基线 · 4 面 / 9,645 控制点',Path(args.baseline)/'trimmed_patch_faces.step'),
           (args.candidate,args.label,out/args.candidate/args.patch_file)]
    font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',33);small=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',26)
    canvas=Image.new('RGB',(3300,1970),'white');draw=ImageDraw.Draw(canvas)
    for i,(name,title,path) in enumerate(items):
        shape=read_step(path);p,t,_,_=tessellate(shape,.001)
        for row,tilt in enumerate([False,True]):
            imagepath=fig/f'{name}_{row}.png';render(p,t,shape,ref['center'],ref['axes'],imagepath,tilt)
            canvas.paste(Image.open(imagepath),(i*1100,65+row*935))
        draw.text((i*1100+30,15),title,font=font,fill='#14283f')
    draw.text((30,1910),args.footer,font=small,fill='#9d291e')
    canvas.save(fig/f'comparison_{args.candidate}.png')
    print(fig/f'comparison_{args.candidate}.png')


if __name__=='__main__':main()
