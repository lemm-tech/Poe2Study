"""Rebuild the original route-arrow icon. Requires Pillow; no network access."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

OUT = Path(__file__).resolve().parent
SIZE = 1024
SCALE = SIZE / 256
def pts(values):
    return [(round(x*SCALE), round(y*SCALE)) for x, y in values]
def box(values):
    return tuple(round(x*SCALE) for x in values)

mask = Image.new('L', (SIZE, SIZE))
d = ImageDraw.Draw(mask)
d.rounded_rectangle(box((8, 8, 248, 248)), radius=round(54*SCALE), fill=255)
base = Image.new('RGBA', (SIZE, SIZE))
pixels = base.load()
for y in range(SIZE):
    for x in range(SIZE):
        t = (x+y)/(SIZE*2)
        pixels[x,y] = (round(29-15*t), round(39-20*t), round(60-32*t), 255)
base.putalpha(mask)
rim = ImageDraw.Draw(base)
rim.rounded_rectangle(box((9, 9, 247, 247)), radius=round(53*SCALE), outline=(77,102,141,150), width=round(2*SCALE))
for n in (76, 128, 180):
    rim.line(pts(((40,n),(216,n))), fill=(91,123,167,16), width=round(SCALE))
    rim.line(pts(((n,40),(n,216))), fill=(91,123,167,16), width=round(SCALE))

route = Image.new('L', (SIZE,SIZE))
d = ImageDraw.Draw(route)
path = [(65,188),(65,138),(136,138),(136,80),(184,80)]
d.line(pts(path), fill=255, width=round(19*SCALE), joint='curve')
for x,y in path[1:-1]:
    d.ellipse(box((x-9.5,y-9.5,x+9.5,y+9.5)),fill=255)
d.polygon(pts(((178,49),(213,80),(178,111))),fill=255)
glow = Image.new('RGBA',(SIZE,SIZE),(55,149,255,0))
glow.putalpha(route.filter(ImageFilter.GaussianBlur(5*SCALE)).point(lambda a: a//3))
base = Image.alpha_composite(base,glow)
ink = Image.new('RGBA',(SIZE,SIZE))
pixels = ink.load()
for y in range(SIZE):
    for x in range(SIZE):
        t = max(0,min(1,(x/SIZE + 1-y/SIZE)/2))
        pixels[x,y] = (round(78+38*t),round(142+87*t),255,255)
ink.putalpha(route)
base = Image.alpha_composite(base,ink)
d = ImageDraw.Draw(base)
d.ellipse(box((49,172,81,204)),fill=(115,196,255,255))
d.ellipse(box((57,180,73,196)),fill=(24,35,52,255))
base.resize((256,256),Image.Resampling.LANCZOS).save(OUT/'poe2.png')
base.save(OUT/'poe2.ico',sizes=[(n,n) for n in (16,20,24,32,40,48,64,128,256)])
print('Saved poe2.ico (9 sizes) and poe2.png')
