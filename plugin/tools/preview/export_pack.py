"""Export the live body (S4 dump, DripSim_body.bin) to the compact pack the preview program loads.

Usage: python export_pack.py [DripSim_body.bin] [out body_pack.bin]
Pack (little endian): "DSP1", u32 vertices, u32 triangles, u32 bones, float position[v*3], float uv[v*2], float weight[v*4], u8 boneIndex[v*4],
u16 triangle[t*3], float skinToBoneRotation[b*9]  (the same arrays as DS::Body::Static).
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "reference"))
import read_body_dump as rb  # noqa: E402


def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else rb.DOCS
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "body_pack.bin")
    nverts, bones, _, _, parts = rb.read(dump)
    p = parts[0]
    stride = (p["desc"] & 0xF) * 4
    v = p["vbuf"].reshape(-1, stride)
    pos = v[:, :12].copy().view("<f4").reshape(-1, 3)
    uv = v[:, 16:20].copy().view("<f2").reshape(-1, 2).astype(np.float32)
    weight = v[:, 20:28].copy().view("<f2").reshape(-1, 4).astype(np.float32)
    index = v[:, 28:32].copy()
    tri = p["ibuf"].reshape(-1, 3).astype("<u2")
    s2b = np.array([b[1][:9] for b in bones], dtype="<f4")
    with open(out, "wb") as f:
        f.write(b"DSP1")
        f.write(struct.pack("<III", len(pos), len(tri), len(bones)))
        f.write(pos.astype("<f4").tobytes())
        f.write(uv.astype("<f4").tobytes())
        f.write(weight.astype("<f4").tobytes())
        f.write(index.astype(np.uint8).tobytes())
        f.write(tri.tobytes())
        f.write(s2b.tobytes())
    print(f"wrote {out}: {len(pos)} vertices, {len(tri)} triangles, {len(bones)} bones")


if __name__ == "__main__":
    main()
