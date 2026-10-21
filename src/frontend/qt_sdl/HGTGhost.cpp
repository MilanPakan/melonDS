// HG-Together fase 1: de ghost tekenen met de sprite (mmodel uit het pakket) en naam van de ander. Frames komen uit het
// ROM-bestand op schijf (NARC a/0/8/1), frame-tabel + timing uit docs/fase1-sprites.md.
// Perspectief: projectie via de spelcamera (Camera op FieldSystem+0x24, zelfde lookAt/fovy als NNS_G3dGlb); occlusie: pixels waar de
// dieptebuffer (W-buffering = kijkdiepte in fx32) dichterbij ligt dan de ghost min een halve tegel.
// shortcut: ren-cyclus op de eigen frameteller i.p.v. de echte animatiefase; max 8 peers met eigen loopfase.
#include "HGTGhost.h"
#include "NDS.h"
#include "GPU.h"
#include "GPU3D_Soft.h"
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>

using namespace melonDS;

namespace HGTGhost
{
std::atomic<int> mySprite{-1}, myFrames{24};

namespace
{
const uint32_t kFieldSysLiteral = 0x0203E304;
const int kTexX = 112, kTexY = 69;   // schermpositie van de 32x32-textuur van de eigen speler (gemeten) -> ijkt de offset t.o.v. het geprojecteerde punt
Narc narc; bool warned = false;
struct Anim { int32_t px = 0, pz = 0; uint32_t lastMove = 0; bool running = false; } anim[8];
bool inRam(uint32_t a) { return a >= 0x02000000 && a < 0x02400000; }

// 3x5-font, rijen van boven naar beneden, 0-9 dan A-Z
const char* kFont[36] = {
    "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001", "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111",
    "010101111101101", "110101110101110", "111100100100111", "110101101101110", "111100111100111", "111100111100100", "111100101101111", "101101111101101", "111010010010111", "001001001101111",
    "101101110101101", "100100100100111", "101111111101101", "110101101101101", "111101101101111", "111101111100100", "111101101111001", "110101110101101", "111100111001111", "111010010010010",
    "101101101101111", "101101101101010", "101101111111101", "101101010101101", "101101010010010", "111001010100111" };

void put(u32* fb, int x, int y, u32 c) { if (x >= 0 && x < 256 && y >= 0 && y < 192) fb[y * 256 + x] = c; }

void label(u32* fb, const char* name, int cx, int y)
{
    int n = (int)strnlen(name, 8); int x = cx - n * 2;
    for (int i = 0; i < n; i++, x += 4)
    {
        char c = toupper((unsigned char)name[i]); int g = isdigit((unsigned char)c) ? c - '0' : (c >= 'A' && c <= 'Z') ? 10 + c - 'A' : -1;
        if (g < 0) continue;
        for (int r = 0; r < 5; r++) for (int b = 0; b < 3; b++) if (kFont[g][r * 3 + b] == '1') { put(fb, x + b + 1, y + r + 1, 0xFF000000); put(fb, x + b, y + r, 0xFFFFFFFF); }
    }
}
}

void init(const std::string& romPath)
{
    narc.load(QString::fromStdString(romPath), {"a", "0", "8", "1"});
    fprintf(stderr, "HGT: ghost-sprites %s (%zu members)\n", narc.data.empty() ? "NIET geladen" : "geladen", narc.fat.size());
}


namespace
{
struct Cam { double pos[3], f[3], s[3], u[3], cot, aspect, halfH; bool ortho; };
void v3(NDS& nds, uint32_t a, double* o) { for (int k = 0; k < 3; k++) o[k] = (int32_t)nds.ARM9Read32(a + 4 * k) / 4096.0; }
void cross(const double* a, const double* b, double* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
double dot(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool norm(double* a) { double l = sqrt(dot(a, a)); if (l < 1e-9) return false; for (int k = 0; k < 3; k++) a[k] /= l; return true; }

bool readCam(NDS& nds, Cam& c)   // Camera: perspective {fovySin, fovyCos, aspect, near, far} @0, lookAt {pos, target, up} @0x14
{
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral); if (!inRam(var)) return false;
    uint32_t fs = nds.ARM9Read32(var);               if (!inRam(fs)) return false;
    uint32_t cam = nds.ARM9Read32(fs + 0x24);        if (!inRam(cam)) return false;
    int32_t fsin = nds.ARM9Read32(cam), fcos = nds.ARM9Read32(cam + 4), asp = nds.ARM9Read32(cam + 8);
    if (fsin <= 0 || asp <= 0) return false;
    c.cot = (double)fcos / fsin; c.aspect = asp / 4096.0;
    c.ortho = nds.ARM9Read8(cam + 0x44) == 1; c.halfH = (double)fsin / fcos * ((int32_t)nds.ARM9Read32(cam + 0x38) / 4096.0);   // ortho: halve hoogte = tan * distance
    double target[3], up[3]; v3(nds, cam + 0x14, c.pos); v3(nds, cam + 0x20, target); v3(nds, cam + 0x2C, up);
    for (int k = 0; k < 3; k++) c.f[k] = target[k] - c.pos[k];
    if (!norm(c.f)) return false;
    cross(c.f, up, c.s); if (!norm(c.s)) return false; cross(c.s, c.f, c.u);
    return true;
}
bool project(const Cam& c, const int32_t* p, double& sx, double& sy, double& w)   // w = kijkdiepte (wereld-eenheden, 16 per tegel)
{
    double v[3] = { p[0] / 4096.0 - c.pos[0], p[1] / 4096.0 - c.pos[1], p[2] / 4096.0 - c.pos[2] };
    w = dot(c.f, v); if (w <= 0.01) return false;
    if (c.ortho) { sx = 128 + 128 * dot(c.s, v) / (c.halfH * c.aspect); sy = 96 - 96 * dot(c.u, v) / c.halfH; return true; }
    sx = 128 + 128 * dot(c.s, v) * c.cot / c.aspect / w;
    sy = 96 - 96 * dot(c.u, v) * c.cot / w;
    return true;
}
}

void readInfo(NDS& nds, Info& i)
{
    i.sprite = (int16_t)mySprite.load(); i.frames = (uint8_t)myFrames.load();
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral); if (!inRam(var)) return;
    uint32_t fs = nds.ARM9Read32(var);               if (!inRam(fs)) return;
    uint32_t pa = nds.ARM9Read32(fs + 0x40), mo = inRam(pa) ? nds.ARM9Read32(pa + 0x30) : 0;
    if (inRam(mo)) i.py = nds.ARM9Read32(mo + 0x74);
    uint32_t sd = nds.ARM9Read32(fs + 0x0C);         if (!inRam(sd)) return;
    uint32_t prof = sd + 0x74;   // PlayerProfile: name u16[8] (0xFFFF-einde), gender @0x18
    for (int k = 0; k < 8; k++) { uint16_t c = nds.ARM9Read16(prof + 2 * k); i.name[k] = c == 0xFFFF ? 0 : dsChar(c); if (!i.name[k]) break; }
    i.gender = nds.ARM9Read8(prof + 0x18);
}

void draw(u32* fb, NDS& nds, int idx, const int32_t me[3], const int32_t pos[3], int facing, uint32_t frame, const Info& peer)
{
    if (idx < 0 || idx >= 8) return;
    int32_t px = pos[0], pz = pos[2];
    // frame kiezen (hero-index): stilstaan / lopen A,B / rennen; zijwaarts = links, rechts gespiegeld
    static const uint8_t kStand[3] = {0, 21, 2}, kWalk[3][2] = {{20, 9}, {23, 22}, {3, 1}}, kRun[3][4] = {{7, 10, 7, 10}, {11, 12, 11, 12}, {15, 14, 16, 14}};
    int side = facing < 2 ? facing : 2; Anim& an = anim[idx];
    if (px != an.px || pz != an.pz)
    {
        int d = std::max(std::abs(px - an.px), std::abs(pz - an.pz)) >> 12, dt = (int)(frame - an.lastMove);
        if (dt > 0 && dt < 30) an.running = d >= 2 * dt;   // 1 px/frame = lopen, 2 = rennen
        an.px = px; an.pz = pz; an.lastMove = frame;
    }
    bool moving = frame - an.lastMove < 3;
    int32_t along = facing < 2 ? pz : px; int pix = (along >> 12) & 15, tile = (along >> 16) & 1;
    bool walkHalf = (facing == 1 || facing == 3) ? pix >= 8 : pix < 8;   // eerste 8 px van een tegel = stilstaand frame
    int fi = !moving ? kStand[side] : an.running ? kRun[side][(frame / 7) & 3] : walkHalf ? kWalk[side][tile] : kStand[side];

    int member = peer.sprite >= 0 ? peer.sprite : (peer.gender ? 70 : 69), frames = peer.sprite >= 0 ? peer.frames : 24;
    if (frames != 24) fi = kNpcMap[fi];
    uint32_t len, tex, pal; const uint8_t* d = narc.file(member, len);
    if (!sheet(d, len, fi + 1, tex, pal)) { if (!warned) { warned = true; fprintf(stderr, "HGT: ghost-sheet mmodel %d onbruikbaar\n", member); } return; }

    // projectie: eigen speler ijkt de textuur-offset en de schaal (1 texel = 1 px op de diepte van de speler)
    Cam cam; double mx, my, mw, gx, gy, gw;
    if (!readCam(nds, cam) || !project(cam, me, mx, my, mw) || !project(cam, pos, gx, gy, gw)) return;
    double sc = cam.ortho ? 1 : mw / gw; int size = (int)lround(32 * sc); if (size < 2 || size > 128) return;
    int ox = (int)lround(gx + (kTexX - mx) * sc), oy = (int)lround(gy + (kTexY - my) * sc);
    // Dieptebuffer (W-buffering) = k * kijkdiepte; k ijken op de eigen billboard (torso-pixel), want de hardware-W is ~2.24x de
    // lookAt-diepte (oorzaak niet gevonden; NNS_G3dGlbPerspective zou scaleW=1 geven). Geen 3D-pixel daar -> geen occlusie.
    auto* r3d = dynamic_cast<SoftRenderer3D*>(&nds.GPU.GetRenderer().Get3DRenderer());
    uint32_t dme = r3d ? r3d->GetDepth(128, 88) : 0; double k = dme && dme < 0xFFFFFF ? dme / mw : 0;
    uint32_t gdepth = (uint32_t)(k * (gw - 8));   // min een halve tegel: de onderrand van de textuur ligt vóór het positiepunt
    if (frame % 600 == 0 && idx == 0) fprintf(stderr, "HGT: ghost w=%.1f (ik %.1f) schaal %.2f k=%.3f\n", gw, mw, sc, k / 4096);
    const uint8_t* fr = d + tex + fi * 512; bool mirror = facing == 3;
    for (int ty = 0; ty < size; ty++)
        for (int tx = 0; tx < size; tx++)
        {
            int sx = ox + tx, sy = oy + ty; if (sx < 0 || sx >= 256 || sy < 0 || sy >= 192) continue;
            int x = (int)(tx / sc), y = (int)(ty / sc); if (x > 31 || y > 31) continue; if (mirror) x = 31 - x;
            int b = fr[(y * 32 + x) / 2], c = (x & 1) ? b >> 4 : b & 15; if (!c) continue;
            if (k && r3d->GetDepth(sx, sy) < gdepth) continue;   // iets staat ervoor
            uint16_t col; memcpy(&col, d + pal + c * 2, 2);
            u32 src = ((col & 31) * 255 / 31) << 16 | (((col >> 5) & 31) * 255 / 31) << 8 | (((col >> 10) & 31) * 255 / 31);
            u32 dst = fb[sy * 256 + sx];
            fb[sy * 256 + sx] = (((src >> 1) & 0x7F7F7F7F) + ((dst >> 1) & 0x7F7F7F7F)) | 0xFF000000 | 0x00200020;   // 50/50 + paarse zweem
        }
    label(fb, peer.name, ox + size / 2, oy - 4);
}
}
