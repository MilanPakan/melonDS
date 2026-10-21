// HG-Together fase 2: Hardcore-Nuzlocke-watcher (docs/fase2-onderzoek.md). Doet niets zonder HGT_NUZLOCKE=1.
#pragma once
class EmuInstance;
namespace HGTNuzlocke
{
// Elk frame na NDS::RunFrame aanroepen vanaf de emu-thread (naast HGTogether::tick).
void tick(EmuInstance* emu);
// Voor het dock (GUI-thread): true als de vriend een andere HGT_NUZLOCKE_STARTER heeft (beurtvolgorde klopt dan niet).
bool starterMismatch(int& mine, int& friendValue);
}
