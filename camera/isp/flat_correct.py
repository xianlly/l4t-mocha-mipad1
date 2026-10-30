import numpy as np
from PIL import Image

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


def demosaic(pix):
    R = np.zeros_like(pix); G = np.zeros_like(pix); B = np.zeros_like(pix)
    R[0::2, 0::2] = pix[0::2, 0::2]; G[0::2, 1::2] = pix[0::2, 1::2]
    G[1::2, 0::2] = pix[1::2, 0::2]; B[1::2, 1::2] = pix[1::2, 1::2]
    Rm = (R > 0).astype(np.float32); Gm = (G > 0).astype(np.float32); Bm = (B > 0).astype(np.float32)
    Rf = box3(R) / np.maximum(box3(Rm), 1e-6)
    Gf = box3(G) / np.maximum(box3(Gm), 1e-6)
    Bf = box3(B) / np.maximum(box3(Bm), 1e-6)
    return np.stack([Rf, Gf, Bf], 2)


def save_rgb(rgb, name):
    x = np.clip(rgb, 0, None)
    hi = np.percentile(x, 99.5, axis=(0, 1), keepdims=True)
    x = np.clip(x / np.maximum(hi, 1), 0, 1) ** (1 / 2.2) * 255
    Image.fromarray(x.astype(np.uint8)).save(rf"L4T\{name}.png")
    print("saved", name)


scene = unpack_packed(np.fromfile(r"L4T\raw10-1280.bin", dtype=np.uint8))
flat = np.fromfile(r"L4T\raw-force41.bin", dtype=np.uint8).view("<u2").reshape(H, W).astype(np.float32)
flat = flat * (1023.0 / max(flat.max(), 1))
print("scene", scene.min(), scene.max(), scene.mean())
print("flat", flat.min(), flat.max(), flat.mean())

# per-column fixed pattern from the covered frame
flat_col = flat.mean(axis=0)
smooth = np.convolve(flat_col, np.ones(65) / 65.0, mode="same")
gain = np.clip(smooth / np.maximum(flat_col, 1), 0.5, 2.0)
print("gain min/max/mean", gain.min(), gain.max(), gain.mean())

corr = scene * gain[None, :]
rgb = demosaic(corr)
means = rgb.reshape(-1, 3).mean(0)
rgb = rgb * (means.mean() / np.maximum(means, 1))
save_rgb(rgb, "isp-flat-corrected")

# without correction for comparison
rgb2 = demosaic(scene)
means2 = rgb2.reshape(-1, 3).mean(0)
rgb2 = rgb2 * (means2.mean() / np.maximum(means2, 1))
save_rgb(rgb2, "isp-no-flat")
