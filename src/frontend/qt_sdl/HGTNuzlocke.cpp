// HG-Together fase 2: Hardcore-Nuzlocke-watcher (docs/fase2-onderzoek.md, regels in docs/plan.md fase 2). Aan met HGT_NUZLOCKE=1.
// Leest elk frame de taakketen van het FieldSystem (wilde encounter -> BattleSetup: mapsec, soort, winFlag) en de party.
// Per speler: de tellende encounter per mapsec (vangst / gemist / reroll), doden (hp 0, blijvend) en verboden vangsten.
// Afdwingen, alleen in de overworld: dode, verboden en boven-de-cap Pokémon gaan naar de eerste lege pc-box; Set-mode altijd aan.
// Staat in <rom>.nuzlocke.json en elke 15 frames via UDP (HGT_PORT+200 -> HGT_PEER-poort+200) naar de ander; ieder is
// eigenaar van zijn eigen helft, dus samenvoegen = de helft van de ander overnemen. Beurt: areas op volgorde van ontdekking,
// starter = area 0 van speler HGT_NUZLOCKE_STARTER (standaard 0), daarna om en om; wie als tweede kiest mag bij een dupe
// rerollen (die encounter telt niet). HGT_INSTANCE (0/1) = wie ik ben.
// Bag-items in battle: de battle werkt op een kopie van de tas (BattleSetup+0x108) die na afloop wordt teruggeschreven
// (pret battle_setup.c). De niet-Ball-pockets (items, medicine, berries, battle items) worden in die kopie leeggemaakt en na
// de battle vanuit een snapshot in de echte tas teruggezet; Balls blijven zoals de battle ze achterliet. Geen ROM-patch.
// shortcut: dupes op soort, niet op evolutiefamilie.
#include "HGTNuzlocke.h"
#include "EmuInstance.h"
#include "NDS.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
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
const uint32_t kFieldSysLiteral = 0x0203E304, kTaskWild = 0x02050C18, kTaskEncounter = 0x02050738, kMapHeaders = 0x020F6BE0;
const int kMapsecLeague = 15, kMapsecIndigo = 147;   // pret constants/map_sections.h: binnen de League vervalt de E4-cap
enum { PENDING, CAUGHT, MISSED, REROLL };
struct Enc { int32_t area; uint16_t species; uint32_t pid; uint8_t outcome; int64_t ts; };
struct Player { std::vector<Enc> encs; std::vector<uint32_t> dead, banned; uint16_t party[6] = {}; uint8_t nParty = 0; };
struct Packet { char magic[4]; uint8_t idx, nEncs, nDead, nBanned, nParty; uint16_t party[6]; Enc encs[200]; uint32_t dead[64], banned[32]; };
struct Mon { uint32_t pid; uint16_t species; uint8_t level, egg; uint16_t hp; };

bool enabled = false, initDone = false; int sock = -1; sockaddr_storage peerAddr{}; socklen_t peerLen = 0;
int myIdx = 0, starterFirst = 0; std::string jsonPath; uint32_t frame = 0, saveData = 0, fieldSys = 0, curSetup = 0, doneSetup = 0, curPid = 0;
bool curEligible = false; uint32_t bagHidden = 0; uint8_t bagSnap[0x798]; bool bagHeld = false; int32_t curArea = -1; uint8_t johto = 0, kanto = 0; bool gameClear = false; bool dirty = false;
Player me, peer;

bool inRam(uint32_t a) { return a >= 0x02000000 && a < 0x02400000; }
int64_t now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
void nzlog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); fprintf(stderr, "HGT-NZ f%u: ", frame); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); }

// --- Pokémon lezen (zelfde ontsleuteling als bench/party.py) ---
const uint8_t kOrder[24][4] = {{0,1,2,3},{0,1,3,2},{0,2,1,3},{0,3,1,2},{0,2,3,1},{0,3,2,1},{1,0,2,3},{1,0,3,2},{2,0,1,3},{3,0,1,2},{2,0,3,1},{3,0,2,1},
    {1,2,0,3},{1,3,0,2},{2,1,0,3},{3,1,0,2},{2,3,0,1},{3,2,0,1},{1,2,3,0},{1,3,2,0},{2,1,3,0},{3,1,2,0},{2,3,1,0},{3,2,1,0}};
void decrypt(uint8_t* d, int len, uint32_t seed)
{
    for (int i = 0; i < len; i += 2) { seed = seed * 1103515245u + 24691u; uint16_t v; memcpy(&v, d + i, 2); v ^= seed >> 16; memcpy(d + i, &v, 2); }
}
bool readMon(NDS& nds, uint32_t a, Mon& m, bool party = true)   // false = checksum klopt niet (half geschreven)
{
    uint8_t raw[0xEC]; for (int i = 0; i < (party ? 0xEC : 0x88); i++) raw[i] = nds.ARM9Read8(a + i);
    uint32_t pid; uint16_t checksum; memcpy(&pid, raw, 4); memcpy(&checksum, raw + 6, 2);
    uint8_t blocks[0x80]; memcpy(blocks, raw + 8, 0x80); decrypt(blocks, 0x80, checksum);
    uint32_t sum = 0; for (int i = 0; i < 0x80; i += 2) { uint16_t v; memcpy(&v, blocks + i, 2); sum += v; }
    if ((sum & 0xFFFF) != checksum) return false;
    const uint8_t* order = kOrder[((pid >> 13) & 0x1F) % 24];
    memset(&m, 0, sizeof m); m.pid = pid; memcpy(&m.species, blocks + order[0] * 0x20, 2);
    uint32_t ivs; memcpy(&ivs, blocks + order[1] * 0x20 + 0x10, 4); m.egg = (ivs >> 30) & 1;
    if (!party) return true;
    uint8_t pt[0x64]; memcpy(pt, raw + 0x88, 0x64); decrypt(pt, 0x64, pid); m.level = pt[4]; memcpy(&m.hp, pt + 6, 2);
    return true;
}
uint32_t saveArray(NDS& nds, int idx) { return saveData + 0x10 + nds.ARM9Read32(saveData + 0x23014 + idx * 0x10 + 8); }

// --- level cap (docs/levelcaps.md): laagste cap van de nog niet verslagen gyms; Johto klaar -> E4 55 tot gameClear; Kanto; Red 94 ---
int levelCap()
{
    static const int kJohto[8] = {15, 21, 24, 29, 35, 40, 44, 50}, kKanto[8] = {61, 62, 63, 65, 64, 66, 68, 70};
    auto lowest = [](const int* t, uint8_t bits) { int c = 0; for (int i = 0; i < 8; i++) if (!(bits >> i & 1) && (!c || t[i] < c)) c = t[i]; return c; };
    if (int c = lowest(kJohto, johto)) return c;
    if (!gameClear) return 55;
    if (int c = lowest(kKanto, kanto)) return c;
    return 94;
}

// --- regels ---
Enc* findEnc(Player& p, int32_t area) { for (auto& e : p.encs) if (e.area == area) return &e; return nullptr; }
bool has(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }
bool owned(uint16_t species)
{
    for (const Player* p : {&me, &peer})
    {
        for (int i = 0; i < p->nParty; i++) if (p->party[i] == species) return true;
        for (auto& e : p->encs) if (e.outcome == CAUGHT && e.species == species) return true;
    }
    return false;
}
int firstPicker(int32_t area)   // n-de ontdekte area (starter = nummer 0 van starterFirst) -> om en om
{
    std::vector<std::pair<int64_t, int32_t>> areas;
    for (const Player* p : {&me, &peer}) for (auto& e : p->encs)
    {
        auto it = std::find_if(areas.begin(), areas.end(), [&](auto& a) { return a.second == e.area; });
        if (it == areas.end()) areas.push_back({e.ts, e.area}); else it->first = std::min(it->first, e.ts);
    }
    std::sort(areas.begin(), areas.end());
    int n = (int)areas.size();
    for (int i = 0; i < (int)areas.size(); i++) if (areas[i].second == area) n = i;
    return (n + 1 + starterFirst) % 2;
}

// --- opslag + sync ---
QJsonObject toJson(const Player& p)
{
    QJsonArray encs, dead, banned;
    for (auto& e : p.encs) encs.append(QJsonObject{{"area", e.area}, {"species", e.species}, {"pid", (qint64)e.pid}, {"outcome", e.outcome}, {"ts", (qint64)e.ts}});
    for (auto d : p.dead) dead.append((qint64)d);
    for (auto b : p.banned) banned.append((qint64)b);
    return {{"encs", encs}, {"dead", dead}, {"banned", banned}};
}
Player fromJson(const QJsonObject& o)
{
    Player p;
    for (auto v : o["encs"].toArray()) { QJsonObject e = v.toObject(); p.encs.push_back({e["area"].toInt(), (uint16_t)e["species"].toInt(), (uint32_t)e["pid"].toDouble(), (uint8_t)e["outcome"].toInt(), (int64_t)e["ts"].toDouble()}); }
    for (auto v : o["dead"].toArray()) p.dead.push_back((uint32_t)v.toDouble());
    for (auto v : o["banned"].toArray()) p.banned.push_back((uint32_t)v.toDouble());
    return p;
}
void save()
{
    dirty = false; if (jsonPath.empty()) return;
    QFile f(QString::fromStdString(jsonPath)); if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"me", toJson(me)}, {"peer", toJson(peer)}}).toJson());
}
void load()
{
    QFile f(QString::fromStdString(jsonPath)); if (!f.open(QIODevice::ReadOnly)) return;
    QJsonObject o = QJsonDocument::fromJson(f.readAll()).object(); me = fromJson(o["me"].toObject()); peer = fromJson(o["peer"].toObject());
}
void exchange()
{
    if (sock < 0) return;
    if (peerLen && frame % 15 == 0)
    {
        Packet p{}; memcpy(p.magic, "HGNZ", 4); p.idx = myIdx;
        p.nEncs = std::min<size_t>(me.encs.size(), 200); memcpy(p.encs, me.encs.data(), sizeof(Enc) * p.nEncs);
        p.nDead = std::min<size_t>(me.dead.size(), 64); memcpy(p.dead, me.dead.data(), 4 * p.nDead);
        p.nBanned = std::min<size_t>(me.banned.size(), 32); memcpy(p.banned, me.banned.data(), 4 * p.nBanned);
        p.nParty = me.nParty; memcpy(p.party, me.party, sizeof p.party);
        sendto(sock, (const char*)&p, sizeof p, 0, (sockaddr*)&peerAddr, peerLen);
    }
    Packet p; sockaddr_storage from{}; socklen_t fromLen = sizeof from; int n;
    while ((n = recvfrom(sock, (char*)&p, sizeof p, 0, (sockaddr*)&from, &fromLen)) > 0)
    {
        if (n != (int)sizeof p || memcmp(p.magic, "HGNZ", 4) || p.idx == myIdx || p.nEncs > 200 || p.nDead > 64 || p.nBanned > 32 || p.nParty > 6) { fromLen = sizeof from; continue; }
        Player q; q.encs.assign(p.encs, p.encs + p.nEncs); q.dead.assign(p.dead, p.dead + p.nDead); q.banned.assign(p.banned, p.banned + p.nBanned);
        q.nParty = p.nParty; memcpy(q.party, p.party, sizeof q.party);
        if (q.encs.size() != peer.encs.size() || memcmp(q.encs.data(), peer.encs.data(), sizeof(Enc) * q.encs.size()) || q.dead != peer.dead || q.banned != peer.banned) dirty = true;
        peer = q; if (!peerLen) { memcpy(&peerAddr, &from, fromLen); peerLen = fromLen; }
        fromLen = sizeof from;
    }
}

void init(const std::string& rom)
{
    initDone = true;
    const char* on = getenv("HGT_NUZLOCKE"); if (!on || !*on || *on == '0') return;
    enabled = true;
    if (const char* e = getenv("HGT_INSTANCE")) myIdx = atoi(e) ? 1 : 0;
    if (const char* e = getenv("HGT_NUZLOCKE_STARTER")) starterFirst = atoi(e) ? 1 : 0;
    jsonPath = rom + ".nuzlocke.json"; load();
    const char* port = getenv("HGT_PORT"); const char* peerStr = getenv("HGT_PEER");
    if (!port) { nzlog("actief zonder HGT_PORT: geen sync"); return; }
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me4{}; me4.sin_family = AF_INET; me4.sin_addr.s_addr = htonl(INADDR_ANY); me4.sin_port = htons((uint16_t)(atoi(port) + 200));
    if (bind(sock, (sockaddr*)&me4, sizeof me4) != 0) { nzlog("bind op poort %d mislukt", atoi(port) + 200); sock = -1; return; }
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
#else
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
#endif
    if (peerStr)
    {
        std::string s = peerStr; auto colon = s.rfind(':'); if (colon == std::string::npos) return;
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; addrinfo* res = nullptr;
        if (getaddrinfo(s.substr(0, colon).c_str(), std::to_string(atoi(s.substr(colon + 1).c_str()) + 200).c_str(), &hints, &res) != 0 || !res) return;
        memcpy(&peerAddr, res->ai_addr, res->ai_addrlen); peerLen = (socklen_t)res->ai_addrlen; freeaddrinfo(res);
    }
    nzlog("actief als speler %d, poort %d, staat %s", myIdx, atoi(port) + 200, jsonPath.c_str());
}

// --- bag: pockets items/medicine/berries/battle items (pret bag_types_def.h, 4 bytes per slot) ---
const struct { uint32_t off, n; } kBlocked[] = {{0, 165}, {0x520, 40}, {0x5C0, 64}, {0x720, 30}};
void bagHide(NDS& nds, uint32_t setup)
{
    uint32_t copy = nds.ARM9Read32(setup + 0x108); if (!inRam(copy)) return;
    if (!bagHeld) { uint32_t bag = saveArray(nds, 3); for (auto& p : kBlocked) for (uint32_t i = 0; i < p.n * 4; i++) bagSnap[p.off + i] = nds.ARM9Read8(bag + p.off + i); bagHeld = true; nzlog("tas: items verborgen voor de battle"); }
    for (auto& p : kBlocked) for (uint32_t i = 0; i < p.n * 4; i++) if (nds.ARM9Read8(copy + p.off + i)) nds.ARM9Write8(copy + p.off + i, 0);
    bagHidden = setup;
}
void bagRestore(NDS& nds)
{
    uint32_t bag = saveArray(nds, 3); for (auto& p : kBlocked) for (uint32_t i = 0; i < p.n * 4; i++) nds.ARM9Write8(bag + p.off + i, bagSnap[p.off + i]);
    bagHeld = false; bagHidden = 0; nzlog("tas: items terug");
}

// --- encounter-watcher: taakketen FS+0x10 -> prev; Task_WildEncounter/Task_StartEncounter hebben env+0x10 = BattleSetup ---
uint32_t encounterSetup(NDS& nds, uint32_t fs)
{
    uint32_t t = nds.ARM9Read32(fs + 0x10);
    for (int i = 0; inRam(t) && i < 8; i++, t = nds.ARM9Read32(t))
    {
        uint32_t func = nds.ARM9Read32(t + 4) & ~1u;
        if (func == kTaskWild || func == kTaskEncounter) { uint32_t s = nds.ARM9Read32(nds.ARM9Read32(t + 0xC) + 0x10); return inRam(s) ? s : 0; }
    }
    return 0;
}
void finish(uint32_t winFlag)   // winFlag: 1 win, 2 lose, 4 gevangen, 5 gevlucht (constants/battle.h)
{
    Enc* e = findEnc(me, curArea); bool caught = winFlag == 4;
    if (caught && !curEligible) me.banned.push_back(curPid);   // dupe / tweede vangst in de area: moet weg
    if (e && e->outcome == PENDING) e->outcome = caught ? CAUGHT : MISSED;
    nzlog("encounter klaar area %d winFlag %u -> %s%s", curArea, winFlag, caught ? "gevangen" : "geen vangst", caught && !curEligible ? " (VERBODEN, gaat naar de box)" : "");
    curSetup = 0; dirty = true;
}
void watchEncounter(NDS& nds, uint32_t fs)
{
    uint32_t setup = fs ? encounterSetup(nds, fs) : 0;
    if (setup && (setup != bagHidden || frame % 30 == 0)) bagHide(nds, setup);
    if (curSetup && setup != curSetup) { finish(0); return; }   // setup weg zonder winFlag (blackout-pad): gemist
    if (!setup) { doneSetup = 0; return; }   // heap hergebruikt hetzelfde adres voor de volgende battle
    if (setup == doneSetup) return;
    if (curSetup)
    {
        uint32_t wf = nds.ARM9Read32(setup + 0x14); if (wf) { doneSetup = setup; finish(wf); }
        return;
    }
    if (nds.ARM9Read32(setup) & 1) { doneSetup = setup; return; }   // trainerbattle: geen Nuzlocke-encounter
    uint32_t enemy = nds.ARM9Read32(setup + 8); Mon wild; if (!inRam(enemy) || !readMon(nds, enemy + 8, wild)) return;
    curSetup = setup; curPid = wild.pid; curArea = (int32_t)nds.ARM9Read32(setup + 0x154);
    Enc* e = findEnc(me, curArea);
    if (e && e->outcome != REROLL && e->outcome != PENDING) { curEligible = false; nzlog("encounter area %d soort %u: area al gebruikt, telt niet", curArea, wild.species); return; }
    bool dupe = owned(wild.species), second = firstPicker(curArea) != myIdx;
    if (!e) { me.encs.push_back({curArea, wild.species, wild.pid, PENDING, now()}); e = &me.encs.back(); }
    e->species = wild.species; e->pid = wild.pid; e->outcome = (dupe && second) ? REROLL : PENDING;
    curEligible = !dupe; dirty = true;
    nzlog("encounter area %d soort %u Lv%u: %s", curArea, wild.species, wild.level, dupe ? (second ? "dupe, reroll toegestaan (vluchten)" : "dupe, niet vangen") : "eerste encounter, vangen mag");
}

// --- party: doden markeren, afdwingen in de overworld (dood / verboden / boven de cap -> eerste lege pc-box) ---
bool box(NDS& nds, uint32_t party, uint32_t cur, int slot)
{
    uint32_t storage = saveArray(nds, 41), curBox = nds.ARM9Read32(storage + 0x12000); if (curBox >= 18) curBox = 0;
    for (uint32_t b = 0; b < 18; b++) for (int s = 0; s < 30; s++)
    {
        uint32_t dst = storage + ((b + curBox) % 18) * 0x1000 + s * 0x88; Mon m;
        if (!readMon(nds, dst, m, false) || m.species) continue;
        uint8_t empty[0x88]; for (int i = 0; i < 0x88; i++) empty[i] = nds.ARM9Read8(dst + i);
        uint32_t src = party + 8 + slot * 0xEC;
        for (int i = 0; i < 0x88; i++) nds.ARM9Write8(dst + i, nds.ARM9Read8(src + i));
        // Party_RemoveMon: slots opschuiven (incl. PartyExtra 5 bytes/slot op +0x590), laatste slot = lege BoxPokemon, curCount--
        for (uint32_t k = slot; k + 1 < cur; k++)
        {
            for (int i = 0; i < 0xEC; i++) nds.ARM9Write8(party + 8 + k * 0xEC + i, nds.ARM9Read8(party + 8 + (k + 1) * 0xEC + i));
            for (int i = 0; i < 5; i++) nds.ARM9Write8(party + 0x590 + k * 5 + i, nds.ARM9Read8(party + 0x590 + (k + 1) * 5 + i));
        }
        for (int i = 0; i < 0x88; i++) nds.ARM9Write8(party + 8 + (cur - 1) * 0xEC + i, empty[i]);
        nds.ARM9Write32(party + 4, cur - 1);
        nzlog("-> box %u slot %d", (b + curBox) % 18 + 1, s + 1);
        return true;
    }
    nzlog("geen lege box-plek");
    return false;
}
void watchParty(NDS& nds, uint32_t fs, bool idle)
{
    uint32_t party = saveArray(nds, 2), cur = nds.ARM9Read32(party + 4); if (cur > 6) return;
    Mon mons[6];
    for (uint32_t i = 0; i < cur; i++) if (!readMon(nds, party + 8 + i * 0xEC, mons[i])) return;   // half geschreven: volgende keer
    me.nParty = cur; for (uint32_t i = 0; i < cur; i++) me.party[i] = mons[i].species;
    for (uint32_t i = 0; i < cur; i++)
        if (mons[i].hp == 0 && !mons[i].egg && !has(me.dead, mons[i].pid)) { me.dead.push_back(mons[i].pid); dirty = true; nzlog("DOOD: soort %u Lv%u pid %08X", mons[i].species, mons[i].level, mons[i].pid); }
    if (!idle) return;
    uint32_t loc = nds.ARM9Read32(fs + 0x20); int32_t mapId = inRam(loc) ? (int32_t)nds.ARM9Read32(loc) : -1;
    int mapsec = mapId >= 0 && mapId < 540 ? nds.ARM9Read8(kMapHeaders + mapId * 0x18 + 18) : -1;
    int cap = levelCap(); bool league = cap == 55 && (mapsec == kMapsecLeague || mapsec == kMapsecIndigo);
    for (uint32_t i = 0; i < cur; i++)
    {
        const char* why = has(me.dead, mons[i].pid) ? "dood" : has(me.banned, mons[i].pid) ? "verboden vangst" : (!league && !mons[i].egg && mons[i].level > cap) ? "boven de level cap" : nullptr;
        if (!why) continue;
        if (cur == 1) { if (frame % 600 == 0) nzlog("laatste Pokémon is %s: run voorbij", why); return; }
        nzlog("soort %u Lv%u pid %08X %s (cap %d)", mons[i].species, mons[i].level, mons[i].pid, why, cap);
        box(nds, party, cur, i); return;   // één per keer; volgende tick leest de nieuwe party
    }
}
}

void HGTNuzlocke::tick(EmuInstance* emu)
{
    if (!initDone) init(emu->baseROMDir + "/" + emu->baseROMName);
    if (!enabled || !emu->getNDS()) return;
    frame++; NDS& nds = *emu->getNDS();
    if (frame % 1800 == 0) nzlog("leeft (party %u, encs %zu, dood %zu)", me.nParty, me.encs.size(), me.dead.size());
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral), fs = inRam(var) ? nds.ARM9Read32(var) : 0; if (!inRam(fs)) fs = 0;
    if (fs) { uint32_t sd = nds.ARM9Read32(fs + 0xC); if (inRam(sd)) saveData = sd; }   // in battle is FS NULL: SaveData-pointer bewaren
    if (saveData && nds.ARM9Read32(saveData + 0x23014 + 2 * 0x10) != 2) saveData = 0;
    if (saveData)
    {
        uint32_t pd = saveArray(nds, 1); nds.ARM9Write16(pd, nds.ARM9Read16(pd) | 0x40);   // Options.battleStyle = Set
        johto = nds.ARM9Read8(pd + 4 + 0x1A); kanto = nds.ARM9Read8(pd + 4 + 0x1F); gameClear = nds.ARM9Read8(pd + 4 + 0x1D) & 1;
        if (fs) fieldSys = fs;
        watchEncounter(nds, fieldSys);   // de taakketen leeft in het FieldSystem, ook als de pointer tijdens de battle NULL is
        bool idle = fs && nds.ARM9Read32(fs + 0x10) == 0 && nds.ARM9Read32(fs + 0x6C) == 1 && !curSetup;
        if (idle && bagHeld) bagRestore(nds);   // na de terugschrijf van de battle-tas
        if (frame % 8 == 0) watchParty(nds, fs, idle);
    }
    exchange();
    if (dirty && frame % 15 == 0) save();
}
