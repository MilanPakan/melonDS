// HG-Together fase 1: leest de spelerpositie uit RAM (zie docs/ram-map.md), wisselt die via UDP uit met één peer
// en tekent de peer als "ghost" over het bovenste scherm (software-renderer framebuffer).
// Config via omgeving: HGT_PORT=eigen UDP-poort, HGT_PEER=host:poort, HGT_LOADSTATE=savestate bij start (optioneel),
// HGT_DUMP_DIR=map voor een PPM van het bovenscherm elke 60 frames (optioneel, voor headless tests).
// shortcut: één peer, geen handshake, plain UDP; upgraden naar ENet/relay zodra de launcher (fase 3) er is.
#include "HGTogether.h"
#include "EmuInstance.h"
#include "NDS.h"
#include "GPU.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace melonDS;

namespace
{
struct State { int32_t mapId, x, z, facing, px, pz; uint32_t frame; };   // px/pz: positionVector (fx32)

const uint32_t kFieldSysLiteral = 0x0203E304;   // literal = &sFieldSysPtr (Sacred Gold / US HG)
const int kPlayerSX = 128, kPlayerSY = 97;       // schermpositie van de eigen speler: midden, voeten op y=97 (gemeten)
const uint32_t kPeerTimeout = 180;               // frames zonder pakket -> ghost verbergen

bool enabled = false, initDone = false;
int sock = -1;
sockaddr_storage peerAddr{}; socklen_t peerLen = 0;
State me{}, peer{}; bool havePeer = false;
uint32_t frame = 0, peerSeen = 0;
std::string dumpDir;

bool inRam(uint32_t a) { return a >= 0x02000000 && a < 0x02400000; }

bool readState(NDS& nds, State& s)
{
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral); if (!inRam(var)) return false;
    uint32_t fs = nds.ARM9Read32(var);               if (!inRam(fs)) return false;
    uint32_t loc = nds.ARM9Read32(fs + 0x20), pa = nds.ARM9Read32(fs + 0x40);
    if (!inRam(loc) || !inRam(pa)) return false;
    uint32_t mo = nds.ARM9Read32(pa + 0x30);         if (!inRam(mo)) return false;
    s.mapId = nds.ARM9Read32(loc);
    s.x = nds.ARM9Read32(mo + 0x64); s.z = nds.ARM9Read32(mo + 0x6C); s.facing = nds.ARM9Read32(mo + 0x28);
    s.px = nds.ARM9Read32(mo + 0x70); s.pz = nds.ARM9Read32(mo + 0x78);
    return true;
}

void init()
{
    initDone = true;
    const char* port = getenv("HGT_PORT"); const char* peerStr = getenv("HGT_PEER");
    if (!port) return;
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me4{}; me4.sin_family = AF_INET; me4.sin_addr.s_addr = htonl(INADDR_ANY); me4.sin_port = htons((uint16_t)atoi(port));
    if (bind(sock, (sockaddr*)&me4, sizeof(me4)) != 0) { fprintf(stderr, "HGT: bind op poort %s mislukt\n", port); return; }
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
#else
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
#endif
    if (peerStr)
    {
        std::string s = peerStr; auto colon = s.rfind(':');
        if (colon == std::string::npos) { fprintf(stderr, "HGT: HGT_PEER moet host:poort zijn\n"); return; }
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; addrinfo* res = nullptr;
        if (getaddrinfo(s.substr(0, colon).c_str(), s.substr(colon + 1).c_str(), &hints, &res) != 0 || !res)
        { fprintf(stderr, "HGT: peer %s niet gevonden\n", peerStr); return; }
        memcpy(&peerAddr, res->ai_addr, res->ai_addrlen); peerLen = (socklen_t)res->ai_addrlen; freeaddrinfo(res);
    }
    if (const char* d = getenv("HGT_DUMP_DIR")) dumpDir = d;
    enabled = true;
    fprintf(stderr, "HGT: actief op poort %s, peer %s\n", port, peerStr ? peerStr : "(geen)");
}

void exchange()
{
    if (peerLen) sendto(sock, (const char*)&me, sizeof(me), 0, (sockaddr*)&peerAddr, peerLen);
    State in{}; sockaddr_storage from{}; socklen_t fromLen = sizeof(from);
    while (recvfrom(sock, (char*)&in, sizeof(in), 0, (sockaddr*)&from, &fromLen) == (int)sizeof(in))
    {
        peer = in; havePeer = true; peerSeen = frame;
        if (!peerLen) { memcpy(&peerAddr, &from, fromLen); peerLen = fromLen; }   // peer onbekend: antwoord naar afzender
        fromLen = sizeof(from);
    }
}

void drawGhost(u32* fb)
{
    // 1 tile = 16 wereld-eenheden (fx32) = 16 px; verticaal benaderd met dezelfde schaal (perspectief later).
    // shortcut: ghost = doorschijnende kopie van de eigen spelersprite (16x24 px rond het midden), dus zelfde
    // geslacht/outfit als jijzelf; echte sprite uit de ROM zodra we OAM-injectie doen.
    int dx = (peer.px - me.px) >> 12, dy = (peer.pz - me.pz) >> 12;
    if (dx == 0 && dy == 0) return;
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 16; x++)
        {
            int sx = kPlayerSX - 8 + x, sy = kPlayerSY - 23 + y, tx = sx + dx, ty = sy + dy;
            if (sx < 0 || sx >= 256 || sy < 0 || sy >= 192 || tx < 0 || tx >= 256 || ty < 0 || ty >= 192) continue;
            u32 src = fb[sy * 256 + sx], dst = fb[ty * 256 + tx];
            fb[ty * 256 + tx] = (((src >> 1) & 0x7F7F7F7F) + ((dst >> 1) & 0x7F7F7F7F)) | 0xFF000000 | 0x00200020;   // 50/50 + paarse zweem
        }
}

void dump(const u32* fb)
{
    char path[512]; snprintf(path, sizeof(path), "%s/frame-%06u.ppm", dumpDir.c_str(), frame);
    FILE* f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n256 192\n255\n");
    for (int i = 0; i < 256 * 192; i++) { unsigned char px[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] }; fwrite(px, 1, 3, f); }
    fclose(f);
}
}

void HGTogether::tick(EmuInstance* emu)
{
    if (!initDone) init();
    if (!enabled || !emu->nds) return;
    frame++;
    if (frame == 2) if (const char* st = getenv("HGT_LOADSTATE")) fprintf(stderr, "HGT: savestate %s %s\n", st, emu->loadState(st) ? "geladen" : "MISLUKT");
    NDS& nds = *emu->nds;
    bool haveMe = readState(nds, me); me.frame = frame;
    if (haveMe) exchange();
    void* top = nullptr; void* bottom = nullptr;
    if (!nds.GPU.GetFramebuffers(&top, &bottom) || !top) return;
    u32* fb = (u32*)top;
    if (haveMe && havePeer && peer.mapId == me.mapId && frame - peerSeen < kPeerTimeout) drawGhost(fb);
    if (!dumpDir.empty() && frame % 60 == 0)
    {
        dump(fb);
        fprintf(stderr, "HGT f%u ik(map %d %d,%d r%d) peer(%s map %d %d,%d r%d)\n", frame, me.mapId, me.x, me.z, me.facing,
                havePeer ? "ok" : "-", peer.mapId, peer.x, peer.z, peer.facing);
    }
}
