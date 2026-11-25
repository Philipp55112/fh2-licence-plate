# Prueft die leuchtenden Lampenglaeser (HZN_lt) im Release gegen das FH2-Automodell:
# Blick von hinten auf die Kennzeichen-Umgebung, grau/farbig = FH2-Modell, gelb umrandet = unser Leuchtglas.
# Aufruf: lt_check.py release.zip out_prefix [Autos...]
import sys, zipfile, struct, re, json, numpy as np, matplotlib
matplotlib.use('Agg'); import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection
sys.path.insert(0, '/home/claude/work'); sys.path.insert(0, '/home/claude/work/light')
import fbundle, light.lampglass as L
from light.render_rear import cat

def lt_tris(data):
    B = fbundle.Bundle.load(data); ids = {}
    for x in B.blobs:
        if x.tag == b'ItaM':
            m = x.meta(b'  dI')
            if m: ids[struct.unpack('<I', m.data)[0]] = (fbundle.name(x) or '').lower()
    lt = [k for k, v in ids.items() if v == 'hzn_lt']
    if not lt: return None
    ib = [x for x in B.blobs if x.tag == b'BdnI'][0].data; ic = struct.unpack_from('<I', ib, 0)[0]; I = np.frombuffer(ib[16:16 + ic * 4], '<u4')
    pos = None
    for x in B.blobs:
        if x.tag == b'BreV':
            m = x.meta(b'  dI'); idv = struct.unpack('<I', m.data)[0] if m else 0xffffffff
            n, nb, st = struct.unpack_from('<IIH', x.data, 0)
            if idv == 0xffffffff and st == 12: pos = np.frombuffer(x.data[16:16 + n * 12], '<f4').reshape(-1, 3)
    T = []
    for x in B.blobs:
        if x.tag == b'hseM' and struct.unpack_from('<H', x.data, 0)[0] in lt:
            s, c = struct.unpack_from('<I', x.data, 25)[0], struct.unpack_from('<I', x.data, 33)[0]
            idx = I[s:s + c]; idx = idx[:len(idx) // 3 * 3].reshape(-1, 3); T.append(pos[idx])
    return np.concatenate(T) if T else None

z = zipfile.ZipFile(sys.argv[1]); names = z.namelist(); out = sys.argv[2]
cars = {}
for l in z.read('LicencePlates/plugins/PlateLightCars.csv').decode().splitlines():
    if not l or l[0] == '#' or '@' not in l: continue
    f = l.split(';'); car = f[0].split('@')[0]; n = int(f[1])
    cars[car] = [np.array([float(x) for x in f[2 + i * 11:5 + i * 11]]) for i in range(n)]
want = sys.argv[3:] or sorted(cars)
res = {}
per_sheet = 20; sheets = [want[i:i + per_sheet] for i in range(0, len(want), per_sheet)]
for si, group in enumerate(sheets):
    cols = 5; rowsn = (len(group) + cols - 1) // cols
    fig, axs = plt.subplots(rowsn, cols, figsize=(5 * cols, 3.6 * rowsn)); axs = np.array(axs).ravel()
    for ax in axs: ax.axis('off')
    for ax, car in zip(axs, group):
        glass = cars[car]
        files = [x for x in names if x.lower().startswith(('LicencePlates/media/cars/' + car + '/scene/exterior/hzn/').lower()) and x.endswith('.modelbin')]
        T = None
        for fn in sorted(files):
            T = lt_tris(z.read(fn))
            if T is not None and len(T): break
        try: c, right, up, n = L.frame(car)
        except Exception as e: ax.set_title(car + ' (kein Rahmen)'); continue
        polys = []; colsl = []; dep = []
        for f, mn, me, TT in L.M.car(L.src[car.lower()]):
            if re.search(r'_[b-z]\.modelbin$', f): continue
            D = TT - c; u = D @ right; v = D @ up; w = D @ n
            m = (np.abs(u.mean(1)) < 0.45) & (v.mean(1) > -0.22) & (v.mean(1) < 0.28) & (w.mean(1) > -0.3) & (w.mean(1) < 0.35)
            col = cat(me or mn or '')
            for k in np.nonzero(m)[0]:
                polys.append(np.stack([u[k], v[k]], 1)); colsl.append(col + (0.55,)); dep.append(w[k].mean())
        o = np.argsort(dep); ax.add_collection(PolyCollection([polys[k] for k in o], facecolors=[colsl[k] for k in o], edgecolors='none'))
        info = 'KEIN HZN_lt'
        if T is not None and len(T):
            P = T.reshape(-1, 3); g = np.mean(glass, 0)
            if np.median(np.min([np.linalg.norm(P - q, axis=1) for q in glass], 0)) > 0.25: T = T + (g - P.mean(0))
            D = T - c
            ax.add_collection(PolyCollection([np.stack([t @ right, t @ up], 1) for t in D], facecolors=(1, 1, 0, 0.9), edgecolors=(1, 0.3, 0, 1), linewidths=0.6))
            uu = (D @ right).ravel(); vv = (D @ up).ravel()
            info = 'lt %.1fx%.1f cm' % ((uu.max() - uu.min()) * 100, (vv.max() - vv.min()) * 100)
            res[car] = info
        W, H = L.rows[car]['W'] / 1000, L.rows[car]['H'] / 1000
        ax.add_patch(plt.Rectangle((-W / 2, -H / 2), W, H, fill=False, ec='b', lw=0.8))
        ax.set_xlim(0.45, -0.45); ax.set_ylim(-0.22, 0.28); ax.set_aspect('equal'); ax.axis('on'); ax.set_xticks([]); ax.set_yticks([])
        ax.set_title('%s  %s' % (car, info), fontsize=9)
    plt.tight_layout(); plt.savefig('%s_%02d.png' % (out, si + 1), dpi=60); plt.close(fig)
    print('sheet', si + 1, flush=True)
json.dump(res, open(out + '_info.json', 'w'), indent=1)
