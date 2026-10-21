// HG-Together fase 1: ghost van de vriend met zíjn sprite en naam (docs/fase1-sprites.md). Deelt de NARC/NSBTX-lezer met HGTDock.
#pragma once
#include "types.h"
#include <QFile>
#include <QByteArray>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
namespace melonDS { class NDS; }

namespace HGTGhost
{
#pragma pack(push, 1)
struct Info { char name[8]; uint8_t gender; int16_t sprite; uint8_t frames; int32_t py; };   // sprite = mmodel (-1 = Ethan/Lyra naar geslacht), frames = 12 (NPC) of 24; py = hoogte (fx32)
#pragma pack(pop)
extern std::atomic<int> mySprite, myFrames;   // eigen keuze, gezet door het dock (applySprite)

void init(const std::string& romPath);        // NARC a/0/8/1 uit het ROM-bestand (het dock patcht alleen de cart in het geheugen)
void readInfo(melonDS::NDS& nds, Info& i);     // naam + geslacht uit het spelersprofiel, sprite uit het dock
// Ghost nr. idx tekenen: me/pos = wereldpositie (x,y,z fx32) van jezelf en van de peer; projectie via de spelcamera,
// occlusie via de dieptebuffer van de software-renderer; frame = eigen frameteller (ren-cyclus).
// top/cpu = wat GPU.GetFramebuffers geeft: cpu = BGRA-framebuffer (software), anders &GLuint van de GL-uitvoertextuur.
void draw(void* top, bool cpu, melonDS::NDS& nds, int idx, const int32_t me[3], const int32_t pos[3], int facing, uint32_t frame, const Info& peer);

inline char dsChar(uint16_t c)   // HGSS charmap: digits 0x121, A 0x12B, a 0x145, space 0x1DE
{
    if (c >= 0x121 && c <= 0x12A) return '0' + (c - 0x121);
    if (c >= 0x12B && c <= 0x144) return 'A' + (c - 0x12B);
    if (c >= 0x145 && c <= 0x15E) return 'a' + (c - 0x145);
    if (c == 0x1DE) return ' ';
    return '?';
}

// One NARC from the player's ROM (FNT walk by path), kept in memory.
struct Narc
{
    std::vector<uint8_t> data; std::vector<std::pair<uint32_t, uint32_t>> fat; uint32_t gmif = 0, base = 0;   // base = ROM offset of the NARC
    static uint32_t u32(const QByteArray& b, int o) { uint32_t v; memcpy(&v, b.constData() + o, 4); return v; }
    static uint16_t u16(const QByteArray& b, int o) { uint16_t v; memcpy(&v, b.constData() + o, 2); return v; }

    void load(const QString& romPath, std::initializer_list<const char*> parts)
    {
        QFile f(romPath); if (!f.open(QIODevice::ReadOnly)) return;
        f.seek(0x40); QByteArray hdr = f.read(16); uint32_t fntOff = u32(hdr, 0), fntLen = u32(hdr, 4), fatOff = u32(hdr, 8), fatLen = u32(hdr, 12);
        f.seek(fntOff); QByteArray fnt = f.read(fntLen); f.seek(fatOff); QByteArray fatb = f.read(fatLen);
        int dir = 0xF000;
        for (const char* part : parts)
        {
            uint32_t sub = u32(fnt, (dir & 0xFFF) * 8); int fid = u16(fnt, (dir & 0xFFF) * 8 + 4); int p = sub, next = -1;
            while (true)
            {
                int n = (uint8_t)fnt[p++]; if (!n) break;
                QByteArray name = fnt.mid(p, n & 0x7F); p += n & 0x7F;
                if (n & 0x80) { int s = u16(fnt, p); p += 2; if (name == part) { next = s; break; } }
                else { if (name == part) { next = fid; break; } fid++; }
            }
            if (next < 0) return;
            dir = next;
        }
        uint32_t s = u32(fatb, dir * 8), e = u32(fatb, dir * 8 + 4);
        f.seek(s); QByteArray d = f.read(e - s); if (!d.startsWith("NARC")) return;
        base = s;
        uint32_t n = u32(d, 0x18);
        for (uint32_t i = 0; i < n; i++) fat.push_back({u32(d, 0x1C + i * 8), u32(d, 0x20 + i * 8)});
        uint32_t btnf = 0x1C + n * 8; gmif = btnf + u32(d, btnf + 4) + 8;
        data.assign(d.begin(), d.end());
    }

    const uint8_t* file(int i, uint32_t& len) const { if (i < 0 || i >= (int)fat.size()) { len = 0; return nullptr; } len = fat[i].second - fat[i].first; return data.data() + gmif + fat[i].first; }
    uint32_t romOffset(int i) const { return base + gmif + fat[i].first; }
};

// NSBTX sprite sheet (a/0/8/1): offsets (from the member start) of the 32x32 4bpp frame data and the 16-colour palette;
// frames = how many 512-byte frames must fit.
inline bool sheet(const uint8_t* d, uint32_t len, int frames, uint32_t& tex, uint32_t& pal)
{
    if (!d || len < 0x60 || memcmp(d, "BTX0", 4)) return false;
    uint32_t t; memcpy(&t, d + 0x10, 4);                        // first block = TEX0
    if (t + 0x40 > len || memcmp(d + t, "TEX0", 4)) return false;
    uint32_t texOff, palOff; memcpy(&texOff, d + t + 0x14, 4); memcpy(&palOff, d + t + 0x38, 4);
    tex = t + texOff; pal = t + palOff;
    return tex + frames * 512 <= len && pal + 32 <= len;
}
// Hero frame index -> NPC-sheet frame. Hero: up stand 0, walk 20/9, run 7/10 (8 same group); down stand 21, walk 23/22,
// run 11/12/13; side stand 2, walk 3/1, run 15/14/16 (17-19, 4-6 same group). NPC: up 0/8/7, side 2/3/1, down 9/10/11.
const uint8_t kNpcMap[24] = {0, 1, 2, 3, 2, 2, 2, 8, 7, 7, 7, 10, 11, 10, 2, 3, 1, 2, 3, 1, 8, 9, 11, 10};
}
