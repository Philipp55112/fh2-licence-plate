#include <mutex>
#include <map>
// Kennzeichen-Editor: Schild mit eigenem Text erzeugen (gleiche Schritte wie make_eu_tex.py / make_us_tex.py)
#include "plate_editor.h"
#include "ext/bc7enc.h"
#include "ext/rgbcx.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
static FILE* uopen(const std::string& p, const char* m) { wchar_t w[1024]; MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, w, 1024); wchar_t wm[8]; MultiByteToWideChar(CP_UTF8, 0, m, -1, wm, 8); return _wfopen(w, wm); }
#else
static FILE* uopen(const std::string& p, const char* m) { return fopen(p.c_str(), m); }
#endif
namespace {
typedef std::vector<uint8_t> B;
struct Img { int w = 0, h = 0, c = 0; std::vector<float> p; float& at(int x, int y, int k) { return p[(size_t(y) * w + x) * c + k]; } };
Img mk(int w, int h, int c) { Img i; i.w = w; i.h = h; i.c = c; i.p.assign(size_t(w) * h * c, 0.f); return i; }
// 3 Box-Durchgaenge ~ Gauss (wie PIL GaussianBlur, Radius = Sigma)
void boxblur1(std::vector<float>& a, int w, int h, int r, bool horiz) {
    if (r < 1) return; std::vector<float> o(a.size());
    int n = horiz ? w : h, m = horiz ? h : w;
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    auto run = [&](int j0, int j1) {   // v4.13: Zeilen/Spalten auf Threads verteilt
        for (int j = j0; j < j1; ++j) {
            auto idx = [&](int i) { i = std::max(0, std::min(n - 1, i)); return horiz ? size_t(j) * w + i : size_t(i) * w + j; };
            double s = 0; for (int i = -r; i <= r; ++i) s += a[idx(i)];
            for (int i = 0; i < n; ++i) { o[horiz ? size_t(j) * w + i : size_t(i) * w + j] = float(s / (2 * r + 1)); s += a[idx(i + r + 1)] - a[idx(i - r)]; }
        }
    };
    std::vector<std::thread> th; int per = (m + int(nt) - 1) / int(nt);
    for (unsigned t = 0; t < nt; ++t) { int j0 = int(t) * per, j1 = std::min(m, j0 + per); if (j0 < j1) th.emplace_back(run, j0, j1); }
    for (auto& x : th) x.join();
    a.swap(o);
}
void gauss(std::vector<float>& a, int w, int h, float sigma) {
    // Boxgroessen fuer 3 Durchgaenge
    float wi = std::sqrt(12.f * sigma * sigma / 3 + 1); int wl = int(wi); if (wl % 2 == 0) wl--; int wu = wl + 2;
    float mi = (12 * sigma * sigma - 3 * wl * wl - 12 * wl - 9) / (-4.f * wl - 4); int mm = int(std::round(mi));
    for (int k = 0; k < 3; ++k) { int sz = k < mm ? wl : wu; int r = (sz - 1) / 2; boxblur1(a, w, h, r, true); boxblur1(a, w, h, r, false); }
}
Img half(const Img& s) {   // 2x2-Mittel
    Img d = mk(std::max(1, s.w / 2), std::max(1, s.h / 2), s.c);
    for (int y = 0; y < d.h; ++y) for (int x = 0; x < d.w; ++x) for (int k = 0; k < s.c; ++k) {
        int x0 = std::min(2 * x, s.w - 1), x1 = std::min(2 * x + 1, s.w - 1), y0 = std::min(2 * y, s.h - 1), y1 = std::min(2 * y + 1, s.h - 1);
        Img& S = const_cast<Img&>(s); d.at(x, y, k) = 0.25f * (S.at(x0, y0, k) + S.at(x1, y0, k) + S.at(x0, y1, k) + S.at(x1, y1, k));
    }
    return d;
}
Img resize_to(Img s, int w, int h) { while (s.w > w || s.h > h) s = half(s); return s; }
// Durango-Kachelung (tile.py)
uint32_t p2(uint32_t v) { uint32_t n = 1; while (n < v) n *= 2; return n; }
struct Lev { int bw, bh, pw, ph; bool d2; size_t off; };
std::vector<Lev> layout(int w, int h, int mips, size_t& total) {
    std::vector<Lev> L; size_t off = 0;
    for (int m = 0; m < mips; ++m) {
        Lev l; l.bw = std::max(1, (w + 3) / 4); l.bh = std::max(1, (h + 3) / 4); l.pw = p2(l.bw); l.ph = p2(l.bh);
        if (l.pw >= 64 && l.ph >= 32) { l.d2 = true; } else { l.d2 = false; l.pw = std::max(l.pw, 8); l.ph = std::max(l.ph, 8); }
        l.off = off; off += size_t(l.pw) * l.ph; L.push_back(l); w = std::max(1, w / 2); h = std::max(1, h / 2);
    }
    total = off; return L;
}
size_t tile_index(const Lev& l, uint32_t X, uint32_t Y) {
    auto b = [](uint32_t v, int k) { return (v >> k) & 1u; };
    if (l.d2) {
        uint32_t bits[11] = {b(X,0), b(Y,0), b(X,1), b(Y,1), b(X,3)^b(X,4)^b(Y,3), b(X,4)^b(Y,4), b(X,5)^b(Y,5), b(Y,4)^b(Y,5)^b(X,6), b(Y,3)^b(X,7), b(X,2), b(Y,2)};
        size_t idx = 0; for (int k = 0; k < 11; ++k) idx |= size_t(bits[k]) << k;
        size_t mx = X >> 6, my = Y >> 5, tpr = l.pw >> 6;
        return l.off + idx + ((my * tpr + mx) << 11);
    }
    size_t inner = b(X,0) | b(Y,0) << 1 | b(X,1) << 2 | b(Y,1) << 3 | b(X,2) << 4 | b(Y,2) << 5;
    size_t pitch = std::max(8, (l.bw + 7) / 8 * 8) / 8;
    return l.off + inner + (((Y >> 3) * pitch + (X >> 3)) << 6);
}
uint8_t cl8(float v) { return uint8_t(std::max(0.f, std::min(255.f, v))); }
// Bloecke eines Bildes kodieren (bc7 = 4 Kanaele, bc5 = Kanal 0/1)
// v4.12: Block-Zwischenspeicher je Ausgabe/Stufe – nur Bloecke mit geaenderten Pixeln neu kodieren
struct EncCache { int w = 0, h = 0, c = 0; bool bc7 = false; std::vector<uint8_t> px, blk; };
std::map<std::string, EncCache> g_enc_cache;
void encode(const Img& im, bool bc7, std::vector<uint8_t>& out, int& bw, int& bh, const std::string& key) {
    bw = std::max(1, (im.w + 3) / 4); bh = std::max(1, (im.h + 3) / 4); out.assign(size_t(bw) * bh * 16, 0);
    static bc7enc_compress_block_params P; static std::once_flag once;
    std::call_once(once, [] { bc7enc_compress_block_init(); rgbcx::init(); bc7enc_compress_block_params_init(&P); bc7enc_compress_block_params_init_linear_weights(&P); P.m_max_partitions = 16; P.m_uber_level = 0; });
    static std::mutex mx; EncCache* Cp; { std::lock_guard<std::mutex> lk(mx); Cp = &g_enc_cache[key]; } EncCache& C = *Cp;
    bool reuse = C.w == im.w && C.h == im.h && C.c == im.c && C.bc7 == bc7 && C.blk.size() == out.size();
    if (!reuse) { C.w = im.w; C.h = im.h; C.c = im.c; C.bc7 = bc7; C.px.assign(size_t(bw) * bh * 64, 0); C.blk.assign(out.size(), 0); }
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; ++t) th.emplace_back([&, t] {
        uint8_t px[64];
        for (int by = int(t); by < bh; by += int(nt)) for (int bx = 0; bx < bw; ++bx) {
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                int sx = std::min(im.w - 1, bx * 4 + x), sy = std::min(im.h - 1, by * 4 + y);
                for (int k = 0; k < 4; ++k) px[(y * 4 + x) * 4 + k] = k < im.c ? cl8(const_cast<Img&>(im).at(sx, sy, k)) : 255;
            }
            size_t bi = size_t(by) * bw + bx; uint8_t* dst = &out[bi * 16];
            if (reuse && memcmp(&C.px[bi * 64], px, 64) == 0) { memcpy(dst, &C.blk[bi * 16], 16); continue; }
            if (bc7) bc7enc_compress_block(dst, px, &P); else rgbcx::encode_bc5(dst, px, 0, 1, 4);
            memcpy(&C.px[bi * 64], px, 64); memcpy(&C.blk[bi * 16], dst, 16);
        }
    });
    for (auto& x : th) x.join();
}
// swatchbin schreiben: Kopf (83 Byte, Masse/Groesse passend) + gekachelte Bloecke; tiles: Atlas (cols x rows gleiche Zellen)
bool write_swatch(const std::string& fn, const uint8_t* head, const Img& src, bool bc7, int cols, int rows, std::string& err) {
    int w, h; memcpy(&w, head + 0x40, 2); w &= 0xffff; memcpy(&h, head + 0x42, 2); h &= 0xffff; int n = head[0x4c];
    size_t total; auto L = layout(w, h, n, total);
    std::vector<uint8_t> pay(total * 16, 0);
    Img cell = src; Img full; bool cv = true;   // cv: Zelle beschreibt die Stufe noch exakt
    auto tile = [&](int W, int H) { Img F = mk(W, H, cell.c); for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) for (int k = 0; k < cell.c; ++k) F.at(x, y, k) = cell.at(x % cell.w, y % cell.h, k); return F; };
    for (int m = 0; m < n; ++m) {
        const Lev& l = L[m]; int W = std::max(1, w >> m), H = std::max(1, h >> m);
        if (cv && !(cell.w * cols == W && cell.h * rows == H)) { cv = false; }
        std::vector<uint8_t> blk; int bw, bh;
        if (cv && cell.w % 4 == 0 && cell.h % 4 == 0) {
            encode(cell, bc7, blk, bw, bh, fn + "#" + std::to_string(m));
            for (int y = 0; y < l.bh; ++y) for (int x = 0; x < l.bw; ++x) memcpy(&pay[tile_index(l, x, y) * 16], &blk[(size_t(y % bh) * bw + (x % bw)) * 16], 16);
            full = Img();
        } else {
            if (cv) full = tile(W, H); else full = half(full);
            encode(full, bc7, blk, bw, bh, fn + "#" + std::to_string(m));
            for (int y = 0; y < l.bh; ++y) for (int x = 0; x < l.bw; ++x) memcpy(&pay[tile_index(l, x, y) * 16], &blk[(size_t(y) * bw + x) * 16], 16);
        }
        if (cv) { if (cell.w >= 2 && cell.h >= 2) cell = half(cell); else { if (full.w == 0) full = tile(W, H); cv = false; } }
    }
    FILE* f = uopen(fn + ".tmp", "wb"); if (!f) { err = "cannot write: " + fn; return false; }
    fwrite(head, 1, 83, f); fwrite(pay.data(), 1, pay.size(), f); fclose(f);
#ifdef _WIN32
    wchar_t a[1024], b2[1024]; MultiByteToWideChar(CP_UTF8, 0, (fn + ".tmp").c_str(), -1, a, 1024); MultiByteToWideChar(CP_UTF8, 0, fn.c_str(), -1, b2, 1024);
    MoveFileExW(a, b2, MOVEFILE_REPLACE_EXISTING);
#else
    rename((fn + ".tmp").c_str(), fn.c_str());
#endif
    return true;
}
}  // namespace

bool pled_generate(const std::string& data, const std::string& text_in, const std::string out[3], std::string& err) {
    bool hidden = !text_in.empty() && text_in[0] == '!';   // "!" vorn = Schild entfernt (Geometrie, v4.14) – Textur unveraendert
    std::string text = hidden ? text_in.substr(1) : text_in;
    FILE* f = uopen(data, "rb"); if (!f) { err = "data missing: " + data; return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); B d(sz); fread(d.data(), 1, sz, f); fclose(f);
    size_t o = 0; auto u32 = [&]() { uint32_t v; memcpy(&v, &d[o], 4); o += 4; return v; }; auto i32 = [&]() { int32_t v; memcpy(&v, &d[o], 4); o += 4; return v; };
    auto f32 = [&]() { float v; memcpy(&v, &d[o], 4); o += 4; return v; };
    if (memcmp(&d[0], "PLED", 4)) { err = "data format"; return false; } o = 4; if (u32() != 2) { err = "data version"; return false; }
    int W = u32(), H = u32(); uint32_t flags = u32(); (void)flags;
    float nstr = f32(), nblur = f32(), aoblur = f32(), aok = f32(), dim = f32();
    int cw = u32(), ch = u32(), acols = u32(), arows = u32(), nw = u32(), nh = u32(), gw_ = u32(), gh_ = u32();
    uint8_t lut[256]; memcpy(lut, &d[o], 256); o += 256; uint8_t ink[5]; memcpy(ink, &d[o], 5); o += 5;
    int ns = u32(); std::vector<int> sl(ns * 4); for (auto& v : sl) v = i32();
    int gw = u32(), gh = u32(), nc = u32(); std::vector<char> gc(nc); std::vector<size_t> go(nc);
    for (int i = 0; i < nc; ++i) { gc[i] = char(d[o]); o += 1; go[i] = o; o += size_t(gw) * gh; }
    size_t ob = o; o += size_t(W) * H * 6;
    int anc[8]; for (int i = 0; i < 8; ++i) anc[i] = i32();
    struct SV { int ax, x0, y0, x1, y1; size_t ov, gm; uint8_t gv; }; int nv = u32(); std::vector<SV> sv(nv);
    for (auto& v : sv) { v.ax = i32(); v.x0 = i32(); v.y0 = i32(); v.x1 = i32(); v.y1 = i32(); size_t n = size_t(v.x1 - v.x0) * (v.y1 - v.y0); v.ov = o; o += n * 4; v.gm = o; o += n; v.gv = d[o]; o += 1; }
    const uint8_t* heads = &d[o];
    // Text in 8 Felder zerlegen: Zeichen A-Z 0-9, Leerzeichen, {0}..{2} = Plakette (Variante)
    std::vector<int> tok;   // >=0 Zeichen, -1 leer, -10-v Plakette v
    for (size_t i = 0; i < text.size() && tok.size() < 8; ++i) {
        char c = text[i];
        if (c == '{' && i + 2 < text.size() && text[i + 2] == '}' && text[i + 1] >= '0' && text[i + 1] <= '9') { tok.push_back(-10 - (text[i + 1] - '0')); i += 2; continue; }
        char u = (char)toupper((unsigned char)c);
        if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')) tok.push_back(u); else if (u == ' ') tok.push_back(-1);
    }
    while (tok.size() < 8) tok.push_back(-1);
    std::vector<float> body(size_t(W) * H * 4), hg(size_t(W) * H), gl(size_t(W) * H);
    for (size_t i = 0; i < size_t(W) * H; ++i) { for (int k = 0; k < 4; ++k) body[i * 4 + k] = d[ob + i * 4 + k]; hg[i] = d[ob + size_t(W) * H * 4 + i]; gl[i] = d[ob + size_t(W) * H * 5 + i]; }
    for (int s = 0; s < 8 && s < ns; ++s) {
        int tk = tok[s];
        if (tk <= -10) {   // Plakette: an die Feldposition verschoben einblenden
            int v = -10 - tk; if (v >= nv) v = 0; const SV& P = sv[v]; int dx = anc[s] - P.ax, sw = P.x1 - P.x0;
            for (int y = P.y0; y < P.y1; ++y) for (int x = P.x0; x < P.x1; ++x) {
                int px = x + dx; if (px < 0 || px >= W || y < 0 || y >= H) continue;
                size_t i = size_t(y - P.y0) * sw + (x - P.x0), j = size_t(y) * W + px;
                float a = d[P.ov + i * 4 + 3] / 255.f;
                if (a > 0) for (int k = 0; k < 3; ++k) body[j * 4 + k] = d[P.ov + i * 4 + k] * a + body[j * 4 + k] * (1 - a);
                float m = d[P.gm + i] / 255.f; if (m > 0) gl[j] = P.gv * m + gl[j] * (1 - m);
            }
            continue;
        }
        if (tk < 0) continue;
        int gi = -1; for (int i = 0; i < nc; ++i) if (gc[i] == char(tk)) gi = i; if (gi < 0) continue;
        int X = sl[s * 4], Y = sl[s * 4 + 1], w = sl[s * 4 + 2], h = sl[s * 4 + 3];
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int px = X + x, py = Y + y; if (px < 0 || py < 0 || px >= W || py >= H) continue;
            float a = d[go[gi] + size_t(std::min(gh - 1, y * gh / h)) * gw + std::min(gw - 1, x * gw / w)] / 255.f; if (a <= 0) continue;
            size_t j = size_t(py) * W + px;
            for (int k = 0; k < 4; ++k) body[j * 4 + k] = (k < 3 ? ink[k] : 255) * a + body[j * 4 + k] * (1 - a);
            hg[j] = ink[3] * a + hg[j] * (1 - a); gl[j] = ink[4] * a + gl[j] * (1 - a);
        }
    }
    // Kontaktschatten neben Schrift/Wulst und Abdunklung
    std::vector<float> hb(hg), hn(hg);
    { std::thread tn([&] { gauss(hn, W, H, nblur); }); gauss(hb, W, H, aoblur); tn.join(); }   // v4.13: beide Unschaerfen gleichzeitig
    Img dif = mk(W, H, 4);
    for (size_t i = 0; i < size_t(W) * H; ++i) {
        float ao = 1 - aok * std::max(0.f, std::min(1.f, (hb[i] - hg[i]) / 255.f));
        for (int k = 0; k < 3; ++k) dif.p[i * 4 + k] = std::floor(std::min(255.f, body[i * 4 + k] * ao * dim)); dif.p[i * 4 + 3] = body[i * 4 + 3];
    }
    // Normalkarte aus der Hoehe
    Img nrm = mk(W, H, 4);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        float gx = (x > 0 && x < W - 1) ? (hn[size_t(y) * W + x + 1] - hn[size_t(y) * W + x - 1]) / 510.f : 0.f;
        float gy = (y > 0 && y < H - 1) ? (hn[size_t(y + 1) * W + x] - hn[size_t(y - 1) * W + x]) / 510.f : 0.f;
        float nx = -gx * nstr, ny = -gy * nstr, l = std::sqrt(nx * nx + ny * ny + 1);
        nrm.at(x, y, 0) = std::floor((nx / l * 0.5f + 0.5f) * 255); nrm.at(x, y, 1) = std::floor((ny / l * 0.5f + 0.5f) * 255); nrm.at(x, y, 2) = 0; nrm.at(x, y, 3) = 255;
    }
    Img glo = mk(W, H, 4);
    for (size_t i = 0; i < size_t(W) * H; ++i) { float v = lut[uint8_t(gl[i])]; for (int k = 0; k < 4; ++k) glo.p[i * 4 + k] = v; }
    Img cell = resize_to(dif, cw, ch), nr = resize_to(nrm, nw, nh), gs = resize_to(glo, gw_, gh_);
    if (!write_swatch(out[0], heads, cell, true, acols, arows, err)) return false;
    if (!write_swatch(out[1], heads + 83, nr, false, 1, 1, err)) return false;
    if (!write_swatch(out[2], heads + 166, gs, true, 1, 1, err)) return false;
    return true;
}
#ifdef PLED_MAIN
#include <chrono>
int main(int c, char** v) {
    std::string out[3] = {v[3], v[4], v[5]}, err; auto t0 = std::chrono::steady_clock::now();
    bool ok = pled_generate(v[1], v[2], out, err);
    printf("%s %s %.2f s\n", ok ? "ok" : "ERROR", err.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return ok ? 0 : 1;
}
#endif
