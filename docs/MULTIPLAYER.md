# Multiplayer

Race 2–4 players, each on their own 3DS. Every console shows its own racer full screen, and CPU
machines can fill the rest of the field. There are three ways to play:

| | Needs | Good for |
|---|---|---|
| **PLAY NEARBY** | consoles in the same room: the 3DS's **local wireless** (no router), or the **same Wi-Fi** | friends sitting together |
| **PLAY ONLINE** with a room code | Wi-Fi and internet; an online server (one is set up for you) | friends anywhere |
| **JOIN BY ADDRESS** | the host forwards one UDP port on their router | online play with no server at all |

All three work on a fresh install: you don't need to edit `gdiffuser.ini`.

## Before you start

- **Everyone needs the same release download.** The Old 3DS and New 3DS builds of one release
  play together. Expansion Kit (`-EK`) and non-Expansion-Kit builds can't share a room.
- **Everyone needs archives prebaked from the same ROM** (F-Zero X USA rev 0, which is the only
  one `prebake.py` accepts). It doesn't matter which PC or operating system ran the prebake.
- Your name in rooms comes from your 3DS profile. You can change it on the MULTIPLAYER menu
  (the row at the bottom, or **Y**).

## Open the MULTIPLAYER menu

On **SELECT MODE** (the title screen, then START), move to the **VS** icon and press **A**. The top
screen stays where it is and the bottom screen opens **MULTIPLAYER**, with two cards:
**PLAY NEARBY** and **PLAY ONLINE**. Use the touch screen, or the D-Pad and **A** / **B**.

## Play nearby (no internet)

- **Host:** PLAY NEARBY → **HOST A ROOM**, then choose **LOCAL WIRELESS** (3DS to 3DS, no router;
  the wireless switch must be on) or **WIFI** (everyone on the same Wi-Fi network).
- **Join:** PLAY NEARBY. Rooms appear in the list as they're found. Tap one, then **JOIN**. A room
  shown greyed out runs a different build (see "Before you start").

## Play online with a room code

- **Host:** PLAY ONLINE → **HOST ONLINE ROOM**. The lobby shows a 5-character room code. Tell it
  to your friends.
- **Join:** PLAY ONLINE → **JOIN WITH CODE**, type the code on the keypad, and tap **JOIN**.

Consoles connect to each other directly where they can, and go through the server's relay when
a router blocks that.

**The server.** This release uses the project's hosted rendezvous server by default, so online
rooms work without any setup. That default is temporary: a later release will drop it. Everyone
in a room has to use the same server. To use another one (your own, or a friend's), set its
address in `sdmc:/3ds/gdiffuser/gdiffuser.ini`; to turn online rooms off, set it to `off`:

```ini
[net]
rendezvous = 203.0.113.7:27200   ; another server: a dotted IPv4 address (no hostnames), :port optional
; rendezvous = off               ; no online rooms (PLAY NEARBY still works)
```

Running your own server takes one Python file and one open UDP port:
[`tools/mp-rendezvous/SELF-HOST.md`](../tools/mp-rendezvous/SELF-HOST.md).

## Join by address (online, no server)

- **The host** sets `[net] mp_direct = host` in `gdiffuser.ini`, and their router forwards
  **UDP port 27300** to their 3DS. Then: PLAY NEARBY → HOST A ROOM → **WIFI**. The lobby's title
  reads `ROOM <code>`.
- **A joiner** sets `[net] mp_direct = <the host's public IPv4>:27300`, then PLAY ONLINE →
  **JOIN BY ADDRESS**, types the room code, and joins.

```ini
[net]
mp_direct = host                 ; on the hosting 3DS
; mp_direct = 203.0.113.7:27300  ; on each joining 3DS: the host's public address
```

This can't work when the host is behind carrier-grade NAT (most phone hotspots and mobile data).
Use PLAY ONLINE for that.

## In the room

- The host opens machine select for everyone. Each player picks a machine on the normal machine
  select screen, then the host picks the course and starts. Timers keep things moving (60 seconds
  for machines, 30 for the course).
- The host also sets the laps, the room size (2–4) and how many CPU machines fill the field: up to
  30, or up to 16 when an Old 3DS is in the room.
- **START RACE** and leaving the room are held to confirm. A player who isn't ready can be left
  out with **START WITHOUT**.
- After the race: rematch, change the course, or pick machines again.

## During the race

- **START** pauses everyone, and shows who paused. Ending the race from the pause menu is
  confirmed for everyone.
- If a player drops out, a CPU driver takes over their machine and the race goes on.
- Going to the HOME menu or closing the lid takes you out of that race, but you stay in the room.
- Online, if the host leaves a race of three or more, the race carries on for the others. A
  player whose connection drops rejoins the room automatically.
- If the consoles ever disagree about the race, it stops cleanly (**RACE STOPPED**) and a report
  is saved to the SD card.
- On Old 3DS, multiplayer races run in 2D, aiming for 60 fps. Set `[stereo] mp_3d = 1` for 3D,
  which is locked to 30 fps there.

## Troubleshooting

| On screen | What it means | What to do |
|---|---|---|
| **UPDATE REQUIRED**: "THIS SERVER NEEDS VERSION N. THIS 3DS HAS VERSION M." | The online server is newer than your game | Install the latest release. If the server's owner added a web address, it's shown on the screen |
| **SERVER NEEDS AN UPDATE**: "IT KNOWS VERSIONS UP TO N ..." | Your game is newer than the server | Whoever runs the server needs to update it (`gdx_rendezvous.py`). Nothing to fix on your 3DS |
| **NO SERVER**: "CAN'T REACH THE ONLINE SERVER." (no answer) | The server didn't answer | Check that Wi-Fi is on and connected. If you set `rendezvous =` yourself, check it's the right dotted IPv4 address and port (no hostname). Ask the owner whether the server is running and UDP 27200 is open. If the server is on your own home network and your router doesn't loop back, use its LAN address (`SELF-HOST.md` §3). Meanwhile, PLAY NEARBY works |
| PLAY ONLINE reads **NO SERVER IS SET UP** | `rendezvous` is `off` or empty in the ini, or isn't a valid address | Remove the line to use the default server, or set a valid one. A hostname or a typo lands here too (`log.txt` says "is not a dotted IPv4") |
| PLAY NEARBY reads **NEARBY PLAY IS TURNED OFF** | `mp_lan = 0` in the ini | Remove the line, or set `[net] mp_lan = 1` |
| **CAN'T JOIN**: "... RUNS A NEWER VERSION" / "... AN OLDER VERSION" / "... ANOTHER BUILD" | Not everyone has the same download | Everyone installs the same release, and the same edition (EK or not) |
| A NEARBY room is greyed out | It runs a different build | As above |
| **WIFI IS OFF** / **WIRELESS IS OFF** | Wi-Fi is disabled, or the wireless switch is off | Turn Wi-Fi on in the HOME menu, or slide the wireless switch on |
| **ROOM CLOSED** / **CONNECTION LOST** | The host left, or a console stopped answering | You're back on the menu (or still in the room). Host or join again |
| **RACE STOPPED** (out of sync) | The consoles disagreed about the race | A report is saved to the SD card. Please attach it, with `log.txt`, to a bug report |

For a bug report, set `[debug] filelog = 1` in `gdiffuser.ini`, reproduce the problem, and attach
`sdmc:/3ds/gdiffuser/log.txt` (`log-EK.txt` for an EK build). See `docs/3DS-HARDWARE.md` §13.

## The `[net]` keys

You don't need any of these to play. The full list, including test switches, is in
`port/3ds/net/gdx3ds_mp_online.h`.

| Key | Default | What it does |
|---|---|---|
| `mp_lan` | `1` | PLAY NEARBY (local wireless and the same Wi-Fi). `0` turns it off |
| `rendezvous` | the hosted server (this release) | PLAY ONLINE's server: a dotted IPv4 `address[:port]` (port 27200 if left out), or `off` |
| `mp_direct` | (none) | JOIN BY ADDRESS: `host` on the hosting 3DS, `<address>:27300` on a joining one |
| `mp_name` | your 3DS profile name | The name others see (up to 12 letters, digits, spaces, `_ . -`) |
| `mp_players` | `4` | When you host: the room size, 2–4 |

`[net] enabled` is a different setting (a developer log link to a PC). It stays off, and
multiplayer doesn't need it.
