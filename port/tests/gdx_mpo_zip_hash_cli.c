/* port/tests/gdx_mpo_zip_hash_cli.c — prints gdx_mpo_zip_cd_hash (the multiplayer content hash,
 * port/gdx_mp_online.c) of each archive named on the command line, one "<hash8> <path>" line each,
 * each folded from the FNV-1a basis. tools/prebake/test_content_hash.py runs it over archives
 * re-packed by prebake.py at different compression levels (they must print the same hash).
 * Exit 1 if any archive is missing or malformed. */
#include <stdint.h>
#include <stdio.h>

#include "gdx_mp_online.h"

int main(int argc, char** argv) {
    int i;
    int bad = 0;
    for (i = 1; i < argc; i++) {
        uint32_t h = 2166136261u;
        const int rc = gdx_mpo_zip_cd_hash(argv[i], &h);
        if (rc != 0) {
            fprintf(stderr, "error: %s rc=%d\n", argv[i], rc);
            bad = 1;
        } else {
            printf("%08lx %s\n", (unsigned long) h, argv[i]);
        }
    }
    return bad;
}
