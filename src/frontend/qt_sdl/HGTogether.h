// HG-Together: ghost-player overlay + positie-sync tussen twee melonDS-instances (fase 1).
#pragma once
#include <string>
#include <vector>
class EmuInstance;
namespace HGTogether
{
// Verbindingsstatus per peer voor het dock: naam/sprite uit het laatste positiepakket, since = seconden sinds dat pakket.
struct PeerStatus { std::string name; int gender, sprite, frames; float since; bool connected; };
std::vector<PeerStatus> peerStatus();
// Elk frame na NDS::RunFrame aanroepen vanaf de emu-thread. Doet niets zonder HGT_PORT in de omgeving.
void tick(EmuInstance* emu);
// Vóór RunFrame: gescripte input uit HGT_CTL (fifo). Geeft true als die de echte input vervangt.
bool input(EmuInstance* emu);
}
