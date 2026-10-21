// HG-Together fase 1: leest de spelerpositie uit RAM (zie docs/ram-map.md), wisselt die via UDP uit met één peer
// en tekent de peer als "ghost" over het bovenste scherm (software-renderer framebuffer).
// Config via omgeving: HGT_PORT=eigen UDP-poort, HGT_PEER=host:poort, HGT_LOADSTATE=savestate bij start (optioneel),
// HGT_DUMP_DIR=map voor een PPM van het bovenscherm elke 60 frames (optioneel, voor headless tests).
// shortcut: één peer, geen handshake, plain UDP; upgraden naar ENet/relay zodra de launcher (fase 3) er is.
#include "HGTogether.h"
#include "HGTGhost.h"
#include "HGTCamera.h"
#include "EmuInstance.h"
#include "NDS.h"
#include "GPU.h"
#include "Config.h"
#include "net/MPInterface.h"
#include "net/LAN.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>   // open/read voor de HGT_CTL-pijp; op Windows alleen compileerbaar, een FIFO bestaat daar niet
#define O_NONBLOCK 0
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
struct State { int32_t mapId, x, z, facing, px, pz; uint32_t frame; HGTGhost::Info info; };   // px/pz: positionVector (fx32)

const uint32_t kFieldSysLiteral = 0x0203E304;   // literal = &sFieldSysPtr (Sacred Gold / US HG)
const uint32_t kPeerTimeout = 180;               // frames zonder pakket -> ghost verbergen

bool enabled = false, initDone = false;
int sock = -1;
sockaddr_storage peerAddr{}; socklen_t peerLen = 0;
State me{};
struct Peer { sockaddr_storage addr; socklen_t len; State st; uint32_t seen; };
std::vector<Peer> peers;                                   // iedereen van wie we een positiepakket kregen
std::vector<std::pair<sockaddr_storage, socklen_t>> sendTo;   // alle HGT_PEER-adressen (komma-gescheiden); peerAddr = de eerste (battle-sein)
// Co-op trainerbattle (bench/hgtpatch.py): de ROM-hook zet HGT.signal = trainerId op kSignal; wij sturen dat als
// BattleMsg naar de ander, die daarop het joiner-script (HGT_JOIN_SCRIPT, standaard 9019) injecteert via de mailbox.
struct BattleMsg { char tag[4]; uint32_t trainer; };
const uint32_t kSignal = 0x02078B04;
BattleMsg peerBattle{}; uint32_t peerBattleSeen = 0, lastJoin = 0;
// Is de hook er echt (scriptcommando 213 -> onze TB-cave)? Zonder hook (ongepatchte ROM/andere states) staat op kSignal
// gewone code: die niet als sein lezen en ook geen joiner-script injecteren (9019 bestaat dan niet).
const uint32_t kCmd213 = 0x020FB054, kCaveLo = 0x02078834, kCaveHi = 0x02078B00;
// club-struct van overlay 3 (pointer op 0x022598A0, alleen gezet zolang de club-UI draait); +0x88 = retCode, 2 = verbonden
bool clubConnected(NDS& nds) { uint32_t c = nds.ARM9Read32(0x022598A0); return c >= 0x02000000 && c < 0x02400000 && nds.ARM9Read32(c + 0x88) == 2; }
bool hookOn(NDS& nds) { uint32_t tb = nds.ARM9Read32(kCmd213); return tb > kCaveLo && tb < kCaveHi; }
// Club-UI automatisch: na het sein (leader) of de injectie (joiner) elke 40 frames A, tot de club verbonden is,
// maximaal kClubFrames.
const uint32_t kClubFrames = 1500; int clubRole = 0; uint32_t clubUntil = 0; u32 autoKeyFrames = 0;
uint32_t frame = 0;
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
    HGTGhost::readInfo(nds, s.info);
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
    for (std::string all = peerStr ? peerStr : ""; !all.empty();)
    {
        size_t comma = all.find(','); std::string s = all.substr(0, comma); all = comma == std::string::npos ? "" : all.substr(comma + 1);
        auto colon = s.rfind(':');
        if (colon == std::string::npos) { fprintf(stderr, "HGT: HGT_PEER moet host:poort[,host:poort] zijn\n"); return; }
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; addrinfo* res = nullptr;
        if (getaddrinfo(s.substr(0, colon).c_str(), s.substr(colon + 1).c_str(), &hints, &res) != 0 || !res)
        { fprintf(stderr, "HGT: peer %s niet gevonden\n", s.c_str()); return; }
        sockaddr_storage a{}; memcpy(&a, res->ai_addr, res->ai_addrlen); sendTo.push_back({a, (socklen_t)res->ai_addrlen}); freeaddrinfo(res);
        if (!peerLen) { peerAddr = a; peerLen = sendTo.back().second; }
    }
    if (const char* d = getenv("HGT_DUMP_DIR")) dumpDir = d;
    enabled = true;
    fprintf(stderr, "HGT: actief op poort %s, peer %s\n", port, peerStr ? peerStr : "(geen)");
}

void exchange()
{
    for (auto& a : sendTo) sendto(sock, (const char*)&me, sizeof(me), 0, (sockaddr*)&a.first, a.second);
    char buf[64]; sockaddr_storage from{}; socklen_t fromLen = sizeof(from); int n;
    while ((n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen)) > 0)
    {
        if (n == (int)sizeof(State))
        {
            Peer* p = nullptr; for (auto& q : peers) if (q.len == fromLen && !memcmp(&q.addr, &from, fromLen)) p = &q;
            if (!p) { peers.push_back({from, fromLen, {}, 0}); p = &peers.back(); }
            memcpy(&p->st, buf, sizeof(State)); p->seen = frame;
        }
        else if (n == (int)sizeof(BattleMsg) && memcmp(buf, "HGTB", 4) == 0) { memcpy(&peerBattle, buf, sizeof(BattleMsg)); peerBattleSeen = frame; }
        if (!peerLen) { memcpy(&peerAddr, &from, fromLen); peerLen = fromLen; sendTo.push_back({from, fromLen}); }   // peer onbekend: antwoord naar afzender
        fromLen = sizeof(from);
    }
}

void drawGhosts(void* top, bool cpu, NDS& nds)
{
    int32_t mine[3] = { me.px, me.info.py, me.pz };
    for (size_t i = 0; i < peers.size(); i++)
    {
        const State& p = peers[i].st; if (p.mapId != me.mapId || frame - peers[i].seen >= kPeerTimeout) continue;
        int32_t pos[3] = { p.px, p.info.py, p.pz };
        HGTGhost::draw(top, cpu, nds, (int)i, mine, pos, p.facing, frame, p.info);   // sprite + naam van de ander (HGTGhost.cpp)
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

// HGT_CTL=<fifo>: stuurkanaal voor tests zonder mens. Regels: "k MASK N" (toetsen MASK = bits A1 B2 Sel4 St8 R16 L32 U64 D128
// R256 L512 X1024 Y2048, N frames vast), "t X Y N" (touch N frames), "w N" (wachten), "s PAD" (screenshot boven+onder
// als 256x384 PPM), "r ADDR LEN PAD" (hex-dump RAM). Commando's lopen op volgorde, één tegelijk.
std::vector<u8> hexBytes(const std::string& h);
int ctlFd = -1; std::string ctlBuf; std::deque<std::string> ctlQ; u32 ctlKeys = 0, ctlFrames = 0; int ctlTouchX = -1, ctlTouchY = -1;
void ctlPoll()
{
    if (ctlFd < 0) return;
    char b[1024]; ssize_t n;
    while ((n = read(ctlFd, b, sizeof(b))) > 0) ctlBuf.append(b, n);
    size_t nl;
    while ((nl = ctlBuf.find('\n')) != std::string::npos) { ctlQ.push_back(ctlBuf.substr(0, nl)); ctlBuf.erase(0, nl + 1); }
}
void ctlShot(NDS& nds, const char* path)
{
    void* top = nullptr; void* bottom = nullptr;
    if (!nds.GPU.GetFramebuffers(&top, &bottom) || !top || !bottom) return;
    std::string tmp = std::string(path) + ".tmp"; FILE* f = fopen(tmp.c_str(), "wb"); if (!f) return;
    fprintf(f, "P6\n256 384\n255\n");
    for (const u32* fb : { (const u32*)top, (const u32*)bottom })
        for (int i = 0; i < 256 * 192; i++) { unsigned char px[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] }; fwrite(px, 1, 3, f); }
    fclose(f); rename(tmp.c_str(), path);
}
void ctlRead(NDS& nds, u32 addr, u32 len, const char* path)
{
    std::string tmp = std::string(path) + ".tmp"; FILE* f = fopen(tmp.c_str(), "w"); if (!f) return;
    for (u32 i = 0; i < len; i++) fprintf(f, "%02X", nds.ARM9Read8(addr + i));
    fprintf(f, "\n"); fclose(f); rename(tmp.c_str(), path);
}
// "c ADDR A0 A1 A2 A3 PAD": ROM-functie aanroepen via de mailbox-cave (bench/multicave.py hangt een bl in
// FieldSystem_Control, dus de aanroep gebeurt elk overworld-frame vanuit de hoofdlus van het spel zelf).
// Mailbox: [0]=functie (0 = vrij) [4..16]=r0-r3 [20]=r0-resultaat [24]=klaar-vlag.
const u32 kMailbox = 0x020511D0;
struct Call { u32 addr, a[4]; std::string out; bool pending = false, active = false; u32 frames = 0; } call;
void callStart(NDS& nds)
{
    for (int i = 0; i < 4; i++) nds.ARM9Write32(kMailbox + 4 + 4 * i, call.a[i]);
    nds.ARM9Write32(kMailbox + 24, 0); nds.ARM9Write32(kMailbox, call.addr | 1);
    call.pending = false; call.active = true; call.frames = 0;
}
bool callPoll(NDS& nds)   // true = klaar
{
    if (!nds.ARM9Read32(kMailbox + 24))
    {
        if (++call.frames < 1800) return false;   // na 30 s opgeven (hook draait niet: geen overworld?) zodat de wachtrij doorloopt
        fprintf(stderr, "HGT: call %08X TIMEOUT (hook actief?)\n", call.addr); nds.ARM9Write32(kMailbox, 0);
        std::string tmp = call.out + ".tmp"; FILE* f = call.out.empty() ? nullptr : fopen(tmp.c_str(), "w"); if (f) { fprintf(f, "TIMEOUT\n"); fclose(f); rename(tmp.c_str(), call.out.c_str()); }
        call.active = false; return true;
    }
    u32 r0 = nds.ARM9Read32(kMailbox + 20); nds.ARM9Write32(kMailbox + 24, 0);
    std::string tmp = call.out + ".tmp"; FILE* f = call.out.empty() ? nullptr : fopen(tmp.c_str(), "w");
    if (f) { fprintf(f, "%08X\n", r0); fclose(f); rename(tmp.c_str(), call.out.c_str()); }
    fprintf(stderr, "HGT: call %08X -> r0=%08X (f%u)\n", call.addr, r0, call.frames);
    call.active = false; return true;
}
bool ctlStep(NDS& nds)   // true = er loopt een gescript commando
{
    if (ctlFd < 0) { if (call.active) callPoll(nds); else if (call.pending) callStart(nds); return false; }
    ctlPoll();
    if (call.active && !callPoll(nds)) return false;
    if (call.pending) { callStart(nds); return false; }
    while (ctlFrames == 0 && !ctlQ.empty())
    {
        std::string l = ctlQ.front(); ctlQ.pop_front();
        char c = l.empty() ? 0 : l[0]; const char* a = l.c_str() + 1;
        u32 x = 0, y = 0, n = 0; char path[512] = {0};
        ctlTouchX = ctlTouchY = -1; ctlKeys = 0;
        if (c == 'k' && sscanf(a, "%u %u", &x, &n) == 2) { ctlKeys = x; ctlFrames = n; }
        else if (c == 't' && sscanf(a, "%u %u %u", &x, &y, &n) == 3) { ctlTouchX = x; ctlTouchY = y; ctlFrames = n; }
        else if (c == 'w' && sscanf(a, "%u", &n) == 1) ctlFrames = n;
        else if (c == 's' && sscanf(a, "%511s", path) == 1) ctlShot(nds, path);
        else if (c == 'r' && sscanf(a, "%x %u %511s", &x, &n, path) == 3) ctlRead(nds, x, n, path);
        else if (c == 'x' && sscanf(a, "%511s", path) == 1)   // registers + stack (via de CPU, dus ook DTCM): waar hangt de ARM9?
        {
            std::string tmp = std::string(path) + ".tmp"; FILE* f = fopen(tmp.c_str(), "w");
            if (f) { for (int i = 0; i < 16; i++) fprintf(f, "R%d=%08X\n", i, nds.ARM9.R[i]); fprintf(f, "CPSR=%08X\n", nds.ARM9.CPSR);
                // geheugen lezen zonder de CPU (DataRead32 gaat via de MPU en veroorzaakte zelf data aborts in het spel):
                // DTCM rechtstreeks uit de buffer, de rest via de bus
                auto rd = [&](u32 a) -> u32 { if ((a & nds.ARM9.DTCMMask) == nds.ARM9.DTCMBase) { u32 v; memcpy(&v, nds.ARM9.DTCM + (a & 0x3FFC), 4); return v; }
                                             return (a >= 0x02000000 && a < 0x03000000) ? nds.ARM9Read32(a) : 0; };
                for (u32 i = 0; i < 64; i++) fprintf(f, "SP+%02X=%08X\n", 4 * i, rd(nds.ARM9.R[13] + 4 * i));
                // gebankte registers: in abort-modus staan hier R13/R14 (SP/LR) van de code die crashte (melonDS wisselt ze om)
                for (int i = 0; i < 3; i++) fprintf(f, "ABT%d=%08X\nSVC%d=%08X\nIRQ%d=%08X\n", i, nds.ARM9.R_ABT[i], i, nds.ARM9.R_SVC[i], i, nds.ARM9.R_IRQ[i]);
                for (u32 i = 0; i < 64; i++) fprintf(f, "ASP+%02X=%08X\n", 4 * i, rd(nds.ARM9.R_ABT[0] + 4 * i));
                fclose(f); rename(tmp.c_str(), path); }
        }
        else if (c == 'p' && sscanf(a, "%x %511s", &x, path) == 2) { std::vector<u8> v = hexBytes(path); for (size_t i = 0; i < v.size(); i++) nds.ARM9Write8(x + i, v[i]); }
        else if (c == 'c' && sscanf(a, "%x %x %x %x %x %511s", &x, &call.a[0], &call.a[1], &call.a[2], &call.a[3], path) == 6) { call.addr = x; call.out = path; call.pending = true; break; }
        else if (!l.empty()) fprintf(stderr, "HGT: ctl '%s' ongeldig\n", l.c_str());
    }
    if (ctlFrames == 0) return false;
    ctlFrames--;
    nds.SetKeyMask(0xFFF & ~ctlKeys);
    if (ctlTouchX >= 0) nds.TouchScreen(ctlTouchX, ctlTouchY); else nds.ReleaseScreen();
    return true;
}

std::vector<HGTogether::PeerStatus> HGTogether::peerStatus()
{
    // shortcut: geen lock; peers groeit alleen bij een nieuwe peer en het dock leest hooguit 4x per seconde; lock zodra het gaat kraken
    std::vector<PeerStatus> v;
    for (const Peer& p : peers)
        v.push_back({ std::string(p.st.info.name, strnlen(p.st.info.name, 8)), p.st.info.gender, p.st.info.sprite, p.st.info.frames, (frame - p.seen) / 60.0f, frame - p.seen < kPeerTimeout, p.st.info.johto, p.st.info.kanto });
    return v;
}

bool HGTogether::input(EmuInstance* emu)
{
    if (!emu->nds) return false;
    if (ctlFd == -1) { const char* p = getenv("HGT_CTL"); ctlFd = (p && *p) ? open(p, O_RDONLY | O_NONBLOCK) : -2; if (p && *p) fprintf(stderr, "HGT: ctl %s %s\n", p, ctlFd >= 0 ? "open" : "MISLUKT"); }
    if (ctlStep(*emu->nds)) return true;   // ook zonder HGT_CTL: ctlStep handelt dan de mailbox-calls af (joiner-injectie)
    if (!autoKeyFrames) return false;
    autoKeyFrames--; emu->nds->SetKeyMask(0xFFF & ~1u); emu->nds->ReleaseScreen(); return true;   // club-A (zie clubRole)
}

// HGT_POKES="020514B8=0FE0;0223E6D2=A9>A8": bytes in RAM overschrijven (ROM-code patchen voor fase 4-experimenten).
// "ADDR=NEW" schrijft één keer (frame 3, arm9 staat dan vast); "ADDR=OLD>NEW" schrijft elk frame zodra er OLD staat,
// nodig voor overlays (bv. battle-overlay 12) die het spel later laadt en daarmee onze bytes overschrijft.
struct Poke { uint32_t addr; std::string oldHex, newHex; };
std::vector<Poke> pokes; bool pokesParsed = false;
std::vector<u8> hexBytes(const std::string& h) { std::vector<u8> v; for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((u8)strtoul(h.substr(i, 2).c_str(), nullptr, 16)); return v; }
void parsePokes()
{
    pokesParsed = true; const char* env = getenv("HGT_POKES"); if (!env) return;
    std::string all(env);
    for (size_t p = 0; p < all.size();)
    {
        size_t e = all.find(';', p); if (e == std::string::npos) e = all.size();
        std::string tok = all.substr(p, e - p); p = e + 1;
        size_t eq = tok.find('='), gt = tok.find('>');
        Poke pk{}; if (eq != std::string::npos) { pk.addr = strtoul(tok.substr(0, eq).c_str(), nullptr, 16);
            pk.newHex = tok.substr(gt == std::string::npos ? eq + 1 : gt + 1); if (gt != std::string::npos) pk.oldHex = tok.substr(eq + 1, gt - eq - 1); }
        bool ok = eq != std::string::npos && inRam(pk.addr) && !pk.newHex.empty() && pk.newHex.size() % 2 == 0 && (pk.oldHex.empty() || pk.oldHex.size() == pk.newHex.size());
        if (!ok) { fprintf(stderr, "HGT: poke '%s' ongeldig\n", tok.c_str()); continue; }
        pokes.push_back(pk);
    }
}
void applyPokes(NDS& nds)
{
    if (!pokesParsed) parsePokes();
    if (frame % 600 == 0) for (const Poke& pk : pokes)   // read-back ter controle (elke 10 s)
    {
        std::string cur; char b[3];
        for (size_t i = 0; i < pk.newHex.size() / 2; i++) { snprintf(b, sizeof(b), "%02X", nds.ARM9Read8(pk.addr + i)); cur += b; }
        fprintf(stderr, "HGT: poke-check %08X = %s (gewenst %s)\n", pk.addr, cur.c_str(), pk.newHex.c_str());
    }
    for (const Poke& pk : pokes)
    {
        std::vector<u8> nw = hexBytes(pk.newHex);
        // zonder OLD: elk frame schrijven tot frame 1200, zodat de bytes ook een koude boot overleven (arm9 wordt pas
        // na de logo's uit de ROM geladen); daarna niet meer, zodat de mailbox vrij blijft
        // (vanaf frame 240: de arm9 van HG pakt zichzelf in de eerste frames uit, eerder schrijven geeft een wit scherm)
        if (pk.oldHex.empty()) { if (frame < 240 || frame > 1200) continue; bool same = true;
            for (size_t i = 0; i < nw.size() && same; i++) same = nds.ARM9Read8(pk.addr + i) == nw[i];
            if (same) continue; }
        else { std::vector<u8> old = hexBytes(pk.oldHex); bool match = true;
            for (size_t i = 0; i < old.size() && match; i++) match = nds.ARM9Read8(pk.addr + i) == old[i];
            if (!match) continue; }
        for (size_t i = 0; i < nw.size(); i++) nds.ARM9Write8(pk.addr + i, nw[i]);
        fprintf(stderr, "HGT: poke %08X <- %s (f%u)\n", pk.addr, pk.newHex.c_str(), frame);
    }
}

// HGT_LAN="host:NAAM:2" of "join:NAAM:ADRES": melonDS-LAN-multiplayer starten (losse processen/pc's zien elkaar niet via
// LocalMP, dat werkt alleen binnen één proces). De DS-wireless loopt dan over LAN; de joiner start na de host.
void startLan(EmuInstance* emu)
{
    const char* env = getenv("HGT_LAN"); if (!env) return;
    std::string spec(env); size_t a = spec.find(':'), b = spec.find(':', a + 1);
    if (a == std::string::npos || b == std::string::npos) { fprintf(stderr, "HGT: HGT_LAN moet host:NAAM:N of join:NAAM:ADRES zijn\n"); return; }
    std::string mode = spec.substr(0, a), name = spec.substr(a + 1, b - a - 1), arg = spec.substr(b + 1);
    // niet setMPInterface(): die werkt Qt-menu's bij en dit draait op de emu-thread
    melonDS::MPInterface::Set(melonDS::MPInterface_LAN);
    melonDS::MPInterface::Get().SetRecvTimeout(Config::GetGlobalTable().GetInt("MP.RecvTimeout"));
    melonDS::LAN& lan = (melonDS::LAN&)melonDS::MPInterface::Get();
    bool ok = mode == "host" ? lan.StartHost(name.c_str(), atoi(arg.c_str())) : lan.StartClient(name.c_str(), arg.c_str());
    lan.Begin(emu->getInstanceID());
    fprintf(stderr, "HGT: LAN %s als %s: %s\n", mode.c_str(), name.c_str(), ok ? "gestart" : "MISLUKT");
}

// Logt één keer per battle het echte battleType + trainer-ids: FieldSystem->processManager->child (battle-app)->args = BattleSetup.
uint32_t lastSetup = 0;
void logBattle(NDS& nds)
{
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral); if (!inRam(var)) return;
    uint32_t fs = nds.ARM9Read32(var);               if (!inRam(fs)) return;
    uint32_t pm = nds.ARM9Read32(fs);                if (!inRam(pm)) return;
    uint32_t child = nds.ARM9Read32(pm + 4);
    if (!inRam(child)) { lastSetup = 0; return; }
    uint32_t setup = nds.ARM9Read32(child + 0x18);   if (!inRam(setup) || setup == lastSetup) return;
    lastSetup = setup;
    fprintf(stderr, "HGT: battle setup=%08X type=%08X trainers=%u,%u,%u,%u mode=%u\n", setup, nds.ARM9Read32(setup),
            nds.ARM9Read32(setup + 0x18), nds.ARM9Read32(setup + 0x1C), nds.ARM9Read32(setup + 0x20), nds.ARM9Read32(setup + 0x24), nds.ARM9Read8(setup + 0x1B2));
}

void HGTogether::tick(EmuInstance* emu)
{
    if (!initDone) init();
    if (!enabled || !emu->nds) return;
    frame++;
    if (frame == 1) { startLan(emu); HGTGhost::init(emu->baseROMDir + "/" + emu->baseROMName); }
    if (frame == 2) if (const char* st = getenv("HGT_LOADSTATE")) if (*st) fprintf(stderr, "HGT: savestate %s %s\n", st, emu->loadState(st) ? "geladen" : "MISLUKT");
    NDS& nds = *emu->nds;
    if (frame >= 3) applyPokes(nds);   // na de savestate (frame 2)
    if (frame % 30 == 0) logBattle(nds);
    bool hook = frame > 300 && hookOn(nds);   // pas na de pokes (frame 240+) en alleen als de TB-hook echt in RAM staat
    uint32_t t = hook ? nds.ARM9Read32(kSignal) : 0;
    if (t)   // ROM-hook: trainerbattle gestart, ander moet joinen
    {
        BattleMsg m{ {'H','G','T','B'}, t }; nds.ARM9Write32(kSignal, 0);
        for (int i = 0; i < 3 && peerLen; i++) sendto(sock, (const char*)&m, sizeof(m), 0, (sockaddr*)&peerAddr, peerLen);
        fprintf(stderr, "HGT: trainerbattle %u -> sein naar de ander\n", t);
        clubRole = 1; clubUntil = frame + kClubFrames;
    }
    if (hook && peerBattleSeen && frame - peerBattleSeen < 2 && frame - lastJoin > 600 && !call.active && !call.pending)
    {
        uint32_t var = nds.ARM9Read32(kFieldSysLiteral), fs = inRam(var) ? nds.ARM9Read32(var) : 0;
        if (inRam(fs))
        {
            const char* js = getenv("HGT_JOIN_SCRIPT");
            nds.ARM9Write32(kSignal - 4, peerBattle.trainer);   // HGT.trainer: zelfde vijand op beide kanten (START leest alleen dit)
            call.addr = 0x0203FE74; call.a[0] = fs; call.a[1] = js ? atoi(js) : 9019; call.a[2] = 0; call.a[3] = 0; call.out.clear(); call.pending = true; lastJoin = frame;
            fprintf(stderr, "HGT: ander start trainerbattle %u -> joiner-script %u\n", peerBattle.trainer, call.a[1]);
            clubRole = 2; clubUntil = frame + kClubFrames;
        }
    }
    if (clubRole)
    {
        bool joined = clubConnected(nds) || (clubRole == 1 && nds.ARM9Read32(kSignal + 8) != 0);
        if (joined || frame >= clubUntil) { fprintf(stderr, "HGT: club %s %s\n", clubRole == 1 ? "leader" : "joiner", joined ? "verbonden" : "TIME-OUT"); clubRole = 0; }
        else if (frame % 40 == 0) autoKeyFrames = 4;   // A (JOIN THIS GROUP / YES); op "Awaiting contact" doet A niets
    }
    HGTCamera::apply(nds);   // camera-experiment (HGT_CAM), vóór de ghost zodat die dezelfde camera leest
    bool haveMe = readState(nds, me); me.frame = frame;
    if (haveMe) exchange();
    void* top = nullptr; void* bottom = nullptr;
    bool cpu = nds.GPU.GetFramebuffers(&top, &bottom); if (!top) return;   // false = OpenGL: top = &GLuint van de uitvoertextuur
    if (haveMe) drawGhosts(top, cpu, nds);
    if (!cpu) return;
    u32* fb = (u32*)top;
    if (!dumpDir.empty() && frame % 60 == 0)
    {
        dump(fb);
        const State* p = peers.empty() ? nullptr : &peers[0].st;
        fprintf(stderr, "HGT f%u ik(map %d %d,%d r%d) peers %zu eerste(map %d %d,%d r%d)\n", frame, me.mapId, me.x, me.z, me.facing,
                peers.size(), p ? p->mapId : 0, p ? p->x : 0, p ? p->z : 0, p ? p->facing : 0);
    }
}

// Uitnodiging: stub (role 0) tot de echte afhandeling er is, zodat het dock al kan compileren en mergen.
namespace HGTogether
{
static std::mutex inviteMx; static InviteStatus invite;
InviteStatus inviteStatus() { std::lock_guard<std::mutex> l(inviteMx); return invite; }
void inviteAnswer(bool accept) { std::lock_guard<std::mutex> l(inviteMx); (void)accept; }
}
