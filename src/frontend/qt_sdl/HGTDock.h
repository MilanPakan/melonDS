// HG-Together: Soul Link dock (see docs/soullink.md). The game stays the central widget; this panel docks beside it.
#pragma once
#include <QDockWidget>
#include <functional>

class EmuInstance;
class HGTPanel;

class HGTDock : public QDockWidget
{
    Q_OBJECT
public:
    // romPath is resolved lazily: the ROM is loaded after the window is built.
    HGTDock(QWidget* parent, EmuInstance* emu, std::function<QString()> romPath);
private:
    HGTPanel* panel;
};
