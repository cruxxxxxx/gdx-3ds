/* port/3ds/gfx/linktest_main.cpp — unresolved-symbol audit driver (stream A).
 *
 * NOT part of the default build (EXCLUDE_FROM_ALL). Linked with
 * --whole-archive around gdx3ds_lus_carve + gdx3ds_gfx so every carve object is
 * pulled in and ALL unresolved externals surface in one pass:
 *
 *   cmake --build build-3ds --target gdx3ds_gfx_linktest 2>&1 | grep 'undefined reference'
 *
 * The link is EXPECTED to fail until the Context/Window/ConsoleVariable/Gui symbol
 * families are either added to the carve or stubbed at integration — the audit
 * result lives in STATUS.md.
 */
#include "gdx3ds_gfx.h"

int main() {
    return Gdx3ds_GetCitro3dRenderer() != nullptr ? 0 : 1;
}
