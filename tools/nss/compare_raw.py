"""Compare two capture_nss_raw.sh runs bit for bit: compare_raw.py <dirA> <dirB>"""
import sys, glob, os
import numpy as np
a, b = sys.argv[1], sys.argv[2]
worst = 0
for fa in sorted(glob.glob(os.path.join(a, (sys.argv[3] if len(sys.argv) > 3 else "upscaled") + "_*.bin"))):
    fb = os.path.join(b, os.path.basename(fa))
    da = np.fromfile(fa, dtype=np.uint8); db = np.fromfile(fb, dtype=np.uint8)
    ha = da[:8].view(np.uint32); w, h = int(ha[0]), int(ha[1])
    pa = da[8:].view(np.float16).reshape(h, w, 4)[..., :3].astype(np.float32)
    pb = db[8:].view(np.float16).reshape(h, w, 4)[..., :3].astype(np.float32)
    diff = np.abs(pa - pb)
    n = int((da[8:] != db[8:]).sum())
    print(f'{os.path.basename(fa)}: differing bytes {n}, max |diff| {diff.max():.6g}, pixels differing {int((diff.max(axis=2) > 0).sum())}')
    worst = max(worst, n)
print('IDENTICAL' if worst == 0 else 'DIFFERENT')
