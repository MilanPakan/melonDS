// HG-Together camera-experiment. HGSS tekent de overworld ORTHOGRAFISCH (Camera.perspectiveType = 1) met de camera ~50° boven de
// horizon op 1563 eenheden afstand; het "3D"-gevoel komt alleen van die kanteling. HGT_CAM="pitch,fov,zoom":
//   pitch = graden boven de horizon (spel: 50), fov = verticale kijkhoek in graden (0 = orthografisch houden), zoom = 1 houdt de
//   framing op de speler gelijk (192 eenheden = 12 tegels hoog), >1 dichterbij.
// Elk frame: camPos/angle/distance/perspective in de Camera-struct (FieldSystem+0x24) én de projectiematrix in NNS_G3dGlb
// (0x021DA4A4, gevonden door RAM-zoektocht naar de ortho-matrix) overschrijven. Het spel zelf verplaatst pos+target alleen met
// de delta van de speler en stuurt lookAt + projectie elk frame naar de GPU, dus de stand blijft staan.
// shortcut: vast adres van NNS_G3dGlb.projMtx (Sacred Gold v1.05, overworld); upgraden naar een pointer als het ooit verschuift.
#include "HGTCamera.h"
#include "NDS.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace melonDS;

namespace HGTCamera
{
namespace
{
const uint32_t kFieldSysLiteral = 0x0203E304, kProjMtx = 0x021DA4A4;
const double kHalfH = 96, kAspect = 4.0 / 3;   // spel: halve hoogte 96 eenheden op de speler, 256x192
bool parsed = false, on = false; double pitch = 50, fov = 0, zoom = 1;
bool inRam(uint32_t a) { return a >= 0x02000000 && a < 0x02400000; }
int32_t fx(double v) { return (int32_t)lround(v * 4096); }
}

void apply(NDS& nds)
{
    if (!parsed)
    {
        parsed = true; const char* e = getenv("HGT_CAM"); if (e && !*e) e = nullptr;
        on = e && sscanf(e, "%lf,%lf,%lf", &pitch, &fov, &zoom) >= 1 && pitch > 1 && pitch < 89 && fov >= 0 && fov < 120 && zoom > 0;
        if (e) fprintf(stderr, "HGT: camera %s pitch %.0f fov %.0f zoom %.2f\n", on ? "aan" : "ONGELDIG (pitch,fov,zoom)", pitch, fov, zoom);
    }
    if (!on) return;
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral); if (!inRam(var)) return;
    uint32_t fs = nds.ARM9Read32(var);               if (!inRam(fs)) return;
    uint32_t cam = nds.ARM9Read32(fs + 0x24);        if (!inRam(cam)) return;
    // alleen de overworld-camera (ortho, 4:3); cutscenes/menu's met een eigen camera laten we met rust
    if ((int32_t)nds.ARM9Read32(cam + 8) != 5461) return;

    double p = pitch * M_PI / 180, halfH = kHalfH / zoom, tx = (int32_t)nds.ARM9Read32(cam + 0x20) / 4096.0,
           ty = (int32_t)nds.ARM9Read32(cam + 0x24) / 4096.0, tz = (int32_t)nds.ARM9Read32(cam + 0x28) / 4096.0;
    bool persp = fov > 0; double half = fov * M_PI / 360;
    double dist = persp ? halfH / tan(half) : 1563.5;   // ortho: afstand doet niets voor de grootte, alleen voor near/far
    double n = persp ? dist * 0.3 : dist - 600, f = persp ? dist * 6 : dist + 600;
    int32_t m[16] = {0};
    if (persp)
    {
        double cot = 1 / tan(half);
        m[0] = fx(cot / kAspect); m[5] = fx(cot); m[10] = fx((f + n) / (n - f)); m[11] = -4096; m[14] = fx(2 * f * n / (n - f));
        nds.ARM9Write32(cam + 0, fx(sin(half))); nds.ARM9Write32(cam + 4, fx(cos(half)));
    }
    else
    {
        m[0] = fx(1 / (halfH * kAspect)); m[5] = fx(1 / halfH); m[10] = fx(2 / (n - f)); m[14] = fx((f + n) / (n - f)); m[15] = 4096;
        nds.ARM9Write32(cam + 0, fx(halfH / dist)); nds.ARM9Write32(cam + 4, 4096);   // halfH = fovySin/fovyCos * distance
    }
    nds.ARM9Write32(cam + 12, fx(n)); nds.ARM9Write32(cam + 16, fx(f));
    nds.ARM9Write32(cam + 0x14, fx(tx)); nds.ARM9Write32(cam + 0x18, fx(ty + dist * sin(p))); nds.ARM9Write32(cam + 0x1C, fx(tz + dist * cos(p)));
    nds.ARM9Write32(cam + 0x38, fx(dist));
    nds.ARM9Write16(cam + 0x3C, (uint16_t)(65536 - lround(pitch * 65536 / 360)));   // angle.x zoals het spel hem zelf zou zetten
    nds.ARM9Write8(cam + 0x44, persp ? 0 : 1);
    for (int i = 0; i < 16; i++) nds.ARM9Write32(kProjMtx + 4 * i, m[i]);
}
}
