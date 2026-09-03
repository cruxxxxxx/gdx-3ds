// port/gdx_console_log.h — feed for libultraship's Console window. See gdx_console_log.cpp.
#pragma once

// Installs the log taps. Call once, as early in boot as the logger allows.
void GdxConsoleLogInstall();

// Hands buffered lines to the Console window. ImGui thread only, once per frame.
void GdxConsoleLogDrain();
