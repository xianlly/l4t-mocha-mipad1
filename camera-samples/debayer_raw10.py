import sys
import numpy as np
from PIL import Image

path = sys.argv[1] if len(sys.argv) > 1 else r"L4T\raw10-1280.bin"
raw = np.fromfile(path, dtype=np.uint8)
print("bytes", raw.size)
W, H = 1280, 720
stride = 2560
packed = 1600
if raw.size < stride * H:
    print("short file")
    sys.exit(1)
img = raw[:stride * H].reshape(H, stride)
data = img[:, :packed]
gap = img[:, packed:]
print("data nonzero %.3f mean %.1f max %d" % ((data > 0).mean(), data.mean(), data.max()))
print("gap  nonzero %.3f mean %.1f max %d" % ((gap > 0).mean(), gap.mean(), gap.max()))

# MIPI RAW10: 4 pixels in 5 bytes, little-endian low bytes.
b = data.reshape(H, packed // 5, 5).astype(np.uint16)
p0 = (b[:, :, 0] | ((b[:, :, 4] & 0x03) << 8))
p1 = (b[:, :, 1] | (((b[:, :, 4] >> 2) & 0x03) << 8))
p2 = (b[:, :, 2] | (((b[:, :, 4] >> 4) & 0x03) << 8))
p3 = (b[:, :, 3] | (((b[:, :, 4] >> 6) & 0x03) << 8))
pix = np.stack([p0, p1, p2, p3], axis=2).reshape(H, W)
print("pix min %d max %d mean %.1f" % (pix.min(), pix.max(), pix.mean()))

# normalize for viewing
vis = (pix.astype(np.float32) / max(1, np.percentile(pix, 99.5)) * 255).clip(0, 255).astype(np.uint8)
Image.fromarray(vis).save(r"L4T\raw10-1280-gray.png")

# half-resolution RGGB: R at (0,0), G at (0,1)/(1,0), B at (1,1)
R = pix[0::2, 0::2].astype(np.float32)
G = (pix[0::2, 1::2].astype(np.float32) + pix[1::2, 0::2].astype(np.float32)) / 2
B = pix[1::2, 1::2].astype(np.float32)
rgb = np.stack([R, G, B], axis=2)
rgb = (rgb / max(1, np.percentile(rgb, 99.5)) * 255).clip(0, 255).astype(np.uint8)
Image.fromarray(rgb).save(r"L4T\raw10-1280-half.png")
print("saved L4T/raw10-1280-gray.png and L4T/raw10-1280-half.png", rgb.shape)
