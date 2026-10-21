// HG-Together camera-experiment (docs/camera.md): HGT_CAM="pitch,fov,zoom" kantelt de overworld-camera en zet hem optioneel op perspectief.
#pragma once
namespace melonDS { class NDS; }
namespace HGTCamera
{
void apply(melonDS::NDS& nds);   // elk frame na RunFrame; doet niets zonder HGT_CAM of dock-diorama
// Dock-schakelaar (GUI-thread): diorama = stand 30,45,1, uit = spelcamera terugzetten. HGT_CAM in de omgeving gaat voor.
void setDiorama(bool on);
bool envSet();
}
