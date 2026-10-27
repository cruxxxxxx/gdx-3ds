#pragma once
/* Host-test stand-in for libultraship's CVar bridge: just the three calls port/disk_savefile.cpp
 * makes, so the sidecar code compiles without LUS. The test defines them. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t CVarGetInteger(const char* name, int32_t defaultValue);
void CVarSetInteger(const char* name, int32_t value);
void CVarSave(void);

#ifdef __cplusplus
}
#endif
