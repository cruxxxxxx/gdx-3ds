/* port/3ds/os/gdx3ds_config.c -- minimal INI loader, see gdx3ds_config.h for the format.
 *
 * Design constraints: tiny, zero dependencies beyond libc, no dynamic allocation (fixed
 * table -- the whole point is a handful of keys), portable C so the host build can compile
 * and unit-test it identically to the 3DS build.
 */
#include "gdx3ds_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GDX3DS_CFG_MAX_ENTRIES 128
#define GDX3DS_CFG_SECTION_LEN 24
#define GDX3DS_CFG_KEY_LEN 32
#define GDX3DS_CFG_VALUE_LEN 64
#define GDX3DS_CFG_LINE_LEN 160

typedef struct {
    char section[GDX3DS_CFG_SECTION_LEN];
    char key[GDX3DS_CFG_KEY_LEN];
    char value[GDX3DS_CFG_VALUE_LEN];
} Gdx3dsCfgEntry;

static Gdx3dsCfgEntry sEntries[GDX3DS_CFG_MAX_ENTRIES];
static int sEntryCount = 0;
/* Set once the first gdx3ds_config_load attempt COMPLETES (missing file included --
 * defaults are then final). Lets one-shot consumers that latch a key at first use
 * (e.g. the gputrace latch in gdx3ds_gpu_prof.c) defer until the table is real
 * instead of latching a fallback forever. */
static int sLoadedOnce = 0;

int gdx3ds_config_loaded(void) {
    return sLoadedOnce;
}

static void gdx3ds_cfg_copy_lower(char* dst, size_t dstLen, const char* src) {
    size_t i;
    for (i = 0; i + 1 < dstLen && src[i] != '\0'; i++) {
        dst[i] = (char) tolower((unsigned char) src[i]);
    }
    dst[i] = '\0';
}

/* Trim leading/trailing whitespace in place; returns the first non-space character. */
static char* gdx3ds_cfg_trim(char* s) {
    char* end;
    while (isspace((unsigned char) *s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char) end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

/* Cut a trailing comment ('#' or ';'), respecting nothing fancy -- values needing literal
 * '#'/';' are out of scope for a handful of numeric/bool keys. */
static void gdx3ds_cfg_strip_comment(char* s) {
    char* p = strpbrk(s, "#;");
    if (p != NULL) {
        *p = '\0';
    }
}

int gdx3ds_config_load(const char* path) {
    char line[GDX3DS_CFG_LINE_LEN];
    char section[GDX3DS_CFG_SECTION_LEN] = "";
    FILE* fp;

    sEntryCount = 0; /* reload replaces the table even if the file is gone */

    fp = fopen(path, "r");
    if (fp == NULL) {
        sLoadedOnce = 1; /* defaults are final: latch-at-first-use consumers may proceed */
        return 1;        /* missing file = defaults; normal on first boot */
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* s;
        char* eq;
        gdx3ds_cfg_strip_comment(line);
        s = gdx3ds_cfg_trim(line);
        if (*s == '\0') {
            continue;
        }
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (close != NULL) {
                *close = '\0';
                gdx3ds_cfg_copy_lower(section, sizeof(section), gdx3ds_cfg_trim(s + 1));
            }
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL || sEntryCount >= GDX3DS_CFG_MAX_ENTRIES) {
            continue; /* not a key=value line, or table full: skip quietly */
        }
        *eq = '\0';
        {
            Gdx3dsCfgEntry* e = &sEntries[sEntryCount];
            char* key = gdx3ds_cfg_trim(s);
            char* value = gdx3ds_cfg_trim(eq + 1);
            if (*key == '\0') {
                continue;
            }
            memcpy(e->section, section, sizeof(e->section));
            gdx3ds_cfg_copy_lower(e->key, sizeof(e->key), key);
            /* Values keep their case (paths etc.); only lookup keys are folded. */
            snprintf(e->value, sizeof(e->value), "%s", value);
            sEntryCount++;
        }
    }

    fclose(fp);
    sLoadedOnce = 1;
    return 0;
}

const char* gdx3ds_config_get_string(const char* section, const char* key, const char* fallback) {
    char wantSection[GDX3DS_CFG_SECTION_LEN];
    char wantKey[GDX3DS_CFG_KEY_LEN];
    int i;

    gdx3ds_cfg_copy_lower(wantSection, sizeof(wantSection), (section != NULL) ? section : "");
    gdx3ds_cfg_copy_lower(wantKey, sizeof(wantKey), (key != NULL) ? key : "");

    for (i = 0; i < sEntryCount; i++) {
        if (strcmp(sEntries[i].section, wantSection) == 0 && strcmp(sEntries[i].key, wantKey) == 0) {
            return sEntries[i].value;
        }
    }
    return fallback;
}

int gdx3ds_config_get_int(const char* section, const char* key, int fallback) {
    const char* v = gdx3ds_config_get_string(section, key, NULL);
    char* end;
    long parsed;
    if (v == NULL || *v == '\0') {
        return fallback;
    }
    parsed = strtol(v, &end, 0); /* base 0: accepts decimal and 0x hex */
    if (end == v) {
        return fallback; /* not a number at all */
    }
    return (int) parsed;
}

/* ---- MENU write-back (v1 bottom-screen menu) --------------------------------------
 * The menu persists user-changed values by updating the in-memory table and rewriting
 * the INI from it. The loader stores EVERY key it parses (unknown sections/keys
 * included), so a rewrite preserves foreign keys; only comments/ordering are lost,
 * which the header line documents. */

int gdx3ds_config_set_string(const char* section, const char* key, const char* value) {
    char wantSection[GDX3DS_CFG_SECTION_LEN];
    char wantKey[GDX3DS_CFG_KEY_LEN];
    int i;

    if (key == NULL || value == NULL) {
        return 1;
    }
    gdx3ds_cfg_copy_lower(wantSection, sizeof(wantSection), (section != NULL) ? section : "");
    gdx3ds_cfg_copy_lower(wantKey, sizeof(wantKey), key);
    if (wantKey[0] == '\0') {
        return 1;
    }

    for (i = 0; i < sEntryCount; i++) {
        if (strcmp(sEntries[i].section, wantSection) == 0 && strcmp(sEntries[i].key, wantKey) == 0) {
            snprintf(sEntries[i].value, sizeof(sEntries[i].value), "%s", value);
            return 0;
        }
    }
    if (sEntryCount >= GDX3DS_CFG_MAX_ENTRIES) {
        return 2; /* table full: value applies in RAM callers, but cannot persist */
    }
    memcpy(sEntries[sEntryCount].section, wantSection, sizeof(wantSection));
    memcpy(sEntries[sEntryCount].key, wantKey, sizeof(wantKey));
    snprintf(sEntries[sEntryCount].value, sizeof(sEntries[sEntryCount].value), "%s", value);
    sEntryCount++;
    return 0;
}

int gdx3ds_config_set_int(const char* section, const char* key, int value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    return gdx3ds_config_set_string(section, key, buf);
}

int gdx3ds_config_save(const char* path) {
    /* Emit sections in first-appearance order; each entry exactly once. */
    FILE* fp = fopen(path, "w");
    int i;
    int j;
    if (fp == NULL) {
        return 1;
    }
    fprintf(fp, "; gdiffuser.ini - rewritten by the in-game menu (comments are not preserved)\n");
    /* Section-less entries must precede any [section] header. */
    for (i = 0; i < sEntryCount; i++) {
        if (sEntries[i].section[0] == '\0') {
            fprintf(fp, "%s = %s\n", sEntries[i].key, sEntries[i].value);
        }
    }
    for (i = 0; i < sEntryCount; i++) {
        int seen = 0;
        if (sEntries[i].section[0] == '\0') {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(sEntries[j].section, sEntries[i].section) == 0) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }
        fprintf(fp, "\n[%s]\n", sEntries[i].section);
        for (j = i; j < sEntryCount; j++) {
            if (strcmp(sEntries[j].section, sEntries[i].section) == 0) {
                fprintf(fp, "%s = %s\n", sEntries[j].key, sEntries[j].value);
            }
        }
    }
    fclose(fp);
    return 0;
}

int gdx3ds_config_get_bool(const char* section, const char* key, int fallback) {
    const char* v = gdx3ds_config_get_string(section, key, NULL);
    if (v == NULL || *v == '\0') {
        return fallback;
    }
    if (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
        strcasecmp(v, "on") == 0) {
        return 1;
    }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0 ||
        strcasecmp(v, "off") == 0) {
        return 0;
    }
    return fallback;
}
