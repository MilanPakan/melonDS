// HG-Together: Soul Link dock. Reads the own party from RAM every 250 ms (same decryption as bench/party.py), swaps party +
// catch log with the friend over UDP (HGT_PORT+100 -> HGT_PEER port+100), pairs catches by encounter area and paints teams,
// required actions, pairs and the graveyard in HeartGold party-menu style with the icon sprites from the player's own ROM.
// shortcut: area = mapId and no "missed encounter" detection; switch to the fase 2 encounter watcher's area id when it lands.
// shortcut: own UDP socket next to the ghost bridge; fold into one transport when the launcher (fase 3) brings ENet/relay.
#include "HGTDock.h"
#include "EmuInstance.h"
#include "NDS.h"
#include "Config.h"
#include <QPainter>
#include <QMouseEvent>
#include <QTimer>
#include <QUdpSocket>
#include <QScrollArea>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QHostAddress>
#include <QDateTime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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
// Map names from pret include/constants/maps.h (MAP_xxx -> words), indexed by mapId.
const char* const kMapNames[] = {
"Everywhere",
"Nothing",
"Union",
"Underground",
"Wifi Single Battle Area",
"Wifi Multi Battle Area",
"Bellchime Tower",
"Burned Tower 1F",
"Ruins Of Alph Unused",
"Route 1",
"Route 2",
"Route 3",
"Route 4",
"Route 5",
"Route 6",
"Route 7",
"Route 8",
"Route 9",
"Route 10",
"Route 11",
"Route 12",
"Route 13",
"Route 14",
"Route 15",
"Route 16",
"Route 17",
"Route 18",
"Route 22",
"Route 24",
"Route 25",
"Route 26",
"Route 27",
"Route 28",
"Route 29",
"Route 30",
"Route 31",
"Route 32",
"Route 33",
"Route 34",
"Route 35",
"Route 36",
"Route 37",
"Route 38",
"Route 39",
"Route 42",
"Route 43",
"Route 44",
"Route 45",
"Route 46",
"Pallet",
"Viridian",
"Pewter",
"Cerulean",
"Lavender",
"Vermilion",
"Celadon",
"Fuchsia",
"Cinnabar Island",
"Indigo Plateau",
"Saffron",
"New Bark",
"New Bark Elm's Lab 1F",
"New Bark Elm's Lab 2F",
"New Bark Player House 1F",
"New Bark Player House 2F",
"New Bark Southwest House",
"New Bark Rival House 1F",
"Cherrygrove",
"Cherrygrove Poké Mart",
"Cherrygrove Pokémon Center 1F",
"Cherrygrove Southwest House",
"Cherrygrove Guide Gent House",
"Cherrygrove Southeast House",
"Violet",
"Azalea",
"Cianwood",
"Goldenrod",
"Olivine",
"Ecruteak",
"Ecruteak Poké Mart",
"Ecruteak Gym",
"Ecruteak Pokémon Center 1F",
"Ecruteak Unused House",
"Ecruteak Bell Tower Barrier Station",
"Ecruteak Dowsing Machine House",
"Ecruteak Southwest House",
"Ecruteak Dance Theater",
"Mahogany",
"Lake Of Rage",
"Blackthorn",
"Mount Silver",
"Route 19",
"Route 20",
"Route 21",
"Route 40",
"Route 41",
"National Park",
"Route 31 Violet Gatehouse",
"Route 32 Ruins Of Alph Gatehouse",
"Union Cave 1F",
"Azalea Ilex Forest Gatehouse",
"Route 35 Goldenrod Gatehouse",
"Route 35 National Park Pokeathalon Gatehouse",
"Route 36 Ruins Of Alph Gatehouse",
"Route 36 National Park Gatehouse",
"Ecruteak Unused Gatehouse",
"Diglett Cave",
"Mount Moon",
"Rock Tunnel 1F",
"Pal Park",
"Sprout Tower 1F",
"Bell Tower 1F",
"Goldenrod Radio Tower 1F",
"Ruins Of Alph",
"Slowpoke Well Entrance",
"Olivine Lighthouse 1F",
"Mahogany Souvenir Shop",
"Ilex Forest",
"Goldenrod Tunnel 1F",
"Mount Mortar 1F Entrance",
"Ice Path 1F",
"Whirl Islands 1F",
"Mount Silver Cave 1F",
"Dark Cave Route 45 Side",
"Victory Road 1F",
"Dragons Den Entrance",
"Tohjo Falls",
"Route 30 Apricorn House",
"Ecruteak Pokémon Center Unused 1",
"Ecruteak Pokémon Center Unused 2",
"Ecruteak Pokémon Center Unused 3",
"Ecruteak Pokémon Center B1F",
"Route 42 Ecruteak Gatehouse",
"Mahogany East House",
"Route 29 Route 46 Gatehouse",
"Violet Gym",
"Azalea Gym Entrance",
"Goldenrod Gym",
"Olivine Gym",
"Cianwood Gym",
"Mahogany Gym Leader Room",
"Blackthorn Gym",
"Route 43 Mahogany Gatehouse",
"Route 30 Mr Pokémon House",
"Cherrygrove Pokémon Center B1F",
"Cerulean Cave 1F",
"Seafoam Islands 1F",
"Viridian Forest",
"Route 9 Unused",
"Violet Route 36 Gatehouse",
"National Park Unused Gatehouse",
"Route 47",
"Route 48",
"Union Cave B1F",
"Union Cave B2F",
"Sprout Tower 2F",
"Sprout Tower 3F",
"Violet Poké Mart",
"Violet Pokémon Center 1F",
"Violet Pokémon School",
"Violet Northwest House",
"Violet Unused",
"Violet Bellsprout Onyx Trade House",
"Azalea Charcoal Kiln",
"Azalea Kurt House",
"Azalea Poké Mart",
"Azalea Pokémon Center 1F",
"Violet Pokémon Center B1F",
"Azalea Pokémon Center B1F",
"Route 32 Pokémon Center 1F",
"Route 32 Pokémon Center B1F",
"Route 34 Ilex Forest Gatehouse",
"Route 38 Ecruteak Gatehouse",
"Safari Zone Entrance Interior",
"Safari Zone Gate",
"Ecruteak Unused 1",
"Dark Cave Route 31 Side",
"Slowpoke Well B1F",
"Victory Road 2F",
"Victory Road 3F",
"Azalea Gym",
"Slowpoke Well B2F",
"Goldenrod Bike Shop",
"Goldenrod Game Corner Jp",
"Goldenrod Flower Shop",
"Goldenrod Pokémon Center 1F",
"Goldenrod Radio Tower 2F",
"Goldenrod Radio Tower 3F",
"Goldenrod Radio Tower 4F",
"Goldenrod Radio Tower 5F",
"Goldenrod Radio Tower Observation Deck",
"Goldenrod Department Store 1F",
"Goldenrod Department Store 2F",
"Goldenrod Department Store 3F",
"Goldenrod Department Store 4F",
"Goldenrod Department Store 5F",
"Goldenrod Department Store 6F",
"Goldenrod Magnet Train Station 1F",
"Goldenrod Magnet Train Station 2F",
"Goldenrod Tunnel B1F",
"Goldenrod Department Store Basement",
"Goldenrod Tunnel B2F",
"Goldenrod Tunnel Warehouse",
"Goldenrod Bills House",
"Goldenrod Friendship Checker House",
"Goldenrod Northeast House",
"Goldenrod Name Rater House",
"Goldenrod Global Terminal 1F",
"Goldenrod Unused 1",
"Goldenrod Unused 2",
"Goldenrod Unused 3",
"Goldenrod Unused 4",
"Goldenrod Unused 5",
"Goldenrod Unused 6",
"Route 39 Moomoo Farm House",
"Route 39 Moomoo Farm Stable",
"Ecruteak Bell Tower Barrier Station Corridor",
"Burned Tower B1F",
"Ruins Of Alph Unused 2",
"Goldenrod Pokémon Center B1F",
"Olivine Lighthouse 2F",
"Olivine Lighthouse Exterior",
"Olivine Lighthouse 3F",
"Olivine Lighthouse 4F",
"Olivine Lighthouse 5F",
"Olivine Lighthouse Light Room",
"Olivine Pokémon Center 1F",
"Olivine Poké Mart",
"Olivine North House",
"Olivine Northeast House",
"Olivine Northwest House",
"Olivine Cafe",
"Cianwood Kirks House",
"Cianwood Pharmacy",
"Cianwood Camerons House",
"Cianwood Central House",
"Cianwood Pokémon Center 1F",
"Ice Path B1F",
"Ice Path B2F",
"Ice Path B3F",
"SS Aqua Olivine Port Interior",
"Cianwood Pokémon Center B1F",
"Whirl Islands B1F",
"Whirl Islands B2F",
"Whirl Islands B3F Lugia Cave",
"Route 43 Gatehouse",
"Mahogany Pokémon Center 1F",
"Team Rocket Headquarters B1F",
"Team Rocket Headquarters B2F",
"Team Rocket Headquarters B3F",
"Mount Mortar 1F Back",
"Mount Mortar 2F",
"Mount Mortar B1F",
"Dragons Den",
"Fight Area Unused 1",
"Fight Area Unused 2",
"Fight Area Unused 3",
"Fight Area Unused 4",
"Fight Area Unused 5",
"Fight Area Unused 6",
"Fight Area Unused 7",
"Fight Area Unused 8",
"Fight Area Unused 9",
"Battle Park Unused 1",
"Battle Park Unused 2",
"Battle Tower",
"Battle Tower Elevator",
"Battle Tower Unused 1",
"Battle Tower Unused 2",
"Battle Tower Unused 3",
"Battle Tower Unused 4",
"Battle Tower Partner Room",
"Battle Frontier",
"Battle Frontier Frontier Access Gatehouse",
"Battle Frontier Battle Tower Unused",
"Battle Frontier Battle Factory",
"Battle Frontier Battle Hall",
"Battle Frontier Battle Castle",
"Battle Frontier Battle Arcade",
"Cliff Edge Gate",
"Pokeathlon Dome",
"Pokeathlon Dome Interior 1F",
"Pokeathlon Dome Interior 2F",
"Pokeathlon Dome Interior 2F Entrance",
"Pokeathlon Dome Interior B1F Solidarity Room",
"Pokeathlon Dome Interior B1F Trust Room",
"Pokeathlon Dome Interior B1F Potential Room",
"Pokeathlon Dome Interior B1F Friendship Room",
"Dragons Den Shrine",
"Blackthorn West House",
"Blackthorn East House",
"Blackthorn Move Tutor House",
"Blackthorn Poké Mart",
"Blackthorn Pokémon Center 1F",
"Lake Of Rage Hidden Power House",
"Lake Of Rage Fishing Guru House",
"Route 26 North House",
"Route 26 Week Siblings House",
"Tohjo Falls Hidden Room",
"Route 22 Pokémon League Reception Gate",
"Pokémon League Entrance",
"Pokémon League Will Room",
"Pokémon League Koga Room",
"Pokémon League Bruno Room",
"Pokémon League Karen Room",
"Pokémon League Lance Room",
"Pokémon League Hall Of Fame",
"SS Aqua 1F",
"SS Aqua Captain Room",
"SS Aqua 1F Southeast Rooms",
"SS Aqua 1F Southwest Rooms",
"SS Aqua 1F Northeast Rooms",
"Ruins Of Alph Northeast Entrance First Room",
"Ruins Of Alph Northeast Hidden Room",
"Ruins Of Alph Southeast Entrance First Room",
"Ruins Of Alph Underground Hall",
"Ruins Of Alph Southwest Entrance First Room",
"Ruins Of Alph Southwest Hidden Room",
"Ruins Of Alph Northwest Entrance First Room",
"Ruins Of Alph Northwest Hidden Room",
"Ruins Of Alph Southeast Hidden Room",
"Ruins Of Alph Research Center",
"Route 27 House",
"Ruins Of Alph Hall Entrance",
"Ruins Of Alph Northeast Entrance Second Room",
"Ruins Of Alph Southeast Entrance Second Room",
"Ruins Of Alph Northwest Entrance Second Room",
"Ruins Of Alph Southwest Entrance Second Room",
"SS Aqua 1F Northwest Rooms",
"SS Aqua B1F",
"SS Aqua Olivine Port Exterior",
"Route 34 Daycare",
"Bell Tower 2F",
"Bell Tower 3F",
"Bell Tower 4F",
"Bell Tower 5F",
"Bell Tower 6F",
"Bell Tower 7F",
"Bell Tower 8F",
"Bell Tower 9F",
"Bell Tower Roof",
"Bell Tower 10F",
"Cliff Cave",
"Safari Zone 01",
"Safari Zone 02",
"Safari Zone 03",
"Safari Zone 04",
"Safari Zone 05",
"Safari Zone 06",
"Safari Zone 07",
"Safari Zone 08",
"Safari Zone 09",
"Safari Zone 10",
"Safari Zone 11",
"Safari Zone 12",
"Safari Zone 13",
"Safari Zone 14",
"Safari Zone Entrance Exterior",
"Vermilion Pokémon Center 1F",
"Vermilion Pokémon Center B1F",
"Vermilion Poké Mart",
"Vermilion Fishing Dude House",
"Vermilion Pokémon Fan Club",
"Vermilion Central House",
"Vermilion South House",
"Vermilion Gym",
"Route 40 Battle Frontier Gatehouse",
"Olivine Pokémon Center B1F",
"Mahogany Pokémon Center B1F",
"Blackthorn Pokémon Center B1F",
"Celadon Department Store 1F",
"Celadon Department Store 2F",
"Celadon Department Store 3F",
"Celadon Department Store 4F",
"Celadon Department Store 5F",
"Celadon Department Store Roof",
"Celadon Condominiums 1F",
"Celadon Condominiums 2F",
"Celadon Condominiums 3F",
"Celadon Condominiums Roof",
"Celadon Condominiums Roof Room",
"Celadon Game Corner Jp",
"Celadon Prize Corner",
"Celadon Restaurant",
"New Bark Rival House 2F",
"Cianwood North House",
"SS Aqua Vermilion Port Interior",
"SS Aqua Vermilion Port Exterior",
"Route 10 South",
"Route 6 Saffron Gatehouse",
"Route 8 Saffron Gatehouse",
"Route 5 Saffron Gatehouse",
"Route 15 Fuchsia Gatehouse",
"Celadon Pokémon Center 1F",
"Celadon Pokémon Center B1F",
"Celadon Gym",
"Mahogany Gym Room 2",
"Mahogany Gym Room 1",
"Saffron Fighting Dojo",
"Saffron Mr Psychic House",
"Saffron Magnet Train Station 1F",
"Saffron Magnet Train Station 2F",
"Saffron Silph Co Hq",
"Saffron Silph Co Rotom Room",
"Saffron Copycat House 1F",
"Saffron Copycat House 2F",
"Saffron Unused",
"Saffron Pokémon Center 1F",
"Saffron Pokémon Center 2F",
"Saffron Poké Mart",
"Saffron Gym",
"Battle Frontier Frontier Access",
"Goldenrod Global Terminal 2F",
"Goldenrod Global Terminal 3F",
"Route 2 East",
"Route 16 House",
"Route 20 Unused",
"Route 2 House",
"Route 2 Southeast Gatehouse",
"Route 2 Viridian Forest South Gatehouse",
"Route 2 Viridian Forest North Gatehouse",
"Route 16 Gatehouse",
"Route 16 East",
"Route 18 Gatehouse",
"Route 19 Fuchsia Gatehouse",
"Route 19 Route 12 Gatehouse",
"Cerulean Poké Mart",
"Cerulean Gym",
"Cerulean Pokémon Center 1F",
"Cerulean North House",
"Cerulean East House",
"Cerulean West House",
"Cerulean Bike Maniac House",
"Lavender Poké Mart",
"Lavender Pokémon Center 1F",
"Lavender Volunteer Pokémon House",
"Lavender Southwest House",
"Lavender Name Rater",
"Lavender House Of Memories",
"Lavender Radio Station",
"Route 25 Sea Cottage",
"Goldenrod Department Store Elevator",
"Celadon Department Store Elevator",
"Celadon Condominiums Left Elevator",
"Celadon Condominiums Right Elevator",
"Saffron Silph Co Elevator",
"Olivine Lighthouse Elevator",
"Goldenrod Radio Tower Elevator",
"Mount Moon Square Entrance",
"Mount Moon Square",
"Cerulean Cave 2F",
"Cerulean Cave B1F",
"Rock Tunnel B1F",
"Seafoam Islands B1F",
"Seafoam Islands B2F",
"Seafoam Islands B3F",
"Seafoam Islands B4F",
"Seafoam Islands Cinnabar Gym",
"Seafoam Islands Unused",
"Mount Silver Cave Upper Mountainside",
"Mount Silver Cave Lower Mountainside",
"Mount Silver Cave Expert Belt Chamber",
"Mount Silver Cave Moltres Chamber",
"Mount Silver Cave 2F",
"Mount Silver Cave 3F",
"Mount Silver Cave Summit",
"Route 10 Pokémon Center 1F",
"Route 10 Power Plant Repaired",
"Route 5 Unused",
"Route 5 Underground Path Gate",
"Route 6 Underground Path Gate",
"Pewter Museum Of Science",
"Pewter Northeast House",
"Pewter Gym",
"Pewter Poké Mart",
"Pewter Pokémon Center 1F",
"Pewter Pokémon Center B1F",
"Pewter Southwest House",
"Fuchsia Poké Mart",
"Fuchsia Pal Park Entrance",
"Fuchsia Gym",
"Fuchsia Southwest House",
"Fuchsia Pokémon Center 1F",
"Fuchsia Safari Zone Warden House",
"Route 10 Pokémon Center B1F",
"Fuchsia Pokémon Center B1F",
"Whirl Islands B3F",
"National Park Bug Catching Contest",
"National Park Unused",
"Route 10 Power Plant Broken",
"Ruins Of Alph Underground Hall Sinjoh Event",
"Ruins Of Alph Hall Entrance Sinjoh Event",
"Ruins Of Alph Underground Hall Sinjoh Event 2",
"Route 7 Saffron Gatehouse",
"Lavender Pokémon Center B1F",
"Cerulean Pokémon Center B1F",
"Viridian Gym",
"Viridian Northeast House",
"Viridian Trainer House 1F",
"Viridian Trainer House B1F",
"Viridian Poké Mart",
"Viridian Pokémon Center 1F",
"Viridian Pokémon Center B1F",
"Pallet Town Reds House 1F",
"Pallet Town Blues House 1F",
"Pallet Town Oaks Lab",
"Pallet Town Reds House 2F",
"Pallet Town Blues House 2F",
"Cinnabar Island Pokémon Center 1F",
"Cinnabar Island Pokémon Center B1F",
"Route 28 House",
"Route 3 Pokémon Center 1F",
"Route 3 Pokémon Center B1F",
"Mount Moon Square Clefairy Event",
"Mount Silver Pokémon Center 1F",
"Mount Silver Pokémon Center B1F",
"Wifi Plaza Warp Room",
"Route 5 House",
"Mount Moon Square Shop",
"Goldenrod Magnet Train Station 2F Empty",
"Saffron Magnet Train Station 2F Empty",
"Sinjoh Ruins Exterior",
"Sinjoh Ruins Mystri Stage",
"Sinjoh Ruins Cabin",
"Embedded Tower Groudon Room",
"Embedded Tower Kyogre Room",
"Embedded Tower Rayquaza Room",
"Viridian Route 1 Gatehouse",
"Frontier Access Pokémon Center 1F",
"Frontier Access Pokémon Center B1F",
"Frontier Access Poké Mart",
"Frontier Access Move Tutor House",
"Route 5 Underground Path",
"Route 12 Fishing Brother House",
"Safari Zone Gate Pokémon Center 1F",
"Safari Zone Gate Pokémon Center B1F",
"Goldenrod Game Corner",
"Celadon Game Corner",
"Unused",
"Pokémon League Entrance Wifi Room",
"Id Max"};
QString areaName(int32_t a) { return a >= 0 && a < (int)(sizeof kMapNames / sizeof *kMapNames) && *kMapNames[a] ? QString(kMapNames[a]) : QString("Map %1").arg(a); }
// Level caps from docs/levelcaps.md, per badge bit (pret constants/badge.h: Zephyr..Rising = Johto, Boulder..Earth = Kanto).
struct Boss { const char* name; int cap; };
const Boss kJohto[8] = {{"Falkner", 15}, {"Bugsy", 21}, {"Whitney", 24}, {"Morty", 29}, {"Chuck", 35}, {"Jasmine", 40}, {"Pryce", 44}, {"Clair", 50}};
const Boss kKanto[8] = {{"Brock", 61}, {"Misty", 62}, {"Lt. Surge", 63}, {"Erika", 65}, {"Janine", 64}, {"Sabrina", 66}, {"Blaine", 68}, {"Blue", 70}};
// Hardcore rule: cap = the next boss; with gyms in free order that is the lowest cap among the unbeaten ones.
Boss levelCap(uint8_t johto, uint8_t kanto, bool gameClear)
{
    Boss best{"Red", 94};
    auto scan = [&](const Boss* t, uint8_t bits) { bool any = false; for (int i = 0; i < 8; i++) if (!(bits >> i & 1) && (!any || t[i].cap < best.cap)) { best = t[i]; any = true; } return any; };
    if (scan(kJohto, johto)) return best;
    if (!gameClear) return {"Elite Four", 55};
    scan(kKanto, kanto); return best;
}
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
// One NARC from the player's ROM (FNT walk by path), kept in memory.
struct Narc
{
    std::vector<uint8_t> data; std::vector<std::pair<uint32_t, uint32_t>> fat; uint32_t gmif = 0;
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
        uint32_t n = u32(d, 0x18);
        for (uint32_t i = 0; i < n; i++) fat.push_back({u32(d, 0x1C + i * 8), u32(d, 0x20 + i * 8)});
        uint32_t btnf = 0x1C + n * 8; gmif = btnf + u32(d, btnf + 4) + 8;
        data.assign(d.begin(), d.end());
    }

    const uint8_t* file(int i, uint32_t& len) const { if (i < 0 || i >= (int)fat.size()) { len = 0; return nullptr; } len = fat[i].second - fat[i].first; return data.data() + gmif + fat[i].first; }
};

static QRgb bgr555(const uint8_t* p) { uint16_t c; memcpy(&c, p, 2); return qRgb((c & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, ((c >> 10) & 31) * 255 / 31); }

struct Icons   // party icons: NARC a/0/2/0 (NCLR member 0, NCGR per species)
{
    Narc narc; std::map<int, QImage> cache;

    QImage get(int species, bool egg)
    {
        int naix = egg ? 501 : species + 7;
        auto it = cache.find(naix); if (it != cache.end()) return it->second;
        QImage img(32, 32, QImage::Format_ARGB32); img.fill(Qt::transparent);
        uint32_t plen, glen; const uint8_t* pal = narc.file(0, plen); const uint8_t* gfx = narc.file(naix, glen);
        if (pal && gfx && plen >= 0x28 && glen >= 0x30 + 1024)
        {
            int palNo = (naix - 7) < (int)sizeof kIconPal && naix >= 7 ? kIconPal[naix - 7] : 0;
            const uint8_t* colors = pal + 0x28 + palNo * 32;        // NCLR: 0x10 header + 0x18 PLTT header
            const uint8_t* px = gfx + 0x30;                          // NCGR: 0x10 header + 0x20 CHAR header, tiled 4bpp
            for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++)
            {
                int tile = (y / 8) * 4 + x / 8, b = px[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
                int c = (x & 1) ? b >> 4 : b & 15; if (!c) continue;
                img.setPixel(x, y, bgr555(colors + c * 2));
            }
        }
        cache[naix] = img; return img;
    }
};

// Overworld trainer sprites: NARC a/0/8/1 (mmodel), one NSBTX per sprite with 32x32 4bpp frames (docs/fase1-sprites.md).
struct Sprites
{
    Narc narc; std::map<int, QImage> cache;

    QImage get(int mmodel, int frame)
    {
        int key = mmodel * 64 + frame; auto it = cache.find(key); if (it != cache.end()) return it->second;
        QImage img(32, 32, QImage::Format_ARGB32); img.fill(Qt::transparent);
        uint32_t len; const uint8_t* d = narc.file(mmodel, len);
        if (d && len > 0x60 && !memcmp(d, "BTX0", 4))
        {
            uint32_t t; memcpy(&t, d + 0x10, 4);                       // first block = TEX0
            uint32_t texOff, palOff; memcpy(&texOff, d + t + 0x14, 4); memcpy(&palOff, d + t + 0x38, 4);
            uint32_t px = t + texOff + frame * 512, pal = t + palOff;
            if (!memcmp(d + t, "TEX0", 4) && px + 512 <= len && pal + 32 <= len)
                for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++)
                {
                    int b = d[px + (y * 32 + x) / 2], c = (x & 1) ? b >> 4 : b & 15; if (!c) continue;
                    img.setPixel(x, y, bgr555(d + pal + c * 2));
                }
        }
        cache[key] = img; return img;
    }
};

// Human overworld sprites the player can wear: sprite id (what PlayerAvatar_GetSpriteByStateAndGender returns), mmodel
// member, unique frames in the sheet (24 = full player sheet, 12 = NPC sheet), name. Generated from pokeheartgold
// sprites.h + overlay_01_sprite_data.s, filtered to 32x32 sheets with animation set 0 or 7 (docs/fase1-sprites.md).
// shortcut: only ids <= 255 fit the one-instruction patch (movs r0,#id); ids above need the overlay-1 table patch.
struct SpriteDef { uint8_t id; uint16_t mmodel; uint8_t frames; const char* name; };
const SpriteDef kSprites[] = {
    {0,69,24,"Hero"}, {1,0,12,"Babyboy 1"}, {2,1,12,"Babygirl 1"}, {3,2,12,"Boy 1"},
    {4,3,12,"Boy 2"}, {5,4,12,"Boy 3"}, {6,5,12,"Girl 1"}, {7,6,12,"Girl 2"},
    {8,7,12,"Girl 3"}, {9,8,12,"Man 1"}, {10,9,12,"Man 2"}, {11,10,12,"Man 3"},
    {12,12,12,"Woman 1"}, {13,13,12,"Woman 2"}, {14,14,12,"Woman 3"}, {15,16,12,"Middleman 1"},
    {16,17,12,"Middlewoman 1"}, {17,18,12,"Oldman 1"}, {18,19,12,"Oldwoman 1"}, {19,34,12,"Bigman"},
    {22,31,12,"Reporter"}, {24,23,12,"Shopm 1"}, {25,24,12,"Shopw 1"}, {29,25,12,"Assistantm"},
    {30,26,12,"Assistantw"}, {31,36,12,"Badman"}, {33,22,12,"Skierw"}, {34,37,12,"Policeman"},
    {35,38,12,"Idol"}, {36,39,12,"Gentleman"}, {37,40,12,"Lady"}, {40,41,12,"Workman"},
    {41,42,12,"Farmer"}, {42,43,12,"Cowgirl"}, {43,44,12,"Clown"}, {44,45,12,"Artist"},
    {45,46,12,"Sportsman"}, {50,47,12,"Explore"}, {51,48,12,"Fighter"}, {52,49,12,"Campboy"},
    {53,50,12,"Picnicgirl"}, {55,52,12,"Ambrella"}, {56,53,12,"Seaman"}, {59,29,12,"Waiter"},
    {60,30,12,"Waitress"}, {62,27,12,"Gorggeousm"}, {63,28,12,"Gorggeousw"}, {68,11,12,"Man 5"},
    {69,15,12,"Woman 5"}, {70,33,12,"Mystery"}, {97,70,24,"Heroine"}, {99,54,12,"Doctor"},
    {126,215,12,"Leader 1"}, {127,216,12,"Leader 2"}, {128,217,12,"Leader 3"}, {129,218,12,"Leader 4"},
    {130,219,12,"Leader 5"}, {131,220,12,"Leader 6"}, {132,221,12,"Leader 7"}, {133,222,12,"Leader 8"},
    {141,62,12,"Seven 1"}, {142,63,12,"Seven 2"}, {143,64,12,"Seven 3"}, {144,65,12,"Seven 4"},
    {146,67,12,"Gsboy 1"}, {148,58,12,"Gsrivel"}, {168,57,12,"Sunglasses"}, {169,61,12,"Towerboss"},
    {175,55,12,"Maid"}, {193,56,12,"Delivery"}, {211,170,12,"Red"}, {218,171,12,"Thief"},
    {219,83,12,"Itako"}, {220,172,12,"Bozu"}, {223,175,12,"Rkanbuw"}, {224,176,12,"Rkanbum"},
    {225,177,12,"Boarder"}, {229,179,12,"Gang"}, {232,180,12,"Dancer"}, {233,223,12,"Gantetsu"},
    {235,181,12,"Masaki"}, {236,182,12,"Captain"}, {238,213,24,"Hero"}, {239,214,24,"Heroine"}
};
const int kNumSprites = sizeof kSprites / sizeof *kSprites;
// WALKING case of PlayerAvatar_GetSpriteByStateAndGender in Sacred Gold (arm9 main, Thumb): movs r0,#id. Male / female branch.
const uint32_t kSpritePatch[2] = {0x0205C8CC, 0x0205C938}; const uint16_t kSpriteOrig[2] = {0x2000, 0x2061};

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
        if (const char* peer = getenv("HGT_PEER")) { QStringList p = QString(peer).split(':'); if (p.size() == 2) { peerHost = QHostAddress(p[0]); peerPort = p[1].toInt() + 100; } }
        if (port) sock.bind(QHostAddress::AnyIPv4, port + 100);   // +1 would collide with the peer's ghost port when ports are consecutive
        QObject::connect(&sock, &QUdpSocket::readyRead, [this] { receive(); });
        QObject::connect(&timer, &QTimer::timeout, [this] { poll(); });
        timer.start(250);
        spriteId = Config::GetGlobalTable().GetInt("HGT.Sprite") - 1;   // stored +1 so "unset" (0) = game default
        if (const char* e = getenv("HGT_SPRITE")) spriteId = atoi(e);
        if (const char* e = getenv("HGT_DOCK_PAGE")) page = atoi(e);   // headless test: dump a given page
    }

private:
    EmuInstance* emu; std::function<QString()> romPath; QTimer timer; QUdpSocket sock; QHostAddress peerHost; int peerPort = 0;
    QString myName = "Player", peerName = "Friend", jsonPath; uint8_t johto = 0, kanto = 0; bool gameClear = false; uint32_t saveData = 0; int32_t mapId = -1;
    Mon mons[6]; int nMons = 0; std::vector<Entry> log; Icons icons; Sprites sprites;
    int spriteId = -1; uint8_t gender = 0; std::vector<QRect> spriteCells;   // -1 = game default
    int page = 0; std::vector<QRect> tabRects;   // Pokegear-style tool tabs: 0 none, 1 Graveyard, 2 Trainer Sprite; Soul Link is always shown
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
        // PLAYERDATA (save array 1): Options (2 bytes, padded to 4) then PlayerProfile: name u16[8], id, money, gender, language, johtoBadges @0x1A, ..., kantoBadges @0x1F
        uint32_t prof = saveData + 0x10 + nds.ARM9Read32(saveData + 0x23014 + 1 * 0x10 + 8) + 4; char nm[8] = {};
        for (int i = 0; i < 7; i++) { uint16_t c = nds.ARM9Read16(prof + 2 * i); if (c == 0xFFFF) break; nm[i] = dsChar(c); }
        if (nm[0]) myName = nm;
        gender = nds.ARM9Read8(prof + 0x18); johto = nds.ARM9Read8(prof + 0x1A); kanto = nds.ARM9Read8(prof + 0x1F); gameClear = nds.ARM9Read8(prof + 0x1D) & 1;
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
        if (jsonPath.isEmpty()) { QString rom = romPath(); if (!rom.isEmpty() && QFile::exists(rom)) { jsonPath = rom + ".soullink.json"; loadJson(); icons.narc.load(rom, {"a", "0", "2", "0"}); sprites.narc.load(rom, {"a", "0", "8", "1"}); } }
        if (readParty(*nds)) updateLog();
        applySprite(*nds);
        send(); update();
        if (const char* d = getenv("HGT_DOCK_DUMP")) if (++ticks % 8 == 0) grab().save(d);   // headless test: PNG of the dock every 2 s
    }

    // --- trainer sprite ---
    // Keeps the chosen sprite patched into the game; the game reloads the player sprite at the next warp (door).
    // Only touches the two halfwords while they still hold a movs r0,#x, so nothing is written before the game is up.
    void applySprite(NDS& nds)
    {
        for (int i = 0; i < 2; i++)
        {
            uint16_t want = spriteId < 0 ? kSpriteOrig[i] : (uint16_t)(0x2000 | spriteId), cur = nds.ARM9Read16(kSpritePatch[i]);
            if ((cur & 0xFF00) == 0x2000 && cur != want) { nds.ARM9Write16(kSpritePatch[i], want); fprintf(stderr, "HGT: sprite %08X <- %04X\n", kSpritePatch[i], want); }
        }
    }
    const SpriteDef* spriteDef(int id) const { for (auto& d : kSprites) if (d.id == id) return &d; return nullptr; }
    QImage thumb(const SpriteDef* d) { return d ? sprites.get(d->mmodel, d->frames == 24 ? 21 : 9) : sprites.get(gender ? 70 : 69, 21); }   // facing-down stand frame

    void mousePressEvent(QMouseEvent* e) override
    {
        for (int i = 0; i < (int)tabRects.size(); i++) if (tabRects[i].contains(e->pos())) { page = page == i + 1 ? 0 : i + 1; update(); return; }   // click again = close
        if (page != 2) return;
        for (int i = 0; i < (int)spriteCells.size(); i++)
            if (spriteCells[i].contains(e->pos()))
            {
                spriteId = i == 0 ? -1 : kSprites[i - 1].id;
                Config::GetGlobalTable().SetInt("HGT.Sprite", spriteId + 1); Config::Save();
                update(); return;
            }
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

    // Pokegear-style tool bar: an icon tile per tool, the open one lit; the tool opens under the bar, Soul Link stays below it.
    // New features = one more tile + one more page.
    void tabBar(QPainter& g, int& y, const std::vector<Pair>& ps)
    {
        const char* names[] = {"Graveyard", "Sprite"}; int n = 2, tw = 64, x0 = (width() - n * (tw + 8)) / 2 + 4;
        int graves = 0; for (auto& p : ps) if (pairDead(p)) graves++;
        tabRects.clear(); QFont f = g.font();
        for (int i = 0; i < n; i++)
        {
            QRect r(x0 + i * (tw + 8), y, tw, 58); tabRects.push_back(r); bool on = i + 1 == page;
            g.setPen(QPen(on ? cGold : cHead.darker(130), 2)); g.setBrush(on ? cSlot : cHead); g.drawRoundedRect(r.adjusted(1, 1, -1, -1), 6, 6);
            int cx = r.center().x(), iy = r.y() + 6;
            if (i == 0)       // tombstone
            {
                g.setPen(QPen(cFrame, 1)); g.setBrush(graves ? QColor(0x90, 0x90, 0x98) : cSlotDead); g.drawRoundedRect(cx - 9, iy + 2, 18, 22, 8, 8); g.drawRect(cx - 12, iy + 20, 24, 4);
                g.setPen(QPen(cFrame, 2)); g.drawLine(cx, iy + 7, cx, iy + 17); g.drawLine(cx - 4, iy + 10, cx + 4, iy + 10);
                if (graves) { g.setPen(Qt::NoPen); g.setBrush(cRed); g.drawEllipse(r.right() - 18, r.y() + 2, 16, 16); g.setPen(cWhite); f.setBold(true); g.setFont(f); g.drawText(QRect(r.right() - 18, r.y() + 2, 16, 16), Qt::AlignCenter, QString::number(graves)); f.setBold(false); g.setFont(f); }
            }
            else g.drawImage(cx - 16, iy - 4, thumb(spriteDef(spriteId)));   // the trainer you are
            g.setPen(on ? cText : cWhite); f.setPointSize(8); g.setFont(f); g.drawText(QRect(r.x(), r.bottom() - 16, r.width(), 14), Qt::AlignCenter, names[i]); f.setPointSize(10); g.setFont(f);
        }
        y += 64;
    }

    void pageSoulLink(QPainter& g, int& y, const std::vector<Pair>& ps)
    {
        int W = width(); QFont f = g.font();
        if (page) header(g, y, "SOUL LINK");   // separates it from an open tool above
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
                if (p.mine->status == DEAD && p.theirs->status != DEAD) acts << QString("▶ %1's %2 must go: partner %3 fainted (%4).").arg(peerName, p.theirs->nick, p.mine->nick, areaName(p.area));
                if (p.theirs->status == DEAD && p.mine->status != DEAD) acts << QString("▶ Your %1 must go: partner %2 fainted (%3).").arg(p.mine->nick, p.theirs->nick, areaName(p.area));
                if (!pairDead(p) && p.mine->status != p.theirs->status) acts << QString("▶ Box rule: %1 is in the %2, %3 in the %4.").arg(p.mine->nick, p.mine->status == BOX ? "box" : "team", p.theirs->nick, p.theirs->status == BOX ? "box" : "team");
            }
        }
        qint64 since = peerSeen ? (QDateTime::currentMSecsSinceEpoch() - peerSeen) / 1000 : -1;
        if (!(since >= 0 && since < 3)) acts << "▶ Friend not connected.";
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
            g.setPen(cMuted); g.drawText(r.x() + 12, r.y() + 14, areaName(p.area));
            int half = (r.width() - 60) / 2;
            entryText(g, r.x() + 12, r.y() + 18, half, p.mine, false);
            g.setPen(kPairColors[p.color]); f.setBold(true); f.setPointSize(13); g.setFont(f); g.drawText(QRect(r.x() + 12 + half, r.y() + 18, 36, 36), Qt::AlignCenter, p.mine && p.theirs ? "∞" : "·"); f.setBold(false); f.setPointSize(10); g.setFont(f);
            entryText(g, r.x() + 48 + half, r.y() + 18, half, p.theirs, true);
            y += 60;
        }
        if (ps.empty()) { g.setPen(cWhite); g.drawText(16, y + 14, "No catches yet."); y += 24; }
    }

    void pageGraveyard(QPainter& g, int& y, const std::vector<Pair>& ps)
    {
        int W = width(); QFont f = g.font();
        header(g, y, "GRAVEYARD");
        int graves = 0;
        for (auto& p : ps)
        {
            if (!pairDead(p)) continue; graves++;
            QRect r(8, y, W - 16, 56); slotBox(g, r, true);
            g.setPen(cMuted); g.drawText(r.x() + 12, r.y() + 14, areaName(p.area));
            int half = (r.width() - 60) / 2;
            entryText(g, r.x() + 12, r.y() + 18, half, p.mine, false);
            g.setPen(cRed); f.setBold(true); f.setPointSize(13); g.setFont(f); g.drawText(QRect(r.x() + 12 + half, r.y() + 18, 36, 36), Qt::AlignCenter, "✕"); f.setBold(false); f.setPointSize(10); g.setFont(f);
            entryText(g, r.x() + 48 + half, r.y() + 18, half, p.theirs, true);
            y += 60;
        }
        if (!graves) { g.setPen(cWhite); g.drawText(16, y + 14, "Empty. Keep it that way."); y += 24; }
    }

    void pageSprite(QPainter& g, int& y)
    {
        int W = width(); QFont f = g.font();
        header(g, y, "TRAINER SPRITE");
        const SpriteDef* cur = spriteDef(spriteId);
        QRect hr(8, y, W - 16, 44); slotBox(g, hr, false);
        g.drawImage(hr.x() + 10, hr.y() + 6, thumb(cur));
        g.setPen(cText); f.setBold(true); g.setFont(f); g.drawText(hr.x() + 50, hr.y() + 19, cur ? cur->name : "Default"); f.setBold(false); g.setFont(f);
        g.setPen(cMuted); g.drawText(hr.x() + 50, hr.y() + 35, "click a sprite; takes effect at the next door");
        y += 48; spriteCells.clear();
        int cols = (W - 16) / 38, n = kNumSprites + 1;
        for (int i = 0; i < n; i++)
        {
            QRect c(8 + (i % cols) * 38, y + (i / cols) * 40, 36, 38); spriteCells.push_back(c);
            bool sel = i == 0 ? spriteId < 0 : (cur && kSprites[i - 1].id == cur->id);
            g.setPen(QPen(sel ? cGold : cBrown.lighter(140), sel ? 2 : 1)); g.setBrush(sel ? cSlot : cSlotDead); g.drawRoundedRect(c.adjusted(1, 1, -1, -1), 4, 4);
            g.drawImage(c.x() + 2, c.y() + 3, i == 0 ? thumb(nullptr) : thumb(&kSprites[i - 1]));
        }
        y += ((n + cols - 1) / cols) * 40 + 4;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this); g.fillRect(rect(), cBg); QFont f = g.font(); f.setPointSize(10); g.setFont(f);
        int W = width(), y = 0; auto ps = pairs();
        qint64 since = peerSeen ? (QDateTime::currentMSecsSinceEpoch() - peerSeen) / 1000 : -1; bool online = since >= 0 && since < 3;
        QString status = online ? QString(QChar(0x25cf)) + " connected" : since < 0 ? QString(QChar(0x25cb)) + " waiting for friend" : QString(QChar(0x25cb)) + QString(" no signal %1s").arg(since);
        header(g, y, "HG TOGETHER");
        g.setPen(online ? cGreen : cYellow); g.drawText(QRect(0, y - 26, W - 8, 22), Qt::AlignRight | Qt::AlignVCenter, status);
        Boss cap = levelCap(johto, kanto, gameClear); int badges = __builtin_popcount(johto) + __builtin_popcount(kanto);
        g.setPen(Qt::NoPen); g.setBrush(cSlot); g.drawRect(8, y, W - 16, 20); g.setPen(cText);
        g.drawText(QRect(16, y, W - 32, 20), Qt::AlignVCenter, QString("%1 %2 %3  %4  Cap %5 (%6)  %4  %7/16 badges").arg(myName, QChar(0x2194), peerName, QChar(0x00b7)).arg(cap.cap).arg(cap.name).arg(badges));
        y += 26;
        tabBar(g, y, ps);
        if (page == 1) pageGraveyard(g, y, ps); else if (page == 2) pageSprite(g, y);
        pageSoulLink(g, y, ps);
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
