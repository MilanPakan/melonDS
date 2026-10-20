// HG-Together: ghost-player overlay + positie-sync tussen twee melonDS-instances (fase 1).
#pragma once
class EmuInstance;
namespace HGTogether
{
// Elk frame na NDS::RunFrame aanroepen vanaf de emu-thread. Doet niets zonder HGT_PORT in de omgeving.
void tick(EmuInstance* emu);
// Vóór RunFrame: gescripte input uit HGT_CTL (fifo). Geeft true als die de echte input vervangt.
bool input(EmuInstance* emu);
}
