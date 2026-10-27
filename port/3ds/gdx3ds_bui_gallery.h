/* port/3ds/gdx3ds_bui_gallery.h — canned views of every bottom-screen product screen, set up as
 * the design's mockups are (same names, same rooms, same focus), so a screen can be rendered with
 * no network, no race and no input: by the host tests (pixel-diffed against the mockups) and by
 * the console itself under [debug] bui_gallery=1 (one autotest/bui_<id>_R.bmp per entry, the
 * emulator evidence for the comparison page). Pure C, like gdx3ds_bui_screens.c. */
#ifndef GDX3DS_BUI_GALLERY_H
#define GDX3DS_BUI_GALLERY_H

#include "gdx3ds_bui_kit.h"
#include "gdx3ds_bui_screens.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUI_GALLERY_MAX_MASKS 6

typedef struct BuiGalleryEntry {
    const char* id;      /* the mockup's id (artifacts/bottom-menus-design), or a new one */
    const char* mockup;  /* the mockup to diff against, NULL = none (a screen the design lacks) */
    const char* note;    /* what the masks cover and why it differs (the comparison page) */
    uint32_t golden;     /* RGB565 hash of the render (host tests), 0 = not recorded */
    int maskCount;
    BuiRect masks[BUI_GALLERY_MAX_MASKS];
} BuiGalleryEntry;

int bui_gallery_count(void);
const BuiGalleryEntry* bui_gallery_entry(int i);
/* Entry i as a race-dash piece (gdx3ds_bui_racedash.h) rather than a BuiView screen: it draws the
 * whole surface itself. NULL = a screen (bui_gallery_view + bui_screen_paint). */
typedef void (*BuiGalleryDraw)(void);
BuiGalleryDraw bui_gallery_draw(int i);
/* The canned view of entry i. */
void bui_gallery_view(int i, BuiView* v);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_BUI_GALLERY_H */
