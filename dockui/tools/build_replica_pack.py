"""Generate a local, static-only prototype icon pack from manually traced geometry.
Requires Pillow for asset generation only; docky runtime does not depend on Python.
"""
from pathlib import Path
import json, math, zipfile
from PIL import Image, ImageDraw, ImageFont
root=Path(__file__).resolve().parents[1]/'examples'
pack=root/'replica-v19'
icons=pack/'icons'; previews=pack/'previews'
icons.mkdir(parents=True,exist_ok=True);previews.mkdir(parents=True,exist_ok=True)
S=1024; OUT=256; white=(245,247,250,255); transparent=(0,0,0,0)
def canvas():
 im=Image.new('RGBA',(S,S),transparent);return im,ImageDraw.Draw(im)
def xy(v):return int(v*S/100)
def line(d,points,width=5,fill=white):
 pts=[(xy(x),xy(y)) for x,y in points]; w=xy(width);d.line(pts,fill=fill,width=w,joint='curve')
 for x,y in pts: d.ellipse((x-w//2,y-w//2,x+w//2,y+w//2),fill=fill)
def rr(d,box,r,fill=None,outline=white,width=4):d.rounded_rectangle(tuple(map(xy,box)),xy(r),fill=fill,outline=outline,width=xy(width))
def save(im,name):im.resize((OUT,OUT),Image.Resampling.LANCZOS).save(icons/name,optimize=True)
# Full Wi-Fi reference: two broad concentric arcs and a circular dot.
im,d=canvas()
for radius,weight in [(35,7),(22,7)]:
 pts=[]
 for i in range(41):
  a=math.radians(225+90*i/40);pts.append((50+radius*math.cos(a),69+radius*math.sin(a)))
 line(d,pts,weight)
d.ellipse(tuple(map(xy,(45,65,55,75))),fill=white);save(im,'wifi.connected.png')
# Two rounded switches with contrasting cut-outs; reference is white on transparent.
im,d=canvas()
for box,knob in [((20,18,80,44),31),((20,54,80,80),69)]:
 rr(d,box,13,fill=None,outline=white,width=5); d.ellipse(tuple(map(xy,(knob-8,box[1]+5,knob+8,box[1]+21))),fill=white)
save(im,'control-center.png')
# Battery silhouette + negative bolt. Dynamic percentage is supplied by docky, not baked as 100.
im,d=canvas();rr(d,(10,25,85,75),13,fill=white,outline=white,width=2);rr(d,(86,41,91,59),2,fill=white,outline=white,width=1)
d.polygon([tuple(map(xy,p)) for p in [(58,29),(47,51),(54,51),(49,70),(66,46),(59,46),(63,29)]],fill=transparent);save(im,'battery.charging.png')
# Bluetooth outline traced from the reference silhouette.
im,d=canvas();line(d,[(50,12),(50,88),(68,69),(35,38)],5);line(d,[(50,12),(68,30),(35,62)],5);save(im,'bluetooth.on.png')
# User circle and bust.
im,d=canvas();d.ellipse(tuple(map(xy,(17,17,83,83))),outline=white,width=xy(6));d.ellipse(tuple(map(xy,(41,29,59,47))),fill=white)
d.pieslice(tuple(map(xy,(27,43,73,86))),180,360,fill=white);save(im,'user.png')
# Input source badge; no screenshot pixels or proprietary asset is embedded.
im,d=canvas();rr(d,(13,12,87,88),18,outline=white,width=5)
font=ImageFont.truetype(r'C:\Windows\Fonts\msyh.ttc',xy(47));box=d.textbbox((0,0),'拼',font=font);d.text(((S-(box[2]-box[0]))/2,(S-(box[3]-box[1]))/2-box[1]),'拼',font=font,fill=white);save(im,'input.pinyin.png')
manifest={'format':'dockypack','schemaVersion':1,'id':'reference-menubar-v19','name':'Reference Menu Bar','version':'0.1.0','description':'Hand-traced personal-use menu bar icon set; missing states use docky defaults.','icons':{'wifi.connected':'icons/wifi.connected.png','control-center':'icons/control-center.png','battery.charging':'icons/battery.charging.png','bluetooth.on':'icons/bluetooth.on.png','user':'icons/user.png','input.pinyin':'icons/input.pinyin.png'},'preview':'previews/overview.png'}
(pack/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
# Preview deliberately contains no user screenshot or network information.
preview=Image.new('RGBA',(1080,220),(17,20,28,255));d=ImageDraw.Draw(preview);font=ImageFont.truetype(r'C:\Windows\Fonts\segoeui.ttf',19)
for i,(key,path) in enumerate(manifest['icons'].items()):
 x=20+i*175;tile=Image.open(pack/path).resize((100,100),Image.Resampling.LANCZOS);preview.alpha_composite(tile,(x+25,24));d.text((x,148),key,font=font,fill=(223,229,238,255))
preview.save(previews/'overview.png',optimize=True)
notes='''# Reference inventory\n\nFive user-provided 2026-11-07 screenshots were inspected read-only. The menu bar shows user, Bluetooth, Wi-Fi, battery with a dynamic numeric percentage and charging bolt, Control Center and the clock. A separate screenshot shows a Pinyin input badge; the remaining screenshots show Wi-Fi/battery panels and the hovered Wi-Fi item.\n\nThe included icons are manually drawn approximations, not extracted proprietary assets or exact pixel matches. White marks use transparent backgrounds. The battery artwork deliberately does not bake the screenshot's 100% into the image; docky must display real battery data separately. The screenshots do not establish disconnected Wi-Fi, volume, alternate battery, dark-foreground or Bluetooth-off appearances, so those variants are omitted and must fall back. docky currently exposes only some of the shown menu items.\n'''
(pack/'reference-notes.md').write_text(notes,encoding='utf-8')
# The V1 archive uses uncompressed ZIP entries for a small, auditable importer.
with zipfile.ZipFile(root/'replica-v19.dockypack','x',compression=zipfile.ZIP_STORED) as z:
 for rel in ['manifest.json',*manifest['icons'].values(),'previews/overview.png']:
  z.write(pack/rel,rel)
print('Built:',root/'replica-v19.dockypack')
