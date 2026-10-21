// HG-Together: ghost-player overlay + positie-sync tussen twee melonDS-instances (fase 1).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
class EmuInstance;
namespace HGTogether
{
// Verbindingsstatus per peer voor het dock: naam/sprite uit het laatste positiepakket, since = seconden sinds dat pakket.
struct PeerStatus { std::string name; int gender, sprite, frames; float since; bool connected; uint8_t johto, kanto; };
std::vector<PeerStatus> peerStatus();
// Co-op-uitnodiging (doel A, UI in het dock): A start een trainerbattle -> A beslist (leader), B krijgt de vraag
// (joiner); beide ja = 2v2, anders solo. Thread-safe; het dock pollt inviteStatus() en roept inviteAnswer() bij een klik.
struct InviteStatus {
    int role = 0;            // 0 = geen uitnodiging, 1 = ik start de battle (leader), 2 = de ander start hem (joiner)
    int phase = 0;           // 0 = vraag aan mij open, 1 = ik zei ja en wacht op de ander, 2 = beide ja: 2v2 start, 3 = solo
    uint32_t trainer = 0;    // trainer-id
    std::string peerName;    // naam van de ander
    float secondsLeft = 0, timeout = 15;   // aftelbalk
};
InviteStatus inviteStatus();
// Zetten na een level-up buiten de battle (doel A): learned = automatisch in een vrij slot gezet; false = alle 4 slots vol,
// overgeslagen ("<soort> kan <zet> leren"). Soort/zet als nummers (namen in het dock). Haalt en leegt de lijst.
struct LearnNotice { uint16_t species, move; bool learned; };
std::vector<LearnNotice> takeLearnNotices();
void inviteAnswer(bool accept);   // alleen zinvol bij phase 0
// Elk frame na NDS::RunFrame aanroepen vanaf de emu-thread. Doet niets zonder HGT_PORT in de omgeving.
void tick(EmuInstance* emu);
// Vóór RunFrame: gescripte input uit HGT_CTL (fifo). Geeft true als die de echte input vervangt.
bool input(EmuInstance* emu);
}
