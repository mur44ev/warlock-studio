"""Draws the Warlock Studio icon (Windows 10 flat tile: accent square, keycaps, one glowing row) into studio.ico."""
from pathlib import Path
from PIL import Image, ImageDraw

SIZE = 1024
ACCENT = (0, 120, 215)
KEY = (255, 255, 255)
GLOW = [(227, 0, 140), (255, 185, 0), (0, 178, 148), (0, 153, 188), (139, 100, 184)]


def draw() -> Image.Image:
    img = Image.new('RGBA', (SIZE, SIZE), ACCENT + (255,))
    layer = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))   # PIL does not blend alpha while drawing
    d = ImageDraw.Draw(layer)
    margin, gap = 150, 34
    cols, rows = 5, 3
    cell_w = (SIZE - 2 * margin - gap * (cols - 1)) // cols
    cell_h = 170
    top = 250
    for r in range(rows):
        for c in range(cols):
            x = margin + c * (cell_w + gap)
            y = top + r * (cell_h + gap)
            if r == 1:                      # the lit row: five RGB keys
                d.rectangle([x, y, x + cell_w, y + cell_h], fill=GLOW[c] + (255,))
            else:
                d.rectangle([x, y, x + cell_w, y + cell_h], fill=KEY + (70,))
    # wide space bar under the grid
    y = top + rows * (cell_h + gap)
    d.rectangle([margin + cell_w + gap, y, SIZE - margin - cell_w - gap, y + cell_h * 0.8], fill=KEY + (70,))
    return Image.alpha_composite(img, layer)


def main():
    out = Path(__file__).with_name('studio.ico')
    base = draw()
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    base.save(out, format='ICO', sizes=sizes)
    base.resize((256, 256), Image.LANCZOS).save(out.with_name('studio-icon-preview.png'))
    print('wrote', out, out.stat().st_size, 'bytes')


if __name__ == '__main__':
    main()
