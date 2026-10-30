import sys
import numpy as np
from PIL import Image

path = sys.argv[1] if len(sys.argv) > 1 else r"L4T\raw10-1280.bin"
W, H = 1280, 720
stride, packed = 2560, 1600


def unpack_packed(raw):
    img = raw[:stride * H].reshape(H, stride)
    data = img[:, :packed]
    b = data.reshape(H, packed // 5, 5).astype(np.uint16)
    p0 = b[:, :, 0] | ((b[:, :, 4] & 0x03) << 8)
    p1 = b[:, :, 1] | (((b[:, :, 4] >> 2) & 0x03) << 8)
    p2 = b[:, :, 2] | (((b[:, :, 4] >> 4) & 0x03) << 8)
    p3 = b[:, :, 3] | (((b[:, :, 4] >> 6) & 0x03) << 8)
    return np.stack([p0, p1, p2, p3], axis=2).reshape(H, W).astype(np.float32)


def box3(a):
    p = np.pad(a, 1, mode="edge")
    return (p[:-2, :-2] + p[:-2, 1:-1] + p[:-2, 2:] +
            p[1:-1, :-2] + p[1:-1, 1:-1] + p[1:-1, 2:] +
            p[2:, :-2] + p[2:, 1:-1] + p[2:, 2:]) / 9.0


def median3(a):
    p = np.pad(a, 1, mode="edge")
    s = [p[i:i + a.shape[0], j:j + a.shape[1]] for i in range(3) for j in range(3)]
    return np.median(np.stack(s, 0), axis=0)


def demosaic(pix, pattern="RGGB"):
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
    else:  # GBRG
        G[0::2, 0::2] = pix[0::2, 0::2]; B[0::2, 1::2] = pix[0::2, 1::2]
        R[1::2, 0::2] = pix[1::2, 0::2]; G[1::2, 1::2] = pix[1::2, 1::2]
    Rm = (R > 0).astype(np.float32); Gm = (G > 0).astype(np.float32); Bm = (B > 0).astype(np.float32)
    Rf = box3(R) / np.maximum(box3(Rm), 1e-6)
    Gf = box3(G) / np.maximum(box3(Gm), 1e-6)
    Bf = box3(B) / np.maximum(box3(Bm), 1e-6)
    return np.stack([Rf, Gf, Bf], 2)


def save_rgb(rgb, name, gamma=2.2):
    x = np.clip(rgb, 0, None)
    hi = np.percentile(x, 99.5, axis=(0, 1), keepdims=True)
    x = x / np.maximum(hi, 1)
    x = np.clip(x, 0, 1) ** (1 / gamma) * 255
    Image.fromarray(x.astype(np.uint8)).save(rf"L4T\{name}.png")
    print("saved", name)


raw = np.fromfile(path, dtype=np.uint8)
print("bytes", raw.size)
img = raw[:stride * H].reshape(H, stride)
gap_nonzero = (img[:, packed:] > 0).mean()
print("gap nonzero %.3f" % gap_nonzero)
if gap_nonzero < 0.02:
    pix = unpack_packed(raw)
    print("packed", pix.min(), pix.max(), pix.mean())
else:
    u16 = raw.view("<u2").reshape(H, W).astype(np.float32)
    if u16.max() > 2000:
        pix = u16 * (1023.0 / max(u16.max(), 1))
    else:
        pix = u16
    print("16-bit", pix.min(), pix.max(), pix.mean())

# black level: 0.5 percentile per Bayer channel
bl = np.percentile(pix, 0.5)
pix = np.clip(pix - bl, 0, None)
print("black level", bl)

rgb = demosaic(pix, "RGGB")
save_rgb(rgb, "isp-rgb-linear")

# gray-world AWB
means = rgb.reshape(-1, 3).mean(0)
gain = means.mean() / np.maximum(means, 1)
rgb_awb = rgb * gain
save_rgb(rgb_awb, "isp-rgb-awb")
print("awb gain", gain)

# denoise chroma lightly, then gamma
rgb_dn = np.stack([median3(rgb_awb[:, :, c]) for c in range(3)], 2)
save_rgb(rgb_dn, "isp-rgb-awb-denoise")
print("done")
