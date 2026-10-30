import sys
import numpy as np
from PIL import Image

path = sys.argv[1] if len(sys.argv) > 1 else r"L4T\raw10-1280.bin"
W, H = 1280, 720
stride, packed = 2560, 1600
raw = np.fromfile(path, dtype=np.uint8)
img = raw[:stride * H].reshape(H, stride)
data = img[:, :packed]
b = data.reshape(H, packed // 5, 5).astype(np.uint16)
p0 = b[:, :, 0] | ((b[:, :, 4] & 0x03) << 8)
p1 = b[:, :, 1] | (((b[:, :, 4] >> 2) & 0x03) << 8)
p2 = b[:, :, 2] | (((b[:, :, 4] >> 4) & 0x03) << 8)
p3 = b[:, :, 3] | (((b[:, :, 4] >> 6) & 0x03) << 8)
pix = np.stack([p0, p1, p2, p3], axis=2).reshape(H, W).astype(np.float32)
print("pix min %d max %d mean %.1f nonzero %.3f" % (pix.min(), pix.max(), pix.mean(), (pix > 0).mean()))

def box3(a):
    p = np.pad(a, 1, mode="edge")
    return (p[:-2, :-2] + p[:-2, 1:-1] + p[:-2, 2:] +
            p[1:-1, :-2] + p[1:-1, 1:-1] + p[1:-1, 2:] +
            p[2:, :-2] + p[2:, 1:-1] + p[2:, 2:]) / 9.0

def demosaic(pix, pattern):
    R = np.zeros_like(pix); G = np.zeros_like(pix); B = np.zeros_like(pix)
    if pattern == "RGGB":
        R[0::2, 0::2] = pix[0::2, 0::2]; G[0::2, 1::2] = pix[0::2, 1::2]
        G[1::2, 0::2] = pix[1::2, 0::2]; B[1::2, 1::2] = pix[1::2, 1::2]
    elif pattern == "BGGR":
        B[0::2, 0::2] = pix[0::2, 0::2]; G[0::2, 1::2] = pix[0::2, 1::2]
        G[1::2, 0::2] = pix[1::2, 0::2]; R[1::2, 1::2] = pix[1::2, 1::2]
    elif pattern == "GRBG":
        G[0::2, 0::2] = pix[0::2, 0::2]; R[0::2, 1::2] = pix[0::2, 1::2]
        B[1::2, 0::2] = pix[1::2, 0::2]; G[1::2, 1::2] = pix[1::2, 1::2]
    elif pattern == "GBRG":
        G[0::2, 0::2] = pix[0::2, 0::2]; B[0::2, 1::2] = pix[0::2, 1::2]
        R[1::2, 0::2] = pix[1::2, 0::2]; G[1::2, 1::2] = pix[1::2, 1::2]
    Rm = (R > 0).astype(np.float32); Gm = (G > 0).astype(np.float32); Bm = (B > 0).astype(np.float32)
    Rf = box3(R) / np.maximum(box3(Rm), 1e-6)
    Gf = box3(G) / np.maximum(box3(Gm), 1e-6)
    Bf = box3(B) / np.maximum(box3(Bm), 1e-6)
    rgb = np.stack([Rf, Gf, Bf], 2)
    flat = rgb.reshape(-1, 3)
    white = np.percentile(flat, 99, axis=0)
    white[white < 1] = 1
    rgb = np.clip(rgb / white, 0, 1) ** (1 / 2.2) * 255
    return rgb.astype(np.uint8)

for pat in ("RGGB", "BGGR", "GRBG", "GBRG"):
    out = demosaic(pix, pat)
    Image.fromarray(out).save(r"L4T\raw10-1280-%s-rgb.png" % pat)
    print("saved", pat)
