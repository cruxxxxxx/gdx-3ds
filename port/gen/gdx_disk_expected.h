/*
 * gdx_disk_expected.h
 *
 * GENERATED FILE - do not edit by hand.
 * Generator : tools/o2r_harness/gen_disk_expected.py
 * Source     : deterministic gdx-extract disk output (fzerox-disk.o2r)
 * Disk identity (stored-image SHA-256)  : b8b7c325c25ae418b331f252d865e78b32c4556e3c302c6891380c43c4e6c1a7
 * Archive SHA-256 (fzerox-disk.o2r)     : 9092e6919a4aac095ebdfc772e0ecee39fab7fe7c72d12767eb7b9549ade7ca2
 * Archive entry count                   : 767
 * Byte-order fmt                        : 0 (native / as-is (64DD disks are not byte-order-variant))
 *
 * PER-USER golden for the 64DD EK disk archive (R8 Step 1 + Step 2). These are self-consistency /
 * dev-drift constants ONLY: the runtime disk step never gates the mount on them (the archive
 * carries its own disk/identity entry, and absence falls back to the R7 managed copy / raw .ndd).
 * The entry count = 2 frozen disk/* entries + the ek/<symbol> per-asset entries (R8 Step 2); it is
 * data-driven (grows if the EK slice manifest gains rows), so the launcher reads it from here.
 * Regenerate with this build's owner disk dump after any change to the extractor's disk path.
 */
#ifndef GDX_DISK_EXPECTED_H
#define GDX_DISK_EXPECTED_H

#define GDX_DISK_EXPECTED_SHA256 "b8b7c325c25ae418b331f252d865e78b32c4556e3c302c6891380c43c4e6c1a7"
#define GDX_DISK_ARCHIVE_EXPECTED_SHA256 "9092e6919a4aac095ebdfc772e0ecee39fab7fe7c72d12767eb7b9549ade7ca2"
#define GDX_DISK_EXPECTED_ENTRY_COUNT 767

#endif /* GDX_DISK_EXPECTED_H */
