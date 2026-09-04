# Heap watch — leak triage on hardware (feat/3ds-heapwatch)

## Why
Long sessions showed heapUsed drifting (40.8 -> 50.2 MB over 37 min in round 5). The port
already had owner attribution (`[mem-census]`) and a live allocation histogram + return-
address sampler (`[live-hist]`, `[live-ra]`), but both wrote only to the svc debug channel,
which retail hardware discards, and the census was gated behind verbose. On the console
nothing said WHICH container grew.

## What it does (always on, `[debug] heap_watch` MB, default 8; 0 = off)
- The 5 s `[watchdog] beat` line now carries `hwm=` (high-water mark) and `base=`.
- Baseline = heapUsed at beat 12 (~60 s, after boot loads settle).
- When heapUsed crosses baseline + heap_watch MB, and every further `heap_watch_step` MB
  (default 4), the watchdog asks the main thread for ONE `[heap-watch] trigger=N ...` line
  followed by a `[mem-census]` (seg/tex/wide/setup containers, LUS resource cache bytes,
  big-allocation counters, linear heap) and, if armed, a `[live-hist]` dump. One render-thread
  join per trigger; nothing in between.
- `[debug] heap_watch_arm=1`: arm the live histogram + RA sampler at the first crossing (levels
  count from that moment; two dumps subtract to the retained growth per size class;
  `[live-ra] size=.. ra=..` -> `arm-none-eabi-addr2line -e G-Diffuser-3DS.elf <ra>`).
- All of `[mem-census]`, `[malloc-hist]`, `[live-hist]`, `[live-ra]` now reach the sdmc filelog.
- STAT tab row 5: `linear .. KB hwm .. KB +N/min` (`!` after a trigger fired).

## Reading a session
Cache fill: `lus=` and `tex=` grow then flatten; heap curve plateaus. Leak: a small size class
in `[live-hist]` climbs at a steady rate across dumps; the `[live-ra]` addresses name the site.

## Verified (Azahar, heap_watch=1, arm=1)
Baseline at beat 12 (42.4 MB), trigger at +1.2 MB on race entry, histogram armed, 27 census
lines, 153 live-hist, 51 live-ra lines in log.txt, 0 errors. Cost: none measurable.
