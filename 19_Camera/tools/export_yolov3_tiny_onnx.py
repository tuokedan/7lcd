# Export the repository YOLOv3-Tiny checkpoint to ONNX on a PC.
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[2]
YOLO_ROOT=ROOT/'yolov3-master'
WEIGHTS=YOLO_ROOT/'yolov3'/'yolov3-tiny.pt'
if not WEIGHTS.exists(): raise FileNotFoundError(WEIGHTS)
cmd=[sys.executable,str(YOLO_ROOT/'export.py'),'--weights',str(WEIGHTS),'--include','onnx','--imgsz','320','--opset','12']
print('Running:', ' '.join(map(str,cmd)))
subprocess.run(cmd,cwd=YOLO_ROOT,check=True)
print('ONNX export completed.')
