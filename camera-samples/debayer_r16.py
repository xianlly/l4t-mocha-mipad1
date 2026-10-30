import sys
import numpy as np
from PIL import Image

path = sys.argv[1] if len(sys.argv) > 1 else r"L4T\raw10-v2.bin"
W, H = 1280, 720
raw = np.fromfile(path, dtype="<u2")
print("bytes", raw.size * 2, "samples", raw.size)
if raw.size != W * H:
    print("unexpected size")
    sys.exit(1)
pix = raw.reshape(H, W).astype(np.float32)
print("min %d max %d mean %.1f nonzero %.3f" % (pix.min(), pix.max(), pix.mean(), (pix > 0).mean()))
# column means: detect a black strip
cm = pix.mean(axis=0)
print("col mean left %.1f mid %.1f right %.1f" % (cm[:100].mean(), cm[560:720].mean(), cm[-100:].mean()))

# Normalize to 10-bit full scale; apply a mild gamma for viewing.
def to8(a):
    a = a / 1023.0
    a = np.clip(a, 0, 1) ** (1 / 1.8)
    return (a * 255).astype(np.uint8)

Image.fromarray(to8(pix)).save(r"L4T\raw10-v2-gray.png")

# Half-resolution RGGB
R = pix[0::2, 0::2]
G = (pix[0::2, 1::2] + pix[1::2, 0::2]) / 2
B = pix[1::2, 1::2]
rgb = np.stack([to8(R), to8(G), to8(B)], axis=2)
Image.fromarray(rgb).save(r"L4T\raw10-v2-half.png")

# Simple white balance: use the brightest 1% as white
flat = rgb.reshape(-1, 3).astype(np.float32)
white = np.percentile(flat, 99, axis=0)
white[white < 1] = 1
rgb2 = np.clip(flat / white * 255, 0, 255).reshape(rgb.shape).astype(np.uint8)
Image.fromarray(rgb2).save(r"L4T\raw10-v2-half-wb.png")
print("saved raw10-v2-gray/half/half-wb")
