// HG-Together: Soul Link dock. Reads the own party from RAM every 250 ms (same decryption as bench/party.py), swaps party +
// catch log with the friend over UDP (HGT_PORT+100 -> HGT_PEER port+100), pairs catches by encounter area and paints teams,
// required actions, pairs and the graveyard in HeartGold party-menu style with the icon sprites from the player's own ROM.
// shortcut: area = mapId and no "missed encounter" detection; switch to the fase 2 encounter watcher's area id when it lands.
// shortcut: own UDP socket next to the ghost bridge; fold into one transport when the launcher (fase 3) brings ENet/relay.
// shortcut: area shown as "Map N"; needs a mapId -> route name table from the ROM headers.
#include "HGTDock.h"
#include "EmuInstance.h"
#include "NDS.h"
#include <QPainter>
#include <QTimer>
#include <QUdpSocket>
#include <QScrollArea>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QHostAddress>
#include <QDateTime>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>

using namespace melonDS;

namespace {
constexpr uint32_t kFieldSysLiteral = 0x0203E304;
bool inRam(uint32_t a) { return a >= 0x02000000 && a < 0x02400000; }
enum Status : uint8_t { TEAM, BOX, DEAD };

#pragma pack(push, 1)
struct Mon { uint32_t pid; uint16_t species; uint8_t level, egg; uint16_t hp, maxhp; char nick[12]; };
struct Entry { uint32_t pid; uint16_t species; int32_t area; uint8_t status, egg; char nick[12]; };
struct Packet { char magic[4]; char name[8]; uint8_t nMons, nEntries; Mon mons[6]; Entry entries[120]; };
#pragma pack(pop)

const uint8_t kOrder[24][4] = {{0,1,2,3},{0,1,3,2},{0,2,1,3},{0,3,1,2},{0,2,3,1},{0,3,2,1},{1,0,2,3},{1,0,3,2},{2,0,1,3},{3,0,1,2},{2,0,3,1},{3,0,2,1},
                               {1,2,0,3},{1,3,0,2},{2,1,0,3},{3,1,0,2},{2,3,0,1},{3,2,0,1},{1,2,3,0},{1,3,2,0},{2,1,3,0},{3,1,2,0},{2,3,1,0},{3,2,1,0}};
// Icon palette per icon file (pret pokemon_icon_idx.c sPokemonPalNoBySpeciesAndForm, indexed by naix-7).
const uint8_t kIconPal[] = {0,1,1,1,0,0,0,0,2,2,1,1,0,1,2,2,0,0,0,2,1,0,0,2,2,2,0,2,2,2,2,2,2,2,2,0,0,2,1,0,0,2,2,1,0,0,0,0,0,2,2,2,1,1,1,2,1,2,0,0,0,0,0,2,2,2,0,2,0,1,1,1,2,2,1,1,1,0,0,0,0,0,0,1,2,2,2,2,2,2,2,2,2,2,2,2,2,1,2,2,0,0,0,1,1,1,2,2,1,2,2,1,1,0,0,1,0,0,0,0,2,2,0,1,2,1,0,2,2,0,0,2,2,2,0,0,0,0,0,0,2,2,0,1,0,0,0,0,0,2,2,0,1,1,1,1,1,1,2,2,2,2,2,2,2,0,0,1,0,2,2,0,0,0,1,2,2,0,0,2,0,0,1,2,2,1,1,1,1,2,2,1,1,1,0,0,2,2,2,0,0,0,0,1,0,2,2,2,0,0,2,0,0,1,2,0,0,2,0,0,2,2,0,0,0,0,2,0,0,0,0,0,0,0,2,1,2,2,1,1,1,1,1,0,2,0,1,0,1,0,1,1,1,0,1,0,0,0,0,0,0,2,2,2,2,0,2,0,2,1,1,1,1,1,1,0,2,2,0,0,1,1,1,2,0,1,1,2,2,1,1,1,1,0,2,2,2,1,2,0,0,2,2,2,2,2,2,0,0,1,0,0,0,0,2,0,1,2,0,0,2,0,1,0,1,0,2,1,1,1,1,1,1,0,0,0,2,1,0,0,0,0,0,1,0,2,0,0,0,2,0,0,1,0,0,0,0,1,0,0,0,2,0,2,2,0,0,0,0,1,0,2,2,0,0,0,0,2,2,0,0,2,2,0,1,0,0,1,1,1,1,0,0,2,2,0,0,0,0,2,2,0,0,0,0,0,1,0,0,0,1,1,1,1,0,0,0,0,0,0,1,0,0,0,2,2,2,2,2,0,1,0,2,0,2,2,0,0,1,0,0,0,0,0,0,0,2,2,2,1,1,0,2,0,0,1,0,0,0,1,1,0,0,1,0,2,1,0,2,1,1,0,2,2,0,1,0,2,2,0,0,0,0,2,2,0,0,0,0,0,0,0,1,0,1,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,1};

void decrypt(uint8_t* d, int len, uint32_t seed)
{
    for (int i = 0; i < len; i += 2)
    {
        seed = seed * 1103515245u + 24691u;
        uint16_t v; memcpy(&v, d + i, 2); v ^= seed >> 16; memcpy(d + i, &v, 2);
    }
}

char dsChar(uint16_t c)   // HGSS charmap: digits 0x121, A 0x12B, a 0x145, space 0x1DE
{
    if (c >= 0x121 && c <= 0x12A) return '0' + (c - 0x121);
    if (c >= 0x12B && c <= 0x144) return 'A' + (c - 0x12B);
    if (c >= 0x145 && c <= 0x15E) return 'a' + (c - 0x145);
    if (c == 0x1DE) return ' ';
    return '?';
}

bool readMon(NDS& nds, uint32_t a, Mon& m)
{
    uint8_t raw[0xEC]; for (int i = 0; i < 0xEC; i++) raw[i] = nds.ARM9Read8(a + i);
    uint32_t pid; uint16_t checksum; memcpy(&pid, raw, 4); memcpy(&checksum, raw + 6, 2);
    uint8_t blocks[0x80]; memcpy(blocks, raw + 8, 0x80); decrypt(blocks, 0x80, checksum);
    uint32_t sum = 0; for (int i = 0; i < 0x80; i += 2) { uint16_t v; memcpy(&v, blocks + i, 2); sum += v; }
    if ((sum & 0xFFFF) != checksum) return false;
    const uint8_t* order = kOrder[((pid >> 13) & 0x1F) % 24];
    auto blk = [&](int which) { return blocks + order[which] * 0x20; };
    memset(&m, 0, sizeof m); m.pid = pid;
    memcpy(&m.species, blk(0), 2);
    uint32_t ivs; memcpy(&ivs, blk(1) + 0x10, 4); m.egg = (ivs >> 30) & 1;
    for (int i = 0; i < 10; i++) { uint16_t c; memcpy(&c, blk(2) + 2 * i, 2); if (c == 0xFFFF) break; m.nick[i] = dsChar(c); }
    uint8_t party[0x64]; memcpy(party, raw + 0x88, 0x64); decrypt(party, 0x64, pid);
    m.level = party[4]; memcpy(&m.hp, party + 6, 2); memcpy(&m.maxhp, party + 8, 2);
    return true;
}

// --- ROM icon sprites (a/0/2/0 = poke_icon.narc: file 0 NCLR, file 7+ NCGR 32x64 4bpp, two frames) ---
struct Icons
{
    bool loaded = false; std::vector<uint8_t> narc; std::vector<std::pair<uint32_t, uint32_t>> fat; uint32_t gmif = 0;
    std::map<int, QImage> cache;

    static uint32_t u32(const QByteArray& b, int o) { uint32_t v; memcpy(&v, b.constData() + o, 4); return v; }
    static uint16_t u16(const QByteArray& b, int o) { uint16_t v; memcpy(&v, b.constData() + o, 2); return v; }

    void load(const QString& romPath)
    {
        loaded = true; QFile f(romPath); if (!f.open(QIODevice::ReadOnly)) return;
        f.seek(0x40); QByteArray hdr = f.read(16); uint32_t fntOff = u32(hdr, 0), fntLen = u32(hdr, 4), fatOff = u32(hdr, 8), fatLen = u32(hdr, 12);
        f.seek(fntOff); QByteArray fnt = f.read(fntLen); f.seek(fatOff); QByteArray fatb = f.read(fatLen);
        int dir = 0xF000; const char* parts[] = {"a", "0", "2", "0"}; int fileId = -1;
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
        fileId = dir;
        uint32_t s = u32(fatb, fileId * 8), e = u32(fatb, fileId * 8 + 4);
        f.seek(s); QByteArray data = f.read(e - s); if (!data.startsWith("NARC")) return;
        uint32_t n = u32(data, 0x18);
        for (uint32_t i = 0; i < n; i++) fat.push_back({u32(data, 0x1C + i * 8), u32(data, 0x20 + i * 8)});
        uint32_t btnf = 0x1C + n * 8; gmif = btnf + u32(data, btnf + 4) + 8;
        narc.assign(data.begin(), data.end());
    }

    const uint8_t* file(int i, uint32_t& len) { if (i < 0 || i >= (int)fat.size()) { len = 0; return nullptr; } len = fat[i].second - fat[i].first; return narc.data() + gmif + fat[i].first; }

    QImage get(int species, bool egg)
    {
        int naix = egg ? 501 : species + 7;
        auto it = cache.find(naix); if (it != cache.end()) return it->second;
        QImage img(32, 32, QImage::Format_ARGB32); img.fill(Qt::transparent);
        uint32_t plen, glen; const uint8_t* pal = file(0, plen); const uint8_t* gfx = file(naix, glen);
        if (pal && gfx && plen >= 0x28 && glen >= 0x30 + 1024)
        {
            int palNo = (naix - 7) < (int)sizeof kIconPal && naix >= 7 ? kIconPal[naix - 7] : 0;
            const uint8_t* colors = pal + 0x28 + palNo * 32;        // NCLR: 0x10 header + 0x18 PLTT header
            const uint8_t* px = gfx + 0x30;                          // NCGR: 0x10 header + 0x20 CHAR header, tiled 4bpp
            for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++)
            {
                int tile = (y / 8) * 4 + x / 8, b = px[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
                int c = (x & 1) ? b >> 4 : b & 15; if (!c) continue;
                uint16_t bgr; memcpy(&bgr, colors + c * 2, 2);
                img.setPixel(x, y, qRgb((bgr & 31) * 255 / 31, ((bgr >> 5) & 31) * 255 / 31, ((bgr >> 10) & 31) * 255 / 31));
            }
        }
        cache[naix] = img; return img;
    }
};

// HeartGold party-menu palette
const QColor cBg(0x3f, 0x9f, 0xb0), cHead(0x1e, 0x5a, 0x66), cSlot(0xf8, 0xf4, 0xe0), cSlotDead(0xd8, 0xd0, 0xc8), cGold(0xe8, 0xc8, 0x60),
             cBrown(0x8c, 0x6a, 0x2c), cText(0x30, 0x30, 0x30), cMuted(0x80, 0x78, 0x70), cRed(0xf0, 0x38, 0x38), cYellow(0xf0, 0xc8, 0x38),
             cGreen(0x40, 0xc8, 0x60), cFrame(0x40, 0x40, 0x40), cWhite(0xff, 0xff, 0xff);
const QColor kPairColors[] = {QColor(0x3c, 0x78, 0xd8), QColor(0xd8, 0x50, 0x50), QColor(0x48, 0xb0, 0x58), QColor(0xe0, 0x98, 0x30),
                              QColor(0x90, 0x58, 0xc8), QColor(0x30, 0xa8, 0xa8), QColor(0xd0, 0x60, 0xa0), QColor(0x88, 0x70, 0x40)};
}

class HGTPanel : public QWidget
{
public:
    HGTPanel(EmuInstance* emu, std::function<QString()> romPath) : emu(emu), romPath(std::move(romPath))
    {
        setMinimumWidth(370);
        int port = atoi(getenv("HGT_PORT") ? getenv("HGT_PORT") : "0");
        if (const char* lan = getenv("HGT_LAN")) { QStringList p = QString(lan).split(':'); if (p.size() > 1) myName = p[1].left(7); }
        if (const char* peer = getenv("HGT_PEER")) { QStringList p = QString(peer).split(':'); if (p.size() == 2) { peerHost = QHostAddress(p[0]); peerPort = p[1].toInt() + 100; } }
        if (port) sock.bind(QHostAddress::AnyIPv4, port + 100);   // +1 would collide with the peer's ghost port when ports are consecutive
        QObject::connect(&sock, &QUdpSocket::readyRead, [this] { receive(); });
        QObject::connect(&timer, &QTimer::timeout, [this] { poll(); });
        timer.start(250);
    }

private:
    EmuInstance* emu; std::function<QString()> romPath; QTimer timer; QUdpSocket sock; QHostAddress peerHost; int peerPort = 0;
    QString myName = "Me", peerName = "Friend", jsonPath; uint32_t saveData = 0; int32_t mapId = -1;
    Mon mons[6]; int nMons = 0; std::vector<Entry> log; Icons icons;
    Mon peerMons[6]; int nPeerMons = 0; std::vector<Entry> peerLog; qint64 peerSeen = 0; int contentHeight = 400, ticks = 0;

    // --- data ---
    bool readParty(NDS& nds)
    {
        uint32_t var = nds.ARM9Read32(kFieldSysLiteral); if (!inRam(var)) return false;
        uint32_t fs = nds.ARM9Read32(var);
        if (inRam(fs))   // overworld: FieldSystem lives; in battle it is NULL and we keep the cached SaveData pointer
        {
            uint32_t loc = nds.ARM9Read32(fs + 0x20); if (inRam(loc)) mapId = (int32_t)nds.ARM9Read32(loc);
            uint32_t sd = nds.ARM9Read32(fs + 0xC); if (inRam(sd)) saveData = sd;
        }
        if (!saveData) return false;
        uint32_t hdr = saveData + 0x23014 + 2 * 0x10;
        if (nds.ARM9Read32(hdr) != 2) { saveData = 0; return false; }
        uint32_t party = saveData + 0x10 + nds.ARM9Read32(hdr + 8), cur = nds.ARM9Read32(party + 4); if (cur > 6) return false;
        Mon tmp[6]; int n = 0;
        for (uint32_t i = 0; i < cur; i++) if (readMon(nds, party + 8 + i * 0xEC, tmp[n])) n++;
        if (n != (int)cur) return false;   // torn read (emu thread mid-write): try again next tick
        memcpy(mons, tmp, sizeof mons); nMons = n; return true;
    }

    Entry* find(std::vector<Entry>& v, uint32_t pid) { for (auto& e : v) if (e.pid == pid) return &e; return nullptr; }

    void updateLog()
    {
        bool changed = false;
        for (int i = 0; i < nMons; i++)
        {
            Mon& m = mons[i]; Entry* e = find(log, m.pid);
            if (!e) { if (mapId < 0) continue; Entry n{m.pid, m.species, mapId, TEAM, m.egg, {}}; memcpy(n.nick, m.nick, 12); log.push_back(n); changed = true; continue; }
            uint8_t st = e->status == DEAD ? DEAD : (m.hp == 0 && !m.egg ? DEAD : TEAM);
            if (st != e->status || e->species != m.species || e->egg != m.egg || memcmp(e->nick, m.nick, 12)) { e->status = st; e->species = m.species; e->egg = m.egg; memcpy(e->nick, m.nick, 12); changed = true; }
        }
        for (auto& e : log)
        {
            bool inParty = false; for (int i = 0; i < nMons; i++) if (mons[i].pid == e.pid) inParty = true;
            if (!inParty && e.status == TEAM) { e.status = BOX; changed = true; }
        }
        if (changed) save();
    }

    void save()
    {
        if (jsonPath.isEmpty()) return;
        QJsonArray a;
        for (auto& e : log) a.append(QJsonObject{{"pid", (qint64)e.pid}, {"species", e.species}, {"area", e.area}, {"status", e.status}, {"egg", e.egg}, {"nick", QString::fromLatin1(e.nick)}});
        QFile f(jsonPath); if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(a).toJson());
    }

    void loadJson()
    {
        QFile f(jsonPath); if (!f.open(QIODevice::ReadOnly)) return;
        for (auto v : QJsonDocument::fromJson(f.readAll()).array())
        {
            QJsonObject o = v.toObject(); Entry e{(uint32_t)o["pid"].toDouble(), (uint16_t)o["species"].toInt(), o["area"].toInt(), (uint8_t)o["status"].toInt(), (uint8_t)o["egg"].toInt(), {}};
            strncpy(e.nick, o["nick"].toString().toLatin1().constData(), 11); log.push_back(e);
        }
    }

    void send()
    {
        if (peerPort == 0) return;
        Packet p{}; memcpy(p.magic, "HGSL", 4); strncpy(p.name, myName.toLatin1().constData(), 7);
        p.nMons = nMons; memcpy(p.mons, mons, sizeof(Mon) * nMons);
        p.nEntries = std::min<size_t>(log.size(), 120); memcpy(p.entries, log.data(), sizeof(Entry) * p.nEntries);
        sock.writeDatagram((const char*)&p, sizeof p, peerHost, peerPort);
    }

    void receive()
    {
        while (sock.hasPendingDatagrams())
        {
            Packet p; qint64 n = sock.readDatagram((char*)&p, sizeof p);
            if (n != sizeof p || memcmp(p.magic, "HGSL", 4) || p.nMons > 6 || p.nEntries > 120) continue;
            p.name[7] = 0; peerName = p.name; nPeerMons = p.nMons; memcpy(peerMons, p.mons, sizeof(Mon) * p.nMons);
            peerLog.assign(p.entries, p.entries + p.nEntries); peerSeen = QDateTime::currentMSecsSinceEpoch();
        }
        update();
    }

    void poll()
    {
        NDS* nds = emu->getNDS(); if (!nds) return;
        if (jsonPath.isEmpty()) { QString rom = romPath(); if (!rom.isEmpty() && QFile::exists(rom)) { jsonPath = rom + ".soullink.json"; loadJson(); icons.load(rom); } }
        if (readParty(*nds)) updateLog();
        send(); update();
        if (const char* d = getenv("HGT_DOCK_DUMP")) if (++ticks % 8 == 0) grab().save(d);   // headless test: PNG of the dock every 2 s
    }

    // --- pairs ---
    struct Pair { int32_t area; const Entry* mine; const Entry* theirs; int color; };
    std::vector<Pair> pairs() const
    {
        std::vector<Pair> out; auto has = [&](int32_t a) { for (auto& p : out) if (p.area == a) return true; return false; };
        for (auto& e : log) if (!has(e.area)) out.push_back({e.area, &e, nullptr, 0});
        for (auto& e : peerLog) { bool hit = false; for (auto& p : out) if (p.area == e.area && !p.theirs) { p.theirs = &e; hit = true; break; } if (!hit && !has(e.area)) out.push_back({e.area, nullptr, &e, 0}); }
        for (size_t i = 0; i < out.size(); i++) out[i].color = i % 8;
        return out;
    }
    bool pairDead(const Pair& p) const { return (p.mine && p.mine->status == DEAD) || (p.theirs && p.theirs->status == DEAD); }
    const Pair* pairOf(const std::vector<Pair>& ps, uint32_t pid) const { for (auto& p : ps) if ((p.mine && p.mine->pid == pid) || (p.theirs && p.theirs->pid == pid)) return &p; return nullptr; }

    // --- painting ---
    static const char* statusText(uint8_t s) { return s == TEAM ? "TEAM" : s == BOX ? "BOX" : "FNT"; }
    QColor statusColor(uint8_t s) { return s == TEAM ? cGreen.darker(130) : s == BOX ? QColor(0x30, 0x78, 0xc0) : cRed; }

    void slotBox(QPainter& g, QRect r, bool dead)
    {
        g.setPen(QPen(cBrown, 2)); g.setBrush(dead ? cSlotDead : cSlot); g.drawRoundedRect(r.adjusted(1, 1, -1, -1), 4, 4);
        g.setPen(QPen(cGold, 1)); g.setBrush(Qt::NoBrush); g.drawRoundedRect(r.adjusted(3, 3, -3, -3), 3, 3);
    }

    void hpBar(QPainter& g, QRect r, int hp, int maxhp)
    {
        g.setPen(cFrame); g.setBrush(cFrame); g.drawRect(r);
        if (maxhp <= 0) return;
        int w = (r.width() - 2) * hp / maxhp; double f = (double)hp / maxhp;
        g.setPen(Qt::NoPen); g.setBrush(f > 0.5 ? cGreen : f > 0.2 ? cYellow : cRed); g.drawRect(r.x() + 1, r.y() + 1, w, r.height() - 2);
    }

    void monSlot(QPainter& g, QRect r, const Mon& m, const Pair* pr)
    {
        const Entry* e = pr ? (pr->mine && pr->mine->pid == m.pid ? pr->mine : pr->theirs) : nullptr;
        bool dead = (m.hp == 0 && !m.egg) || (e && e->status == DEAD); slotBox(g, r, dead);
        if (pr) { g.setPen(Qt::NoPen); g.setBrush(kPairColors[pr->color]); g.drawRect(r.x() + 4, r.y() + 5, 4, r.height() - 10); }
        g.drawImage(r.x() + 10, r.y() + (r.height() - 32) / 2, icons.get(m.species, m.egg));
        g.setPen(dead ? cMuted : cText); QFont f = g.font(); f.setBold(true); g.setFont(f);
        g.drawText(r.x() + 46, r.y() + 17, QString::fromLatin1(m.nick));
        f.setBold(false); g.setFont(f);
        g.drawText(r.right() - 44, r.y() + 17, m.egg ? "EGG" : QString("Lv.%1").arg(m.level));
        if (dead) { g.setPen(cRed); g.drawText(r.x() + 46, r.y() + 33, "FAINTED"); }
        else if (!m.egg) { hpBar(g, QRect(r.x() + 46, r.y() + 24, r.width() - 100, 8), m.hp, m.maxhp); g.setPen(cText); g.drawText(r.right() - 48, r.y() + 33, QString("%1/%2").arg(m.hp).arg(m.maxhp)); }
    }

    void emptySlot(QPainter& g, QRect r) { g.setPen(QPen(cBrown.lighter(140), 1, Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawRoundedRect(r.adjusted(1, 1, -1, -1), 4, 4); }

    void header(QPainter& g, int& y, const QString& text)
    {
        g.setPen(Qt::NoPen); g.setBrush(cHead); g.drawRect(0, y, width(), 22);
        g.setPen(cHead.darker(160)); QFont f = g.font(); f.setBold(true); g.setFont(f); g.drawText(9, y + 16, text); g.setPen(cWhite); g.drawText(8, y + 15, text);
        f.setBold(false); g.setFont(f); y += 26;
    }

    void entryText(QPainter& g, int x, int y, int w, const Entry* e, bool right)
    {
        if (!e) { g.setPen(cMuted); g.drawText(QRect(x, y, w, 36), (right ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, "—"); return; }
        QImage ic = icons.get(e->species, e->egg); int ix = right ? x + w - 32 : x; g.drawImage(ix, y + 2, ic);
        int tx = right ? x : x + 36; int tw = w - 36;
        QFont f = g.font(); f.setBold(true); g.setFont(f); g.setPen(e->status == DEAD ? cMuted : cText);
        g.drawText(QRect(tx, y + 2, tw, 16), (right ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, QString::fromLatin1(e->nick));
        f.setBold(false); g.setFont(f); g.setPen(statusColor(e->status));
        g.drawText(QRect(tx, y + 18, tw, 16), (right ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, statusText(e->status));
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this); g.fillRect(rect(), cBg); QFont f = g.font(); f.setPointSize(10); g.setFont(f);
        int W = width(), y = 0; auto ps = pairs();
        bool online = peerSeen && QDateTime::currentMSecsSinceEpoch() - peerSeen < 3000;
        header(g, y, QString("SOUL LINK   %1 ↔ %2  %3").arg(myName, peerName, online ? "● online" : "○ offline"));

        // teams
        int colW = (W - 24) / 2;
        for (int side = 0; side < 2; side++)
        {
            int x = 8 + side * (colW + 8); const Mon* ms = side ? peerMons : mons; int n = side ? nPeerMons : nMons;
            g.setPen(cWhite); f.setBold(true); g.setFont(f); g.drawText(x + 2, y + 14, side ? peerName : myName); f.setBold(false); g.setFont(f);
            for (int i = 0; i < 6; i++)
            {
                QRect r(x, y + 20 + i * 42, colW, 40);
                if (i < n) monSlot(g, r, ms[i], pairOf(ps, ms[i].pid)); else emptySlot(g, r);
            }
        }
        y += 20 + 6 * 42 + 8;

        // actions
        header(g, y, "WHAT NEEDS TO HAPPEN");
        QStringList acts;
        for (auto& p : ps)
        {
            if (p.mine && p.theirs)
            {
                if (p.mine->status == DEAD && p.theirs->status != DEAD) acts << QString("▶ %1's %2 must go: partner %3 fainted (Map %4).").arg(peerName, p.theirs->nick, p.mine->nick).arg(p.area);
                if (p.theirs->status == DEAD && p.mine->status != DEAD) acts << QString("▶ Your %1 must go: partner %2 fainted (Map %3).").arg(p.mine->nick, p.theirs->nick).arg(p.area);
                if (!pairDead(p) && p.mine->status != p.theirs->status) acts << QString("▶ Box rule: %1 is in the %2, %3 in the %4.").arg(p.mine->nick, p.mine->status == BOX ? "box" : "team", p.theirs->nick, p.theirs->status == BOX ? "box" : "team");
            }
        }
        if (!online) acts << "▶ Friend not connected.";
        if (acts.isEmpty()) acts << "All pairs in order. Trainer battles allowed.";
        int tw = W - 36, ah = 0; QList<int> hs;
        for (auto& a : acts) { int h = g.fontMetrics().boundingRect(QRect(0, 0, tw, 1000), Qt::TextWordWrap, a).height() + 4; hs << h; ah += h; }
        QRect ar(8, y, W - 16, 10 + ah); slotBox(g, ar, false); int ay = ar.y() + 6;
        for (int i = 0; i < acts.size(); i++) { g.setPen(acts[i].startsWith(QChar(0x25b6)) ? cRed : cGreen.darker(140)); g.drawText(QRect(ar.x() + 10, ay, tw, hs[i]), Qt::TextWordWrap, acts[i]); ay += hs[i]; }
        y += ar.height() + 8;

        // pairs per area
        header(g, y, "PAIRS PER AREA");
        for (auto& p : ps)
        {
            if (pairDead(p)) continue;
            QRect r(8, y, W - 16, 56); slotBox(g, r, false);
            g.setPen(Qt::NoPen); g.setBrush(kPairColors[p.color]); g.drawRect(r.x() + 4, r.y() + 5, 4, r.height() - 10);
            g.setPen(cMuted); g.drawText(r.x() + 12, r.y() + 14, QString("Map %1").arg(p.area));
            int half = (r.width() - 60) / 2;
            entryText(g, r.x() + 12, r.y() + 18, half, p.mine, false);
            g.setPen(kPairColors[p.color]); f.setBold(true); f.setPointSize(13); g.setFont(f); g.drawText(QRect(r.x() + 12 + half, r.y() + 18, 36, 36), Qt::AlignCenter, p.mine && p.theirs ? "∞" : "·"); f.setBold(false); f.setPointSize(10); g.setFont(f);
            entryText(g, r.x() + 48 + half, r.y() + 18, half, p.theirs, true);
            y += 60;
        }
        if (ps.empty()) { g.setPen(cWhite); g.drawText(16, y + 14, "No catches yet."); y += 24; }

        // graveyard
        header(g, y, "GRAVEYARD");
        int graves = 0;
        for (auto& p : ps)
        {
            if (!pairDead(p)) continue; graves++;
            QRect r(8, y, W - 16, 56); slotBox(g, r, true);
            g.setPen(cMuted); g.drawText(r.x() + 12, r.y() + 14, QString("Map %1").arg(p.area));
            int half = (r.width() - 60) / 2;
            entryText(g, r.x() + 12, r.y() + 18, half, p.mine, false);
            g.setPen(cRed); f.setBold(true); f.setPointSize(13); g.setFont(f); g.drawText(QRect(r.x() + 12 + half, r.y() + 18, 36, 36), Qt::AlignCenter, "✕"); f.setBold(false); f.setPointSize(10); g.setFont(f);
            entryText(g, r.x() + 48 + half, r.y() + 18, half, p.theirs, true);
            y += 60;
        }
        if (!graves) { g.setPen(cWhite); g.drawText(16, y + 14, "Empty. Keep it that way."); y += 24; }
        y += 8;
        if (y != contentHeight) { contentHeight = y; setMinimumHeight(y); }
    }
};

HGTDock::HGTDock(QWidget* parent, EmuInstance* emu, std::function<QString()> romPath) : QDockWidget("Soul Link", parent)
{
    setObjectName("HGTDock");
    panel = new HGTPanel(emu, std::move(romPath));
    auto* scroll = new QScrollArea(this); scroll->setWidget(panel); scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); scroll->setMinimumWidth(390);
    setWidget(scroll);
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
}
