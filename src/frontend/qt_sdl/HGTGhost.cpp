// HG-Together fase 1: de ghost tekenen met de sprite (mmodel uit het pakket) en naam van de ander. Frames komen uit het
// ROM-bestand op schijf (NARC a/0/8/1), frame-tabel + timing uit docs/fase1-sprites.md.
// Perspectief: projectie via de spelcamera (Camera op FieldSystem+0x24, zelfde lookAt/fovy als NNS_G3dGlb); occlusie: pixels waar de
// dieptebuffer (W-buffering = kijkdiepte in fx32) dichterbij ligt dan de ghost min een halve tegel.
// OpenGL-renderer: het vlak rond de ghost wordt uit de uitvoertextuur van het bovenscherm teruggelezen (glReadPixels), op de interne
// resolutie (schaal S, nearest-neighbour) overschreven en teruggeschreven, vóór drawScreen op dezelfde emu-thread. Occlusie onder GL:
// de 3D-renderer schrijft gl_FragDepth = DS-diepte / 2^24, dus de dieptetextuur van zijn MainFramebuffer heeft dezelfde schaal als de
// software-dieptebuffer; we lezen het vlak terug en ijken k op dezelfde manier.
// shortcut: ren-cyclus op de eigen frameteller i.p.v. de echte animatiefase; max 8 peers met eigen loopfase.
#include "HGTGhost.h"
#include "NDS.h"
#include "GPU.h"
#include "GPU3D_Soft.h"
#ifdef OGLRENDERER_ENABLED
#include "OpenGLSupport.h"
#include "GPU3D_OpenGL.h"
#endif
#include <vector>
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

// Tekendoel: CPU-framebuffer (BGRA, 256x192, S=1) of een teruggelezen vlak uit de GL-uitvoertextuur (RGBA, schaal S).
// Coördinaten X,Y zijn uitvoerpixels (DS-pixel × S); px dekt [x0,x0+w) × [y0,y0+h).
struct Target { u32* px; int x0, y0, w, h, S; bool rgba; };
u32* at(Target& t, int X, int Y) { X -= t.x0; Y -= t.y0; return X < 0 || Y < 0 || X >= t.w || Y >= t.h ? nullptr : &t.px[Y * t.w + X]; }
u32 rgb(const Target& t, u32 r, u32 g, u32 b) { return 0xFF000000 | (t.rgba ? b << 16 | g << 8 | r : r << 16 | g << 8 | b); }
void block(Target& t, int x, int y, u32 c) { for (int j = 0; j < t.S; j++) for (int i = 0; i < t.S; i++) if (u32* p = at(t, x * t.S + i, y * t.S + j)) *p = c; }   // één DS-pixel

void label(Target& t, const char* name, int cx, int y)
{
    int n = (int)strnlen(name, 8); int x = cx - n * 2;
    for (int i = 0; i < n; i++, x += 4)
    {
        char c = toupper((unsigned char)name[i]); int g = isdigit((unsigned char)c) ? c - '0' : (c >= 'A' && c <= 'Z') ? 10 + c - 'A' : -1;
        if (g < 0) continue;
        for (int r = 0; r < 5; r++) for (int b = 0; b < 3; b++) if (kFont[g][r * 3 + b] == '1') { block(t, x + b + 1, y + r + 1, 0xFF000000); block(t, x + b, y + r, 0xFFFFFFFF); }
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

bool overworld(NDS& nds)
{
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral); if (!inRam(var)) return false;
    uint32_t fs = nds.ARM9Read32(var);               if (!inRam(fs)) return false;
    uint32_t pm = nds.ARM9Read32(fs);                if (!inRam(pm)) return false;
    return !inRam(nds.ARM9Read32(pm + 4));
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
    i.gender = nds.ARM9Read8(prof + 0x18); i.johto = nds.ARM9Read8(prof + 0x1A); i.kanto = nds.ARM9Read8(prof + 0x1F);   // zelfde bytes als HGTNuzlocke
}

void draw(void* top, bool cpu, NDS& nds, int idx, const int32_t me[3], const int32_t pos[3], int facing, uint32_t frame, const Info& peer)
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
    double sc = cam.ortho ? 1 : mw / gw, size = 32 * sc; if (size < 2 || size > 128) return;
    double ox = gx + (kTexX - mx) * sc, oy = gy + (kTexY - my) * sc;   // textuur-oorsprong in DS-pixels
    // Dieptebuffer (W-buffering) = k * kijkdiepte; k ijken op de eigen billboard (torso-pixel), want de hardware-W is ~2.24x de
    // lookAt-diepte (oorzaak niet gevonden). Geen 3D-pixel daar -> geen occlusie. Software: DepthBuffer; OpenGL: dieptetextuur (zie boven).
    auto* r3d = cpu ? dynamic_cast<SoftRenderer3D*>(&nds.GPU.GetRenderer().Get3DRenderer()) : nullptr;
    uint32_t dme = r3d ? r3d->GetDepth(128, 88) : 0;
    Target t{}; std::vector<u32> buf; std::vector<float> depth;   // depth: GL-diepte van het vlak (0..1), leeg = geen GL-occlusie
#ifdef OGLRENDERER_ENABLED
    GLuint glTex = 0; GLint prevFb = 0, prevTex = 0;
#endif
    if (cpu) t = { (u32*)top, 0, 0, 256, 192, 1, false };
    else
    {
#ifdef OGLRENDERER_ENABLED
        glTex = *(GLuint*)top;   // GLRenderer::GetFramebuffers: &FPOutputTex[front], array-textuur, laag 0 = bovenscherm
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevFb); glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevTex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, glTex); GLint W = 0; glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &W);
        int S = std::max(1, (int)W / 256), lab = std::max(0.0, 16 - size / 2) + 2;   // labelbreedte tot 28 px rond het midden
        int x0 = std::max(0, (int)floor((ox - lab) * S)), x1 = std::min(256 * S, (int)ceil((ox + size + lab) * S));
        int y0 = std::max(0, (int)floor((oy - 6) * S)), y1 = std::min(192 * S, (int)ceil((oy + size) * S));
        if (x1 <= x0 || y1 <= y0) { glBindTexture(GL_TEXTURE_2D_ARRAY, prevTex); return; }
        t = { nullptr, x0, y0, x1 - x0, y1 - y0, S, true }; buf.resize((size_t)t.w * t.h); t.px = buf.data();
        static GLuint fbo = 0; if (!fbo) glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, glTex, 0, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glReadPixels(t.x0, t.y0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, t.px);
        if (auto* g3d = dynamic_cast<GLRenderer3D*>(&nds.GPU.GetRenderer().Get3DRenderer()); g3d && g3d->GetScaleFactor() == S)
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g3d->GetMainFramebuffer());
            float me1 = 1; glReadPixels(128 * S + S / 2, 88 * S + S / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &me1);
            dme = (uint32_t)(me1 * 16777216.0);
            depth.resize(buf.size()); glReadPixels(t.x0, t.y0, t.w, t.h, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        }
#else
        return;
#endif
    }

    double k = dme && dme < 0xFFFFFF ? dme / mw : 0;
    uint32_t gdepth = (uint32_t)(k * (gw - 8));   // min een halve tegel: de onderrand van de textuur ligt vóór het positiepunt
    if (frame % 600 == 0 && idx == 0) fprintf(stderr, "HGT: ghost w=%.1f (ik %.1f) schaal %.2f k=%.3f %s x%d\n", gw, mw, sc, k / 4096, cpu ? "software" : depth.empty() ? "opengl zonder diepte" : "opengl", t.S);
    const uint8_t* fr = d + tex + fi * 512; bool mirror = facing == 3; int S = t.S;
    for (int Y = (int)ceil(oy * S); Y < (int)ceil((oy + size) * S); Y++)
        for (int X = (int)ceil(ox * S); X < (int)ceil((ox + size) * S); X++)
        {
            u32* p = cpu && (X < 0 || X >= 256 || Y < 0 || Y >= 192) ? nullptr : at(t, X, Y); if (!p) continue;
            int x = (int)(((X + 0.5) / S - ox) / sc), y = (int)(((Y + 0.5) / S - oy) / sc);   // texel, nearest-neighbour
            if (x < 0 || y < 0 || x > 31 || y > 31) continue; if (mirror) x = 31 - x;
            int b = fr[(y * 32 + x) / 2], c = (x & 1) ? b >> 4 : b & 15; if (!c) continue;
            if (k && (r3d ? r3d->GetDepth(X, Y) : (uint32_t)(depth[(size_t)(p - t.px)] * 16777216.0)) < gdepth) continue;   // iets staat ervoor
            uint16_t col; memcpy(&col, d + pal + c * 2, 2);
            u32 src = rgb(t, (col & 31) * 255 / 31, ((col >> 5) & 31) * 255 / 31, ((col >> 10) & 31) * 255 / 31);
            *p = src;   // ondoorzichtig, net als de eigen sprite (wens Milan 2026-10-21)
        }
    label(t, peer.name, (int)lround(ox + size / 2), (int)lround(oy) - 4);

#ifdef OGLRENDERER_ENABLED
    if (!cpu)
    {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, t.x0, t.y0, 0, t.w, t.h, 1, GL_RGBA, GL_UNSIGNED_BYTE, t.px);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevFb); glBindTexture(GL_TEXTURE_2D_ARRAY, prevTex);
    }
#endif
}
}
