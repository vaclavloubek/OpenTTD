"""YouTube thumbnail 1280x720 from 4K frames: main shot (train) + bus, ship and aircraft insets."""
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

D = sys.argv[1]
OUT = sys.argv[2]
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
VIEW = (8, 88, 3832, 1790)  # the 3D picture inside the cab window, without title bar and dashboard


def shot(name, w, h):
    im = Image.open(f'{D}/{name}.png').convert('RGB').crop(VIEW)
    sw, sh = im.size
    r = max(w / sw, h / sh)
    im = im.resize((round(sw * r), round(sh * r)), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


img = shot('k2', 1280, 720)
d = ImageDraw.Draw(img, 'RGBA')
# dark band on the left for the text
for x in range(700):
    d.line([(x, 0), (x, 720)], fill=(0, 0, 0, int(190 * (1 - x / 700) ** 1.3)))


def outlined(xy, txt, font, fill):
    for dx in range(-5, 6):
        for dy in range(-5, 6):
            if dx * dx + dy * dy <= 25:
                d.text((xy[0] + dx, xy[1] + dy), txt, font=font, fill=(0, 0, 0, 255))
    d.text(xy, txt, font=font, fill=fill)


outlined((40, 40), 'OpenTTD', ImageFont.truetype(BOLD, 96), (255, 255, 255))
outlined((40, 150), 'IN 3D', ImageFont.truetype(BOLD, 170), (252, 212, 0))
outlined((44, 350), 'first person view', ImageFont.truetype(BOLD, 52), (255, 255, 255))
# insets: bus, ship, aircraft
for k, name in enumerate(['bus', 'ship', 'air']):
    w, h = 300, 169
    x, y = 40 + k * (w + 16), 720 - h - 34
    d.rectangle((x - 6, y - 6, x + w + 6, y + h + 6), fill=(0, 0, 0, 255))
    d.rectangle((x - 3, y - 3, x + w + 3, y + h + 3), fill=(252, 212, 0, 255))
    img.paste(shot(name, w, h), (x, y))
img.save(OUT)
