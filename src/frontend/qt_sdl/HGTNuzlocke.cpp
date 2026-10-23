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
// Dupes per evolutiefamilie (kFamily, uit de ROM gegenereerd met bench/families.py).
#include "HGTNuzlocke.h"
#include "EmuInstance.h"
#include "NDS.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <map>
#include <atomic>
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
const uint32_t kFieldSysLiteral = 0x0203E304, kTaskWild = 0x02050C18, kTaskEncounter = 0x02050738, kTaskSafari = 0x02050D1C, kTaskBugContest = 0x02050EB8, kTaskLink = 0x020508B8, kHgtActive = 0x02078B18, kMapHeaders = 0x020F6BE0;
const int kMapsecLeague = 15, kMapsecIndigo = 147;   // pret constants/map_sections.h: binnen de League vervalt de E4-cap
enum { PENDING, CAUGHT, MISSED, REROLL };
struct Enc { int32_t area; uint16_t species; uint32_t pid; uint8_t outcome; int64_t ts; };
struct Mon { uint32_t pid; uint16_t species; uint8_t level, egg, nicked, busy; uint16_t hp, maxhp; };   // busy: het spel heeft dit record nu ontsleuteld in gebruik
// party-deel onzin (level 0 of > 100, HP > max): niets afdwingen, alleen melden (playtest 2026-10-21: party-deel onversleuteld weggeschreven)
bool corrupt(const Mon& m) { return !m.egg && (m.level == 0 || m.level > 100 || m.maxhp == 0 || m.hp > m.maxhp); }
struct Link { uint32_t pid, partner; int32_t area; uint8_t cause, enforce; };   // Soul Link: mijn pid moet weg; cause 0 dood, 1 gemist, 2 partner in de box
struct Player { std::vector<Enc> encs; std::vector<uint32_t> dead, banned; uint16_t party[6] = {}; uint32_t partyPid[6] = {}; uint8_t nParty = 0; uint32_t starter = 0; uint16_t starterSpecies = 0; std::vector<Link> linked; std::vector<Mon> corrupt; };
struct Packet { char magic[4]; uint8_t idx, nEncs, nDead, nBanned, nParty, starter, soulLink; uint32_t starterPid; uint16_t starterSpecies; uint16_t party[6]; uint32_t partyPid[6]; Enc encs[200]; uint32_t dead[64], banned[32]; };

bool enabled = false, initDone = false; int sock = -1; sockaddr_storage peerAddr{}; socklen_t peerLen = 0;
std::atomic<int> peerStarter{-1}, mineStarter{0};   // atomics: het dock (GUI-thread) leest ze
// calm: ARM9 wacht op vblank (spel niet midden in zijn werk)
int myIdx = 0, starterFirst = 0, soulLink = 0, peerSoulLink = -1; bool peerParty = false, calm = false, partyDue = false; uint32_t calmTicks = 0, allTicks = 0; std::string jsonPath; uint32_t frame = 0, saveData = 0, fieldSys = 0, curSetup = 0, doneSetup = 0, curPid = 0;
std::vector<uint32_t> pendingDead; uint32_t linkSetup = 0;   // doden in een linkbattle (co-op): pas definitief als die normaal eindigt
bool curEligible = false; uint32_t bagHidden = 0, bagCopy = 0, bagSnapSD = 0; uint8_t bagSnap[0x798]; bool bagHeld = false; int32_t curArea = -1; uint8_t johto = 0, kanto = 0; bool gameClear = false; bool dirty = false;
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
// Het spel ontsleutelt records soms in RAM zelf en houdt ze zo over meerdere frames (vlaggen op +4: bit 0 partyDecrypted, bit 1
// boxDecrypted; bv. de meelopende Pokémon in slot 0). Dan zijn die delen al plat: niet nog eens ontsleutelen, anders lees je onzin
// (bench 2026-10-21: Totodile "Lv105, HP 51119"). In de playtest leidde zo'n leesfout tot een box-write tijdens die vergrendeling,
// waarna het spel de verschoven Eevee nogmaals "versleutelde" = plat wegschreef. Daarom ook: busy-records nooit verplaatsen.
bool readMon(NDS& nds, uint32_t a, Mon& m, bool party = true)   // false = checksum klopt niet (half geschreven)
{
    uint8_t raw[0xEC]; for (int i = 0; i < (party ? 0xEC : 0x88); i++) raw[i] = nds.ARM9Read8(a + i);
    uint32_t pid; uint16_t flags, checksum; memcpy(&pid, raw, 4); memcpy(&flags, raw + 4, 2); memcpy(&checksum, raw + 6, 2);
    uint8_t blocks[0x80]; memcpy(blocks, raw + 8, 0x80); if (!(flags & 2)) decrypt(blocks, 0x80, checksum);
    uint32_t sum = 0; for (int i = 0; i < 0x80; i += 2) { uint16_t v; memcpy(&v, blocks + i, 2); sum += v; }
    if ((sum & 0xFFFF) != checksum && !(flags & 2)) return false;   // ontsleuteld in gebruik: checksum kan net bijgewerkt worden
    const uint8_t* order = kOrder[((pid >> 13) & 0x1F) % 24];
    memset(&m, 0, sizeof m); m.pid = pid; m.busy = flags & 3; memcpy(&m.species, blocks + order[0] * 0x20, 2);
    uint32_t ivs; memcpy(&ivs, blocks + order[1] * 0x20 + 0x10, 4); m.egg = (ivs >> 30) & 1; m.nicked = ivs >> 31;   // blok B +0x10: bit 30 isEgg, bit 31 hasNickname
    if (!party) return true;
    uint8_t pt[0x64]; memcpy(pt, raw + 0x88, 0x64); if (!(flags & 1)) decrypt(pt, 0x64, pid);
    m.level = pt[4]; memcpy(&m.hp, pt + 6, 2); memcpy(&m.maxhp, pt + 8, 2);
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
// Evolutiefamilie per soort (basisvorm), gegenereerd met bench/families.py uit a/0/3/4 van Sacred Gold v1.05 (508 soorten).
const uint16_t kFamily[508] = {
    0,1,1,1,4,4,4,7,7,7,10,10,10,13,13,13,16,16,16,19,
    19,21,21,23,23,172,172,27,27,29,29,29,32,32,32,173,173,37,37,174,
    174,41,41,43,43,43,46,46,48,48,50,50,52,52,54,54,56,56,58,58,
    60,60,60,63,63,63,66,66,66,69,69,69,72,72,74,74,74,77,77,79,
    79,81,81,83,84,84,86,86,88,88,90,90,92,92,92,95,96,96,98,98,
    100,100,102,102,104,104,236,236,108,109,109,111,111,440,114,115,116,116,118,118,
    120,120,439,123,238,239,240,127,128,129,129,131,132,133,133,133,133,137,138,138,
    140,140,142,446,144,145,146,147,147,147,150,151,152,152,152,155,155,155,158,158,
    158,161,161,163,163,165,165,167,167,41,170,170,172,173,174,175,175,177,177,179,
    179,179,43,298,298,438,60,187,187,187,190,191,191,193,194,194,133,133,198,79,
    200,201,360,203,204,204,206,207,95,209,209,211,123,213,214,215,216,216,218,218,
    220,220,222,223,223,225,458,227,228,228,116,231,231,137,234,235,236,236,238,239,
    240,241,440,243,244,245,246,246,246,249,250,251,252,252,252,255,255,255,258,258,
    258,261,261,263,263,265,265,265,265,265,270,270,270,273,273,273,276,276,278,278,
    280,280,280,283,283,285,285,287,287,287,290,290,290,293,293,293,296,296,298,299,
    300,300,302,303,304,304,304,307,307,309,309,311,312,313,314,406,316,316,318,318,
    320,320,322,322,324,325,325,327,328,328,328,331,331,333,333,335,336,337,338,339,
    339,341,341,343,343,345,345,347,347,349,349,351,352,353,353,355,355,357,433,359,
    360,361,361,363,363,363,366,366,366,369,370,371,371,371,374,374,374,377,378,379,
    380,381,382,383,384,385,386,387,387,387,390,390,390,393,393,393,396,396,396,399,
    399,401,401,403,403,403,406,406,408,408,410,410,412,412,412,415,415,417,418,418,
    420,420,422,422,190,425,425,427,427,200,198,431,431,433,434,434,436,436,438,439,
    440,441,442,443,443,443,446,447,447,449,449,451,451,453,453,455,456,456,458,459,
    459,215,81,108,111,114,239,240,175,193,133,133,207,220,137,280,299,355,361,479,
    480,481,482,483,484,485,486,487,488,489,490,491,492,493,494,495,496,497,498,499,
    500,501,502,503,504,505,506,507,
};
uint16_t family(uint16_t s) { return s < sizeof kFamily / sizeof *kFamily ? kFamily[s] : s; }
bool owned(uint16_t species)   // dupe = een Pokémon uit dezelfde evolutiefamilie in een party of als vangst van een van beiden
{
    species = family(species);
    for (const Player* p : {&me, &peer})
    {
        for (int i = 0; i < p->nParty; i++) if (family(p->party[i]) == species) return true;
        for (auto& e : p->encs) if (e.outcome == CAUGHT && family(e.species) == species) return true;
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

// --- Soul Link (docs/soullink.md): de eerste vangst per area van beide spelers is een paar, de starters ook (area -1).
// Is de partner van de vriend dood, of heeft de vriend zijn encounter in die area gemist, dan moet mijn vangst ook weg. ---
bool inParty(const Player& p, uint32_t pid) { for (int i = 0; i < p.nParty && i < 6; i++) if (p.partyPid[i] == pid) return true; return false; }
// Paren (mijn pid, partner-pid, area) waarvan beide nog leven: vangst-paren per area + het starterpaar (area -1).
std::vector<Link> pairs(const Player& a, const Player& b)
{
    std::vector<Link> out;
    for (auto& e : a.encs) if (e.outcome == CAUGHT && !has(a.dead, e.pid))
        for (auto& f : b.encs) if (f.area == e.area && f.outcome == CAUGHT && !has(b.dead, f.pid)) out.push_back({e.pid, f.pid, e.area, 2, 0});
    if (a.starter && b.starter && !has(a.dead, a.starter) && !has(b.dead, b.starter)) out.push_back({a.starter, b.starter, -1, 2, 0});
    return out;
}
// Box-regel: alleen afdwingen op de overgang team -> box van de partner (gezien in deze sessie), anders kan een paar nooit meer terug in
// het team: wie als eerste terughaalt zou meteen weer geboxt worden. Een verschil zonder die overgang staat alleen in de json (enforce false).
std::map<uint32_t, bool> partnerSeenInTeam;
std::vector<Link> links(const Player& a, const Player& b, bool mine)   // wat a kwijt moet door b
{
    std::vector<Link> out; if (!soulLink || soulLink != peerSoulLink) return out;
    for (auto& e : a.encs)
    {
        if (e.outcome != CAUGHT || has(a.dead, e.pid)) continue;
        for (auto& f : b.encs) if (f.area == e.area)
        {
            if (f.outcome == CAUGHT && has(b.dead, f.pid)) out.push_back({e.pid, f.pid, e.area, 0, 1});
            else if (f.outcome == MISSED) out.push_back({e.pid, 0, e.area, 1, 1});
        }
    }
    if (a.starter && b.starter && !has(a.dead, a.starter) && has(b.dead, b.starter)) out.push_back({a.starter, b.starter, -1, 0, 1});
    if (peerParty)
        for (auto& l : pairs(a, b))
            if (inParty(a, l.pid) && !inParty(b, l.partner)) { l.enforce = mine && partnerSeenInTeam[l.partner]; out.push_back(l); }
    return out;
}
void updateLinks()
{
    if (peerParty) for (auto& l : pairs(me, peer))
    {
        if (inParty(peer, l.partner)) partnerSeenInTeam[l.partner] = true;
        else if (!inParty(me, l.pid)) partnerSeenInTeam[l.partner] = false;   // allebei in de box: regel voldaan, terughalen mag
    }
    std::vector<Link> mine = links(me, peer, true);
    for (auto& l : mine)
    {
        bool known = false; for (auto& k : me.linked) known |= k.pid == l.pid && k.enforce == l.enforce;
        if (known) continue;
        if (l.cause == 2 && l.enforce) nzlog("SOUL LINK: area %d, partner %08X van de vriend is naar de box -> mijn pid %08X moet ook naar de box", l.area, l.partner, l.pid);
        else if (l.cause == 2) nzlog("Soul Link: area %d, partner %08X van de vriend staat in de box en mijn pid %08X in het team (niet afgedwongen: geen overgang gezien)", l.area, l.partner, l.pid);
        else nzlog("SOUL LINK: area %d, partner %08X van de vriend is %s -> mijn pid %08X moet weg", l.area, l.partner, l.cause ? "gemist" : "dood", l.pid);
    }
    std::vector<Link> theirs = links(peer, me, false);   // ook de afleiding voor de vriend in de json (het dock toont die)
    auto same = [](const std::vector<Link>& a, const std::vector<Link>& b) { if (a.size() != b.size()) return false; for (size_t i = 0; i < a.size(); i++) if (a[i].pid != b[i].pid || a[i].cause != b[i].cause || a[i].enforce != b[i].enforce) return false; return true; };
    if (!same(mine, me.linked) || !same(theirs, peer.linked)) dirty = true;
    me.linked = mine; peer.linked = theirs;
}
bool linked(uint32_t pid) { for (auto& l : me.linked) if (l.pid == pid && l.enforce) return true; return false; }

// --- opslag + sync ---
QJsonObject toJson(const Player& p)
{
    QJsonArray encs, dead, banned;
    for (auto& e : p.encs) encs.append(QJsonObject{{"area", e.area}, {"species", e.species}, {"pid", (qint64)e.pid}, {"outcome", e.outcome}, {"ts", (qint64)e.ts}});
    for (auto d : p.dead) dead.append((qint64)d);
    for (auto b : p.banned) banned.append((qint64)b);
    QJsonArray bad;   // party-Pokémon met onzin-waarden (alleen gemeld, niets afgedwongen)
    for (auto& m : p.corrupt) bad.append(QJsonObject{{"pid", (qint64)m.pid}, {"species", m.species}, {"level", m.level}, {"hp", m.hp}, {"maxhp", m.maxhp}});
    QJsonArray linked;   // afgeleid (Soul Link), voor het dock: area -1 = het starter-paar
    for (auto& l : p.linked) linked.append(QJsonObject{{"pid", (qint64)l.pid}, {"partner", (qint64)l.partner}, {"area", l.area}, {"cause", l.cause == 2 ? "boxed" : l.cause ? "missed" : "dead"}, {"enforce", l.enforce != 0}});
    return {{"encs", encs}, {"dead", dead}, {"banned", banned}, {"starter", (qint64)p.starter}, {"starterSpecies", p.starterSpecies}, {"linked", linked}, {"corrupt", bad}};
}
Player fromJson(const QJsonObject& o)
{
    Player p;
    for (auto v : o["encs"].toArray()) { QJsonObject e = v.toObject(); p.encs.push_back({e["area"].toInt(), (uint16_t)e["species"].toInt(), (uint32_t)e["pid"].toDouble(), (uint8_t)e["outcome"].toInt(), (int64_t)e["ts"].toDouble()}); }
    for (auto v : o["dead"].toArray()) p.dead.push_back((uint32_t)v.toDouble());
    for (auto v : o["banned"].toArray()) p.banned.push_back((uint32_t)v.toDouble());
    p.starter = (uint32_t)o["starter"].toDouble(); p.starterSpecies = (uint16_t)o["starterSpecies"].toInt();
    return p;
}
void save()
{
    dirty = false; if (jsonPath.empty()) return;
    QJsonObject o{{"me", toJson(me)}, {"peer", toJson(peer)}, {"starter", starterFirst}, {"soullink", soulLink != 0}};
    if (me.starterSpecies && peer.starterSpecies) o["sameStarter"] = family(me.starterSpecies) == family(peer.starterSpecies);   // dock: "jullie hebben dezelfde starter"
    if (peerStarter >= 0) { o["peerStarter"] = peerStarter.load(); o["starterMismatch"] = peerStarter != starterFirst; }
    QFile f(QString::fromStdString(jsonPath)); if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(o).toJson());
}
void load()
{
    QFile f(QString::fromStdString(jsonPath)); if (!f.open(QIODevice::ReadOnly)) return;
    QJsonObject o = QJsonDocument::fromJson(f.readAll()).object(); me = fromJson(o["me"].toObject()); peer = fromJson(o["peer"].toObject());
    if (o.contains("starter")) starterFirst = o["starter"].toInt() ? 1 : 0;
    if (o.contains("soullink")) soulLink = o["soullink"].toBool() ? 1 : 0;
}
void exchange()
{
    if (sock < 0) return;
    if (peerLen && frame % 15 == 0)
    {
        Packet p{}; memcpy(p.magic, "HGNZ", 4); p.idx = myIdx; p.starter = starterFirst; p.soulLink = soulLink; p.starterPid = me.starter; p.starterSpecies = me.starterSpecies;
        p.nEncs = std::min<size_t>(me.encs.size(), 200); memcpy(p.encs, me.encs.data(), sizeof(Enc) * p.nEncs);
        p.nDead = std::min<size_t>(me.dead.size(), 64); memcpy(p.dead, me.dead.data(), 4 * p.nDead);
        p.nBanned = std::min<size_t>(me.banned.size(), 32); memcpy(p.banned, me.banned.data(), 4 * p.nBanned);
        p.nParty = me.nParty; memcpy(p.party, me.party, sizeof p.party); memcpy(p.partyPid, me.partyPid, sizeof p.partyPid);
        sendto(sock, (const char*)&p, sizeof p, 0, (sockaddr*)&peerAddr, peerLen);
    }
    Packet p; sockaddr_storage from{}; socklen_t fromLen = sizeof from; int n;
    while ((n = recvfrom(sock, (char*)&p, sizeof p, 0, (sockaddr*)&from, &fromLen)) > 0)
    {
        if (n != (int)sizeof p || memcmp(p.magic, "HGNZ", 4) || p.nEncs > 200 || p.nDead > 64 || p.nBanned > 32 || p.nParty > 6) { fromLen = sizeof from; continue; }
        static bool sameIdxWarned = false;
        if (p.idx == myIdx) { if (!sameIdxWarned) nzlog("FOUT: de vriend heeft ook HGT_INSTANCE=%d; sync genegeerd tot dat verschilt", myIdx); sameIdxWarned = true; fromLen = sizeof from; continue; }
        if (p.starter != peerStarter)
        {
            peerStarter = p.starter; dirty = true;
            if (p.starter != starterFirst) nzlog("BEURT-MISMATCH: ik heb HGT_NUZLOCKE_STARTER=%d, de vriend %d; beurtvolgorde en reroll-recht kloppen niet tot dit gelijk is", starterFirst, p.starter);
            else nzlog("beurtvolgorde gelijk met de vriend (starter %d)", starterFirst);
        }
        if (p.soulLink != peerSoulLink)
        {
            peerSoulLink = p.soulLink;
            if (p.soulLink != soulLink) nzlog("SOULLINK-MISMATCH: ik heb HGT_SOULLINK=%d, de vriend %d; Soul Link geldt pas als het gelijk is", soulLink, p.soulLink);
        }
        Player q; q.starter = p.starterPid; q.starterSpecies = p.starterSpecies; q.encs.assign(p.encs, p.encs + p.nEncs); q.dead.assign(p.dead, p.dead + p.nDead); q.banned.assign(p.banned, p.banned + p.nBanned);
        q.nParty = p.nParty; memcpy(q.party, p.party, sizeof q.party); memcpy(q.partyPid, p.partyPid, sizeof q.partyPid); peerParty = true;
        if (q.encs.size() != peer.encs.size() || memcmp(q.encs.data(), peer.encs.data(), sizeof(Enc) * q.encs.size()) || q.dead != peer.dead || q.banned != peer.banned || q.starter != peer.starter || q.starterSpecies != peer.starterSpecies) dirty = true;
        q.linked = peer.linked; peer = q; if (!peerLen) { memcpy(&peerAddr, &from, fromLen); peerLen = fromLen; }
        fromLen = sizeof from;
    }
}

void init(const std::string& rom)
{
    initDone = true;
    const char* on = getenv("HGT_NUZLOCKE"); if (!on || !*on || *on == '0') return;
    enabled = true;
    if (const char* e = getenv("HGT_INSTANCE")) myIdx = atoi(e) ? 1 : 0;
    jsonPath = rom + ".nuzlocke.json"; load();   // starter uit de run; de omgeving wint, met een melding als die afwijkt
    if (const char* e = getenv("HGT_NUZLOCKE_STARTER"))
    {
        int v = atoi(e) ? 1 : 0;
        if (QFile::exists(QString::fromStdString(jsonPath)) && v != starterFirst) nzlog("let op: HGT_NUZLOCKE_STARTER=%d wijkt af van de opgeslagen run (%d); %d geldt", v, starterFirst, v);
        starterFirst = v;
    }
    if (const char* e = getenv("HGT_SOULLINK")) soulLink = atoi(e) ? 1 : 0;
    mineStarter = starterFirst; save();   // bestand bestaat altijd vanaf de start
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
    nzlog("actief als speler %d, poort %d, staat %s, Soul Link %s", myIdx, atoi(port) + 200, jsonPath.c_str(), soulLink ? "aan" : "uit");
}

// --- bag: pockets items/medicine/berries/battle items (pret bag_types_def.h, 4 bytes per slot) ---
const struct { uint32_t off, n; } kBlocked[] = {{0, 165}, {0x520, 40}, {0x5C0, 64}, {0x720, 30}};
bool bagOk(NDS& nds) { return saveData && nds.ARM9Read32(saveData + 0x23014 + 3 * 0x10) == 3; }   // blok-header 3 = BAG
void bagHide(NDS& nds, uint32_t setup)
{
    // alleen zolang de battle loopt (winFlag 0) en alleen in de tas-kopie die we bij het begin van deze battle zagen: geen
    // schrijfacties in geheugen dat na de battle al vrijgegeven en hergebruikt kan zijn
    uint32_t copy = nds.ARM9Read32(setup + 0x108); if (!inRam(copy) || nds.ARM9Read32(setup + 0x14) != 0) return;
    if (setup != bagHidden) bagCopy = copy; else if (copy != bagCopy) return;
    if (!bagHeld)
    {
        if (!bagOk(nds)) { nzlog("tas: SaveData klopt niet (blok 3), tas niet aangeraakt"); return; }
        uint32_t bag = saveArray(nds, 3); for (auto& p : kBlocked) for (uint32_t i = 0; i < p.n * 4; i++) bagSnap[p.off + i] = nds.ARM9Read8(bag + p.off + i);
        bagHeld = true; bagSnapSD = saveData; nzlog("tas: items verborgen voor de battle");
    }
    for (auto& p : kBlocked) for (uint32_t i = 0; i < p.n * 4; i++) if (nds.ARM9Read8(copy + p.off + i)) nds.ARM9Write8(copy + p.off + i, 0);
    bagHidden = setup;
}
void bagRestore(NDS& nds)   // aanroep alleen in de overworld: saveData is dan net uit het FieldSystem gelezen
{
    bagHeld = false; bagHidden = bagCopy = 0;
    if (!bagOk(nds) || saveData != bagSnapSD) { nzlog("tas: SaveData anders dan bij het verbergen (%08X, was %08X) of blok 3 klopt niet: items NIET teruggezet", saveData, bagSnapSD); return; }
    uint32_t bag = saveArray(nds, 3); for (auto& p : kBlocked) for (uint32_t i = 0; i < p.n * 4; i++) nds.ARM9Write8(bag + p.off + i, bagSnap[p.off + i]);
    nzlog("tas: items terug");
}

// --- encounter-watcher: taakketen FS+0x10 -> prev; Task_Wild/Safari/BugContestEncounter en Task_StartEncounter hebben env+0x10 = BattleSetup
//     (Safari en Bug Contest zijn wilde encounters met een eigen taak; zonder deze twee waren vangsten daar een vrije "gift").
//     Task_020508B8 = linkbattle (co-op via de cave van doel A, Colosseum): Encounter, setup ook op env+0x10 ---
uint32_t encounterSetup(NDS& nds, uint32_t fs)
{
    uint32_t t = nds.ARM9Read32(fs + 0x10);
    for (int i = 0; inRam(t) && i < 8; i++, t = nds.ARM9Read32(t))
    {
        uint32_t func = nds.ARM9Read32(t + 4) & ~1u;
        if (func == kTaskWild || func == kTaskEncounter || func == kTaskSafari || func == kTaskBugContest || func == kTaskLink) { uint32_t s = nds.ARM9Read32(nds.ARM9Read32(t + 0xC) + 0x10); return inRam(s) ? s : 0; }
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
// Doden tijdens de battle: de battle werkt op de party in de BattleSetup (+4 = party[PLAYER]) en schrijft die pas na een
// gewonnen battle terug. Bij verlies (whiteout) heelt het spel de echte party, dus zonder deze lezing zou een dood daar onzichtbaar zijn.
void watchBattleParty(NDS& nds, uint32_t setup)
{
    uint32_t party = nds.ARM9Read32(setup + 4), cur = inRam(party) ? nds.ARM9Read32(party + 4) : 0; if (cur > 6) return;
    for (uint32_t i = 0; i < cur; i++)
    {
        Mon m; if (!readMon(nds, party + 8 + i * 0xEC, m)) continue;
        if (!m.species || m.hp || m.egg || m.busy || corrupt(m) || has(me.dead, m.pid) || has(pendingDead, m.pid)) continue;
        // linkbattle: de fork van doel A zet bij wegvallen van de ander de savestate van vóór de co-op terug; dan leeft hij weer
        if (nds.ARM9Read32(setup) & 4) { pendingDead.push_back(m.pid); nzlog("DOOD (linkbattle, voorlopig tot het einde): soort %u Lv%u pid %08X", m.species, m.level, m.pid); }
        else { me.dead.push_back(m.pid); dirty = true; nzlog("DOOD (in de battle): soort %u Lv%u pid %08X", m.species, m.level, m.pid); }
    }
}
void watchEncounter(NDS& nds, uint32_t fs)
{
    uint32_t setup = fs ? encounterSetup(nds, fs) : 0;
    if (setup && calm) watchBattleParty(nds, setup);   // ook trainerbattles; alleen als het spel stilstaat
    if (setup && (setup != bagHidden || frame % 30 == 0)) bagHide(nds, setup);
    if (linkSetup && setup != linkSetup)   // linkbattle weg zonder einde: savestate-restore (co-op afgebroken) of Colosseum
    {
        if (!pendingDead.empty()) nzlog("linkbattle zonder einde (restore of Colosseum): %zu voorlopige dood vervalt", pendingDead.size());
        pendingDead.clear(); linkSetup = 0;
    }
    if (setup && (nds.ARM9Read32(setup) & 4))
    {
        linkSetup = setup;
        // normaal einde: winFlag gezet terwijl de co-op loopt (HGT.active 1; vriendschappelijke Colosseum-battles tellen niet)
        if (nds.ARM9Read32(setup + 0x14) && nds.ARM9Read32(kHgtActive) == 1 && !pendingDead.empty())
        {
            for (uint32_t pid : pendingDead) { me.dead.push_back(pid); nzlog("DOOD (co-op, battle klaar winFlag %u): pid %08X", nds.ARM9Read32(setup + 0x14), pid); }
            pendingDead.clear(); dirty = true;
        }
    }
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
    nzlog("encounter area %d soort %u Lv%u (speler %d kiest hier eerst, ik ben %d): %s", curArea, wild.species, wild.level, firstPicker(curArea), myIdx,
          dupe ? (second ? "dupe, reroll toegestaan (vluchten)" : "dupe, niet vangen") : "eerste encounter, vangen mag");
}

// --- party: doden markeren, afdwingen in de overworld (dood / verboden / boven de cap -> eerste lege pc-box) ---
bool box(NDS& nds, uint32_t party, uint32_t cur, int slot)
{
    // alleen records verplaatsen die het spel nu niet ontsleuteld heeft (+4 vlaggen partyDecrypted/boxDecrypted/checksumFailed = 0)
    for (uint32_t k = slot; k < cur; k++) if (nds.ARM9Read16(party + 8 + k * 0xEC + 4)) { nzlog("box: slot %u is nu ontsleuteld in gebruik, later opnieuw", k); return false; }
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
        // laatste slot leeg zoals ZeroMonData: box-deel = lege BoxPokemon (nullen versleuteld met checksum 0, gekopieerd van de lege pc-plek),
        // party-deel = 0x64 nullen versleuteld met pid 0 (Porygon 2: anders bleef het oude party-deel daar staan)
        uint8_t zeroParty[0x64] = {}; decrypt(zeroParty, 0x64, 0);
        for (int i = 0; i < 0x88; i++) nds.ARM9Write8(party + 8 + (cur - 1) * 0xEC + i, empty[i]);
        for (int i = 0; i < 0x64; i++) nds.ARM9Write8(party + 8 + (cur - 1) * 0xEC + 0x88 + i, zeroParty[i]);
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
    me.nParty = cur; for (uint32_t i = 0; i < 6; i++) { me.party[i] = i < cur ? mons[i].species : 0; me.partyPid[i] = i < cur ? mons[i].pid : 0; }
    if (!me.starter && me.encs.empty() && cur == 1 && !mons[0].egg) { me.starter = mons[0].pid; me.starterSpecies = mons[0].species; dirty = true; nzlog("starter: soort %u pid %08X (Soul Link-paar met de starter van de vriend)", mons[0].species, mons[0].pid); }
    for (uint32_t i = 0; i < cur; i++) if (mons[i].busy) return;   // spel heeft een record vast: deze tick niets beslissen of schrijven
    std::vector<Mon> bad; for (uint32_t i = 0; i < cur; i++) if (corrupt(mons[i])) bad.push_back(mons[i]);
    for (auto& m : bad)
    {
        bool known = false; for (auto& k : me.corrupt) known |= k.pid == m.pid;
        if (!known) nzlog("FOUT: party-gegevens van soort %u pid %08X zijn onzin (Lv %u, HP %u/%u); niets afgedwongen. Herstel: in de pc zetten en weer ophalen", m.species, m.pid, m.level, m.hp, m.maxhp);
    }
    if (bad.size() != me.corrupt.size()) dirty = true;
    me.corrupt = bad;
    static bool sameWarned = false;
    if (!sameWarned && me.starterSpecies && peer.starterSpecies && family(me.starterSpecies) == family(peer.starterSpecies))
    { sameWarned = true; nzlog("ZELFDE STARTER: jij en de vriend hebben allebei soort %u (familie %u); starters vallen buiten de dupes-regel, er wordt niets geboxt", me.starterSpecies, family(me.starterSpecies)); }
    static std::vector<uint32_t> nickWarned;   // verplichte nickname: één melding per Pokémon per sessie (naam geven: Name Rater, Goldenrod)
    for (uint32_t i = 0; i < cur; i++)
        if (!mons[i].egg && !mons[i].nicked && !has(nickWarned, mons[i].pid)) { nickWarned.push_back(mons[i].pid); nzlog("NICKNAME VERPLICHT: soort %u pid %08X heeft geen bijnaam", mons[i].species, mons[i].pid); }
    for (uint32_t i = 0; i < cur; i++)
        if (mons[i].hp == 0 && !mons[i].egg && !corrupt(mons[i]) && !has(me.dead, mons[i].pid)) { me.dead.push_back(mons[i].pid); dirty = true; nzlog("DOOD: soort %u Lv%u pid %08X", mons[i].species, mons[i].level, mons[i].pid); }
    if (!idle) return;
    uint32_t loc = nds.ARM9Read32(fs + 0x20); int32_t mapId = inRam(loc) ? (int32_t)nds.ARM9Read32(loc) : -1;
    int mapsec = mapId >= 0 && mapId < 540 ? nds.ARM9Read8(kMapHeaders + mapId * 0x18 + 18) : -1;
    int cap = levelCap(); bool league = cap == 55 && (mapsec == kMapsecLeague || mapsec == kMapsecIndigo);
    for (uint32_t i = 0; i < cur; i++)
    {
        if (corrupt(mons[i])) continue;   // gemeld hierboven; een bug mag geen run beëindigen
        const char* why = has(me.dead, mons[i].pid) ? "dood" : has(me.banned, mons[i].pid) ? "verboden vangst" : linked(mons[i].pid) ? "Soul Link-partner weg" : (!league && !mons[i].egg && mons[i].level > cap) ? "boven de level cap" : nullptr;
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
    calm = nds.ARM9.Halted != 0; allTicks++; calmTicks += calm;
    if (frame % 1800 == 0) nzlog("leeft (party %u, encs %zu, dood %zu, rustig %u%%)", me.nParty, me.encs.size(), me.dead.size(), allTicks ? calmTicks * 100 / allTicks : 0);
    uint32_t var = nds.ARM9Read32(kFieldSysLiteral), fs = inRam(var) ? nds.ARM9Read32(var) : 0; if (!inRam(fs)) fs = 0;
    if (fs) { uint32_t sd = nds.ARM9Read32(fs + 0xC); if (inRam(sd)) saveData = sd; }   // in battle is FS NULL: SaveData-pointer bewaren
    if (saveData && nds.ARM9Read32(saveData + 0x23014 + 2 * 0x10) != 2) saveData = 0;
    if (saveData)
    {
        uint32_t pd = saveArray(nds, 1); nds.ARM9Write16(pd, nds.ARM9Read16(pd) | 0x40);   // Options.battleStyle = Set
        johto = nds.ARM9Read8(pd + 4 + 0x1A); kanto = nds.ARM9Read8(pd + 4 + 0x1F); gameClear = nds.ARM9Read8(pd + 4 + 0x1D) & 1;
        if (fs) fieldSys = fs;
        watchEncounter(nds, fieldSys);   // de taakketen leeft in het FieldSystem, ook als de pointer tijdens de battle NULL is
        bool idle = fs && nds.ARM9Read32(fs + 0x10) == 0 && nds.ARM9Read32(fs + 0x6C) == 1 && !curSetup && inRam(nds.ARM9Read32(fs)) && nds.ARM9Read32(nds.ARM9Read32(fs) + 8) == 0;   // + [[FS]+8] isPaused == 0 (geen menu/app open)
        if (idle && bagHeld) bagRestore(nds);   // na de terugschrijf van de battle-tas
        // Party alleen lezen/schrijven als de ARM9 stilstaat (wacht op vblank). melonDS stopt een frame op een vast aantal cycli, ook als het spel
        // dan midden in het (ont)sleutelen van een record zit: dat gaf op de bench steeds dezelfde halve record ("Totodile Lv105, HP 51119").
        if (frame % 8 == 0) partyDue = true;
        if (partyDue && calm) { partyDue = false; updateLinks(); watchParty(nds, fs, idle); }
    }
    exchange();
    if (dirty && frame % 15 == 0) save();
}

bool HGTNuzlocke::starterMismatch(int& mine, int& friendValue)
{
    mine = mineStarter; friendValue = peerStarter; return enabled && friendValue >= 0 && friendValue != mine;
}
