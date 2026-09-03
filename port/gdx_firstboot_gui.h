// G-Diffuser — in-window first-time setup flow (ImGui).
//
// A setup screen drawn INSIDE the game window, SoH/BattleShip style: it asks for the three original
// inputs (F-Zero X US rev0 ROM .z64, Expansion Kit disk .ndd, 64DD IPL ROM), validates and copies them
// beside the executable, runs the O2R extraction with live progress, hot-mounts the produced
// fzerox.o2r, and lets boot continue IN-PROCESS (no relaunch).
//
// main() invokes it only when FirstBootRun() returns FirstBootStatus::NeedsSetup, AFTER the window /
// Gui / FileDropMgr exist and BEFORE the game boots — between InitFileDropMgr and
// RegisterResourceFactories, since everything past RegisterResourceFactories requires the ROM. It
// drives its own GUI-only frame pump (Window::RunGuiOnly) while no game threads run.
#pragma once

#include <string>

namespace gdx {

// Blocks (pumping GUI-only frames) until the user completes setup or closes the window.
//
//   dataDir      Absolute data directory (== exeDir; where the inputs + fzerox.o2r live).
//   exeDir       Absolute executable directory (where gdx-extract + decomp-recipes ship).
//   outRomPath   On success, receives the absolute path of the installed ROM (for gdx_init_rom).
//
// True means boot should continue: ROM/disk/IPL installed, completion marker written, and — if
// extraction succeeded — fzerox.o2r hot-mounted. False means the user closed the window and the
// caller should exit cleanly; files already copied beside the exe persist, so the next launch resumes
// with those rows pre-filled.
bool GdxFirstBootSetupRun(const std::string& dataDir, const std::string& exeDir, std::string& outRomPath);

} // namespace gdx
