# v0.10.0 phase 12: offline simulation of the captured fence's alpha test (ATS frame 1634, PS R3102: discard when
# alpha < 0.05; texture R14778 128x2048 BC3, 12 mips, anisotropic x16 trilinear, WRAP) under the mod's 8 Halton jitter phases,
# with and without a texture LOD bias. Plain Python 3 + numpy + Pillow (not RenderDoc).
#   python scripts\fence_alpha_sim.py [dump dir]     (default captures\ats_flat_fence_frame1634.fence_dump, written by
#                                                      scripts\rdc_fence_audit.py: the alpha mips m0.png .. m11.png)
# Model: the fence plane face-on (aniso x1) or seen at an angle (x2: twice the texels per pixel along x), mip-0 texel
# densities 2.3 .. 11.3 texels per pixel (the capture: 4-8 at the near / middle fence), hardware-like LOD = log2(major / N)
# + bias with N = min(16, ceil(major / minor)) taps along the major axis, trilinear between the captured mips, bilinear,
# alpha test at 0.05; reference = the box-filtered coverage of the mip-0 mask (alpha >= 0.5) over each pixel (8x8 supersampled).
# Printed per (pattern, aniso, bias), averaged over the densities:
#   flicker     = % of the covered pixels whose pass / fail differs between the 8 jitter phases (what DLAA must average out)
#   temporal std = the mean per-pixel standard deviation of that 1-bit signal over the 8 phases
#   moire        = the std of the 8x8-block low-pass of (coverage - reference): a structured pattern (mean offset removed),
#                  single frame (phase 0) and converged (the 8-phase mean = a perfect temporal accumulation)
#   coverage     = the mean converged coverage vs the true one (the 0.05 threshold dilates the wires: they read thicker)
import os, sys
import numpy as np
from PIL import Image

base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = sys.argv[1] if len(sys.argv) > 1 else os.path.join(base, "captures", "ats_flat_fence_frame1634.fence_dump")
M = []
while os.path.isfile(os.path.join(D, "m%d.png" % len(M))):
    M.append(np.asarray(Image.open(os.path.join(D, "m%d.png" % len(M))).convert("L")).astype(np.float64) / 255.0)
if not M:
    sys.exit("no alpha mips in %s (run scripts\\rdc_fence_audit.py first)" % D)
H0, W0 = M[0].shape
# the mod's 8 phases (the log's 'DLAA eval' viewport shifts, flat)
PH = [(0, -0.1667), (-0.25, 0.1667), (0.25, -0.3889), (-0.375, -0.0556), (0.125, 0.2778), (-0.125, -0.2778), (0.375, 0.0556),
      (-0.4375, 0.3889)]

def bil(L, u, v):
    a = M[L]; h, w = a.shape
    x = u * w - 0.5; y = v * h - 0.5
    x0 = np.floor(x); y0 = np.floor(y); fx = x - x0; fy = y - y0
    x0 = x0.astype(int); y0 = y0.astype(int)
    g = lambda xx, yy: a[np.mod(yy, h), np.mod(xx, w)]
    return g(x0, y0) * (1 - fx) * (1 - fy) + g(x0 + 1, y0) * fx * (1 - fy) + g(x0, y0 + 1) * (1 - fx) * fy + g(x0 + 1, y0 + 1) * fx * fy

def tri(u, v, lam):
    lam = float(np.clip(lam, 0, len(M) - 1)); L0 = int(np.floor(lam)); fr = lam - L0
    s = bil(L0, u, v)
    if fr > 0 and L0 + 1 < len(M): s = s * (1 - fr) + bil(L0 + 1, u, v) * fr
    return s

def render(tx, ty, v0, bias, jx, jy, NX, NY, aniso=16):
    X, Y = np.meshgrid(np.arange(NX) + 0.5 + jx, np.arange(NY) + 0.5 + jy)
    u = X * tx / W0; v = (v0 + Y * ty) / H0
    major, minor = max(tx, ty), min(tx, ty)
    N = min(aniso, int(np.ceil(major / minor)))
    lam = np.log2(major / N) + bias
    acc = 0
    for k in range(N):
        o = (k + 0.5) / N - 0.5
        acc = acc + (tri(u + o * tx / W0, v, lam) if tx >= ty else tri(u, v + o * ty / H0, lam))
    return acc / N

def ref(tx, ty, v0, NX, NY, ss=8):
    r = np.zeros((NY, NX))
    for i in range(ss):
        for j in range(ss):
            X, Y = np.meshgrid(np.arange(NX) + (i + 0.5) / ss, np.arange(NY) + (j + 0.5) / ss)
            r += M[0][np.floor(v0 + Y * ty).astype(int) % H0, np.floor(X * tx).astype(int) % W0] >= 0.5
    return r / (ss * ss)

def lp(a, k=8):
    h, w = a.shape; h -= h % k; w -= w % k
    return a[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))

s = H0 / 2048.0                                         # the atlas regions in mip-0 rows of the 2048-row texture
for name, v0, v1 in (("welded grid", 513 * s, 965 * s), ("chain link", 1058 * s, 2045 * s)):
    for ratio in (1, 2):
        for bias in (0.0, -0.5, -1.0):
            F = []; Dv = []; Es = []; Et = []; Cv = []; Rv = []
            for t in (2.3, 3.1, 4.4, 5.7, 7.9, 11.3):
                tx, ty = t * ratio, t
                NX = 192; NY = min(96, int((v1 - v0) / ty) - 2)
                r = ref(tx, ty, v0, NX, NY)
                c = np.array([(render(tx, ty, v0, bias, jx, jy, NX, NY) >= 0.05).astype(float) for jx, jy in PH])
                m = c.mean(0); ever = c.max(0) > 0
                F.append(np.mean(c.max(0)[ever] != c.min(0)[ever])); Dv.append(np.mean(c.std(0)[ever]))
                Es.append(np.std(lp(c[0] - r))); Et.append(np.std(lp(m - r))); Cv.append(m.mean()); Rv.append(r.mean())
            print("%-11s aniso x%d bias %+.1f: flicker %4.1f%% of covered px (temporal std %.3f), moire single frame %.3f / "
                  "converged %.3f, coverage %.2f (true %.2f)" % (name, ratio, bias, 100 * np.mean(F), np.mean(Dv), np.mean(Es),
                                                                  np.mean(Et), np.mean(Cv), np.mean(Rv)))
