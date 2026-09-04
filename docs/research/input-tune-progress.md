# Input tune — circle-pad feel and d-pad steering (feat/3ds-inputtune)

## Why
F-Zero X is tuned for the N64 stick. The circle pad reaches the game through a linear map
(raw 0..~156 per axis -> N64 -80..80 with a 16-unit deadzone and full deflection at raw 145),
which had no menu surface, and the d-pad did nothing in a race.

## What the game actually reads
`racer.c` normalises `stickX / 63` and clamps to +-1, so full steer arrives at N64 63 of 80.
With the default range that is raw ~118 of ~156: three quarters of the pad's throw. The game
never reads the N64 d-pad buttons (no CONT_UP/DOWN/LEFT/RIGHT use anywhere in `decomp/src`);
menus derive direction from the stick via `STICK_TO_BUTTON` and OR the d-pad bits in.
So the d-pad can drive the stick everywhere without a decomp change and without double-firing
in menus.

## Added (port side only, `port/3ds/os/gdx3ds_os_ctru.c`, `gdx3ds_menu.c`)
INP tab rows 18-22:
- `DEADZONE [-] n [+]` raw units, 0-80, step 2 (default 16)
- `RANGE    [-] n [+]` raw units where +-80 is reached, 60-156, step 5 (default 145;
  lower = full steer sooner = more sensitive)
- `CURVE    [-] LINEAR/SOFT/SOFTER [+]` response on the normalised magnitude
  (soft = t*(0.5+0.5t), softer = t^2): a gentler centre for fine corrections, endpoints unchanged
- `DPAD     [-] OFF/FULL/RAMP [+]` d-pad steers: FULL = +-80 immediately; RAMP = a tap gives
  24, a hold reaches 80 after 8 polls (~8 frames). Per axis the larger of pad and d-pad wins;
  the d-pad bits stay asserted for menus.
- live readout `pad rawX rawY -> stick sx sy`, repainted at <= 10 Hz while the pad moves
- RESET DEFAULTS restores binds and tuning
Persisted as `[input] deadzone / range / curve / dpad_steer`; live on the next poll, no relaunch.
Defaults reproduce today's behaviour exactly (killswitch by default).

## Verified (Azahar)
Menu opens, INP tab paints, steppers change values and persist (`[menu] input tune dz=18
range=145 curve=0 dpad=2` receipt, ini rewritten), 0 error lines. Feel needs hardware.

## What to watch on hardware
- Default feel unchanged. Then try RANGE 120-130 (full steer earlier) and CURVE SOFT.
- DPAD FULL for menu-style digital steering; RAMP for a softer tap. Check no menu double-steps.
- Readout row: resting pad should show raw within +-16 of 0 (inside the deadzone).
