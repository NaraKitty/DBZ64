"""Tile PNG frames from a run directory into one labelled contact sheet.

    python tools/contact_sheet.py RUN_DIR OUT.png [columns] [scale]
"""
import os
import sys

from PIL import Image, ImageDraw


def main():
    run, out = sys.argv[1], sys.argv[2]
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    scale = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    files = sorted(f for f in os.listdir(run) if f.startswith('frame_') and f.endswith('.png'))
    if not files:
        print('no frames')
        return
    first = Image.open(os.path.join(run, files[0]))
    w, h = int(first.width * scale), int(first.height * scale)
    rows = (len(files) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * w, rows * (h + 12)), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for k, f in enumerate(files):
        im = Image.open(os.path.join(run, f)).convert('RGB').resize((w, h), Image.NEAREST)
        x, y = (k % cols) * w, (k // cols) * (h + 12)
        sheet.paste(im, (x, y + 12))
        d.text((x + 2, y), f[6:-4], fill=(255, 255, 0))
    sheet.save(out)
    print('%d frames -> %s' % (len(files), out))


if __name__ == '__main__':
    main()
