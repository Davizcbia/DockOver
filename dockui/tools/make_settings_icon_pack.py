"""Build an original docky Settings icon pack (requires Pillow, not shipped at runtime)."""
from PIL import Image, ImageDraw
from io import BytesIO
from pathlib import Path
import json, zipfile, math

names=['general','appearance','dock','menu-bar','launchpad','control-center','taskbar','workshop','migration','about']
colors=['#86868c','#25252a','#242428','#85858b','#0088f5','#86868c','#59626e','#0585f8','#8a66c5','#85858b']
def icon(name,color):
 im=Image.new('RGBA',(192,192)); d=ImageDraw.Draw(im); d.rounded_rectangle((6,6,186,186),radius=42,fill=color); w='#ffffff'; line=10
 def ln(points):d.line(points,fill=w,width=line,joint='curve')
 def rect(box,r=14):d.rounded_rectangle(box,radius=r,outline=w,width=line)
 if name=='general':
  d.ellipse((51,51,141,141),outline=w,width=line);d.ellipse((79,79,113,113),outline=w,width=line)
  for i in range(8):
   a=i*math.pi/4;ln([(96+54*math.cos(a),96+54*math.sin(a)),(96+68*math.cos(a),96+68*math.sin(a))])
 elif name=='appearance':
  d.ellipse((45,45,147,147),outline=w,width=line);d.pieslice((45,45,147,147),-90,90,fill=w)
 elif name=='dock':rect((35,58,157,132));ln([(48,111),(144,111)])
 elif name=='menu-bar':
  for y in (62,96,130):ln([(40,y),(152,y)])
 elif name=='launchpad':
  for x in (43,105):
   for y in (43,105):rect((x,y,x+42,y+42),9)
 elif name=='control-center':
  rect((39,48,153,86),19);rect((39,106,153,144),19);d.ellipse((50,57,68,77),fill=w);d.ellipse((123,115,141,135),fill=w)
 elif name=='taskbar':rect((36,50,156,142));ln([(43,120),(149,120)])
 elif name=='workshop':rect((45,60,147,150));ln([(72,59),(72,39),(120,39),(120,59)])
 elif name=='migration':ln([(42,71),(147,71),(127,51)]);ln([(147,121),(42,121),(62,141)])
 else:d.ellipse((45,45,147,147),outline=w,width=line);ln([(96,87),(96,127)]);d.ellipse((91,62,101,72),fill=w)
 return im
icons={}; files={}
for name,color in zip(names,colors):
 key='settings.'+name; path='icons/'+key+'.png';b=BytesIO();icon(name,color).resize((96,96),Image.Resampling.LANCZOS).save(b,format='PNG');icons[key]=path;files[path]=b.getvalue()
preview=Image.new('RGBA',(600,240),(28,28,30,255))
for i,(name,color) in enumerate(zip(names,colors)):preview.alpha_composite(icon(name,color).resize((96,96),Image.Resampling.LANCZOS),((i%5)*116+10,(i//5)*110+10))
b=BytesIO();preview.save(b,format='PNG');files['previews/overview.png']=b.getvalue()
manifest={'format':'dockypack','schemaVersion':1,'id':'settings-original-v1','name':'docky Original Settings','version':'1.0.0','description':'Original vector-inspired category icons created for docky. No Apple or SF Symbols assets.','icons':icons,'preview':'previews/overview.png'}
files['manifest.json']=json.dumps(manifest,indent=2).encode()
out=Path(__file__).resolve().parents[1]/'examples'/'settings-original-v1.dockypack'
with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_STORED) as z:
 for name,data in sorted(files.items()):
  info=zipfile.ZipInfo(name,date_time=(2026,1,1,0,0,0));info.compress_type=zipfile.ZIP_STORED;z.writestr(info,data)
print(out)