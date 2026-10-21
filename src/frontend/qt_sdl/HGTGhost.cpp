// HG-Together fase 1: de ghost tekenen met de sprite (mmodel uit het pakket) en naam van de ander. Frames komen uit het
// ROM-bestand op schijf (NARC a/0/8/1), frame-tabel + timing uit docs/fase1-sprites.md.
// shortcut: één peer, geen perspectief/occlusie; ren-cyclus op de eigen frameteller i.p.v. de echte animatiefase.
#include "HGTGhost.h"
#include "NDS.h"
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
const int kTexX = 112, kTexY = 69;   // schermpositie van de 32x32-textuur van de eigen speler (gemeten)
Narc narc; bool warned = false;
int32_t lastPx = 0, lastPz = 0; uint32_t lastMove = 0; bool running = false;
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

void readInfo(NDS& nds, Info& i)
{
    i.sprite = (int16_t)mySprite.load(); i.frames = (uint8_t)myFrames.load();
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral); if (!inRam(var)) return;
    uint32_t fs = nds.ARM9Read32(var);               if (!inRam(fs)) return;
    uint32_t sd = nds.ARM9Read32(fs + 0x0C);         if (!inRam(sd)) return;
    uint32_t prof = sd + 0x74;   // PlayerProfile: name u16[8] (0xFFFF-einde), gender @0x18
    for (int k = 0; k < 8; k++) { uint16_t c = nds.ARM9Read16(prof + 2 * k); i.name[k] = c == 0xFFFF ? 0 : dsChar(c); if (!i.name[k]) break; }
    i.gender = nds.ARM9Read8(prof + 0x18);
}

void draw(u32* fb, int dx, int dy, int facing, int32_t px, int32_t pz, uint32_t frame, const Info& peer)
{
    // frame kiezen (hero-index): stilstaan / lopen A,B / rennen; zijwaarts = links, rechts gespiegeld
    static const uint8_t kStand[3] = {0, 21, 2}, kWalk[3][2] = {{20, 9}, {23, 22}, {3, 1}}, kRun[3][4] = {{7, 10, 7, 10}, {11, 12, 11, 12}, {15, 14, 16, 14}};
    int side = facing < 2 ? facing : 2;
    if (px != lastPx || pz != lastPz)
    {
        int d = std::max(std::abs(px - lastPx), std::abs(pz - lastPz)) >> 12, dt = (int)(frame - lastMove);
        if (dt > 0 && dt < 30) running = d >= 2 * dt;   // 1 px/frame = lopen, 2 = rennen
        lastPx = px; lastPz = pz; lastMove = frame;
    }
    bool moving = frame - lastMove < 3;
    int32_t pos = facing < 2 ? pz : px; int pix = (pos >> 12) & 15, tile = (pos >> 16) & 1;
    bool walkHalf = (facing == 1 || facing == 3) ? pix >= 8 : pix < 8;   // eerste 8 px van een tegel = stilstaand frame
    int idx = !moving ? kStand[side] : running ? kRun[side][(frame / 7) & 3] : walkHalf ? kWalk[side][tile] : kStand[side];

    int member = peer.sprite >= 0 ? peer.sprite : (peer.gender ? 70 : 69), frames = peer.sprite >= 0 ? peer.frames : 24;
    if (frames != 24) idx = kNpcMap[idx];
    uint32_t len, tex, pal; const uint8_t* d = narc.file(member, len);
    if (!sheet(d, len, idx + 1, tex, pal)) { if (!warned) { warned = true; fprintf(stderr, "HGT: ghost-sheet mmodel %d onbruikbaar\n", member); } return; }
    const uint8_t* fr = d + tex + idx * 512; bool mirror = facing == 3;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
        {
            int b = fr[(y * 32 + x) / 2], c = (x & 1) ? b >> 4 : b & 15; if (!c) continue;
            uint16_t col; memcpy(&col, d + pal + c * 2, 2);
            u32 src = ((col & 31) * 255 / 31) << 16 | (((col >> 5) & 31) * 255 / 31) << 8 | (((col >> 10) & 31) * 255 / 31);
            int tx = kTexX + dx + (mirror ? 31 - x : x), ty = kTexY + dy + y;
            if (tx < 0 || tx >= 256 || ty < 0 || ty >= 192) continue;
            u32 dst = fb[ty * 256 + tx];
            fb[ty * 256 + tx] = (((src >> 1) & 0x7F7F7F7F) + ((dst >> 1) & 0x7F7F7F7F)) | 0xFF000000 | 0x00200020;   // 50/50 + paarse zweem
        }
    label(fb, peer.name, kTexX + 16 + dx, kTexY + dy - 4);
}
}
