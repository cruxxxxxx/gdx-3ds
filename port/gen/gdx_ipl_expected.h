/*
 * gdx_ipl_expected.h
 *
 * GENERATED FILE - do not edit by hand.
 * Generator : tools/o2r_harness/gen_ipl_expected.py
 * Source     : deterministic gdx-extract ipl output (n64ddipl.o2r)
 * IPL identity (normalized IPL SHA-256): 806400ec0df94b0755de6c5b8249d6b6a9866124c5ddbdac198bde22499bfb8b
 * Archive SHA-256 (n64ddipl.o2r)       : f42f656eb991e74020fc066b2451e15f8de29c099a310f0e008858bdf5f4db6a
 * Byte-order fmt                        : 0 (z64/native big-endian)
 *
 * PER-USER golden for the 64DD IPL archive (R3 contract C-R3.5). These are
 * self-consistency / dev-drift constants ONLY: the runtime IPL step never gates
 * the mount on them (the archive carries its own ipl/identity entry, and absence
 * falls back to the raw IPL). Regenerate with this build's owner IPL dump after
 * any change to the extractor's IPL path.
 */
#ifndef GDX_IPL_EXPECTED_H
#define GDX_IPL_EXPECTED_H

#define GDX_IPL_EXPECTED_SHA256 "806400ec0df94b0755de6c5b8249d6b6a9866124c5ddbdac198bde22499bfb8b"
#define GDX_IPL_ARCHIVE_EXPECTED_SHA256 "f42f656eb991e74020fc066b2451e15f8de29c099a310f0e008858bdf5f4db6a"

#endif /* GDX_IPL_EXPECTED_H */
