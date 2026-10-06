"""iiv-server のアイコンを作る。

    python tools/make-icon.py

src/iiv-server.ico         ふだん: 青いタワー型 PC と、外へ出ていく矢印
src/iiv-server-active.ico  誰かが接続中: 電源ボタンと矢印の縁が緑
src/iiv-server.png         README 用
"""
import math, os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

BLUE = (37, 99, 235, 255)
BLUE_DARK = (29, 78, 216, 255)
BLUE_LIGHT = (147, 197, 253, 255)
NAVY = (23, 37, 84, 255)
WHITE = (255, 255, 255, 255)
GREEN = (34, 197, 94, 255)


def arrow(p0, p1, shaft, head):
    """p0 から p1 へ向かう矢印の多角形(p1 が先)"""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    n = math.hypot(dx, dy)
    ux, uy = dx / n, dy / n
    px, py = -uy, ux
    bx, by = p1[0] - ux * head, p1[1] - uy * head          # 頭の付け根
    s, h = shaft / 2, head * 0.85
    return [(p0[0] + px * s, p0[1] + py * s), (bx + px * s, by + py * s), (bx + px * h, by + py * h), p1,
            (bx - px * h, by - py * h), (bx - px * s, by - py * s), (p0[0] - px * s, p0[1] - py * s)]


def draw(size, accent):
    s = 1024
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 本体(奥行きの影 → 正面)
    d.rounded_rectangle([290, 70, 720, 960], radius=80, fill=BLUE_DARK)
    d.rounded_rectangle([230, 90, 660, 960], radius=80, fill=BLUE)
    # ドライブの溝
    d.rounded_rectangle([310, 200, 580, 255], radius=24, fill=BLUE_LIGHT)
    d.rounded_rectangle([310, 300, 580, 355], radius=24, fill=BLUE_LIGHT)
    # 電源ボタン
    d.ellipse([385, 760, 505, 880], fill=NAVY)
    d.ellipse([413, 788, 477, 852], fill=accent)
    # 外へ出ていく矢印(右上へ)。縁取りで本体から浮かせる
    a = arrow((560, 700), (950, 310), 150, 250)
    edge = NAVY if accent == WHITE else accent
    d.line(a + [a[0]], fill=edge, width=80, joint='curve')  # 縁: 輪郭を太い線でなぞる(外側の半分が見える)
    d.polygon(a, fill=WHITE)
    return im.resize((size, size), Image.LANCZOS)


def save(name, accent):
    imgs = [draw(n, accent) for n in SIZES]
    path = os.path.join(ROOT, 'src', name)
    imgs[-1].save(path, format='ICO', sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])
    return imgs[-1]


big = save('iiv-server.ico', WHITE)
save('iiv-server-active.ico', GREEN)
big.resize((128, 128), Image.LANCZOS).save(os.path.join(ROOT, 'src', 'iiv-server.png'))
print('ok')
