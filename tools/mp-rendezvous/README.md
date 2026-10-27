# mp-rendezvous — the online multiplayer rendezvous + relay server

`gdx_rendezvous.py` lets up to 4 G-Diffuser consoles find each other over the internet and
race VS. It is one Python 3 file with no dependencies beyond the standard library, it keeps no
accounts and writes nothing to disk, and it listens on a single UDP port.

The server does four things:

| job | what happens |
|---|---|
| rooms | a host asks for a room and gets a 5-character code (`ABCDEFGHJKLMNPQRSTUVWXYZ23456789`: no I/O/0/1); friends join by typing it |
| endpoints | every member learns every other member's public endpoint (as the server sees it) and private (LAN) endpoint |
| hole punching | everyone gets the roster at the same moment and fires at each other's candidates; the consoles confirm a direct path in both directions before they rely on it |
| relay fallback | a console that cannot reach a peer directly (symmetric / carrier-grade NAT) sends through the server, which forwards the game datagram **verbatim** to the members named in a slot mask |

The race itself never runs on the server. Every console runs the whole race in deterministic
lockstep, and only pads and state digests are exchanged (about 130 B per tick per peer). The
server sees room codes, endpoints, names and those datagrams, nothing else.

## Running it

```sh
python3 tools/mp-rendezvous/gdx_rendezvous.py                 # 0.0.0.0:27200
python3 tools/mp-rendezvous/gdx_rendezvous.py --port 27200 --json > rv.log
```

On the consoles (`sdmc:/3ds/gdiffuser/gdiffuser.ini`):

```ini
[net]
rendezvous = 203.0.113.7:27200   ; dotted IPv4 of the server (:27200 is the default port)
mp_name = PLAYER                 ; optional: what the others see
```

Without a `rendezvous` line a console uses the project's hosted server (`GDX_MPO_DEFAULT_RENDEZVOUS`
in `port/3ds/net/gdx3ds_mp_online.h`; a temporary default that a later release drops), and
`rendezvous = off` turns online rooms off. To play (player guide: `docs/MULTIPLAYER.md`):

1. **VS** on the main menu opens MULTIPLAYER; **PLAY ONLINE -> HOST ONLINE ROOM** shows a code.
2. Friends pick **PLAY ONLINE -> JOIN WITH CODE**, type the code on the keypad and tap **JOIN**.
3. The host opens machine select; everyone picks a machine.
4. The host picks a course and starts.

The race uses the host's course and VS settings. The input delay is picked from the worst RTT
between any two players.

### Hosting it for real

For other people hosting their own server (plain Python, or Docker Compose with an optional
Grafana / Prometheus / Loki stack, a hardened systemd unit): **[SELF-HOST.md](SELF-HOST.md)**.

- **Network.** Open one UDP port (default 27200) inbound. The server needs nothing else: no
  TCP, no DNS, no outbound connections. The consoles take a dotted IPv4, so give the box a
  stable address.
- **Size.** It is light. A room in play is at most 4 relayed streams of about 60 datagrams/s,
  and only for links that could not punch through. One small VM handles many rooms.
- **Supervision.** Run it under systemd (or similar) so it restarts:

  ```ini
  # /etc/systemd/system/gdx-rendezvous.service
  [Unit]
  Description=G-Diffuser online multiplayer rendezvous
  After=network.target

  [Service]
  ExecStart=/usr/bin/python3 /opt/gdx/gdx_rendezvous.py --port 27200 --json
  DynamicUser=yes
  NoNewPrivileges=yes
  ProtectSystem=strict
  Restart=always

  [Install]
  WantedBy=multi-user.target
  ```

- **Restarts.** A restart drops every room. Consoles in a room see "room closed" and host
  again; a race in progress over direct links finishes, but relayed links stop.
- **Test switches.** Never pass the test-only switches (below) on a public instance.

### Options

| option | default | meaning |
|---|---|---|
| `--bind` | `0.0.0.0` | listen address |
| `--port` | `27200` | UDP port (`0` = any; with `--port-file` to learn which) |
| `--max-rooms` | 2000 | room table bound |
| `--max-rooms-per-ip` | 8 | rooms one public IP may host at once |
| `--member-timeout` | 30 s | a member without a keepalive for this long is dropped (consoles send one every 2 s) |
| `--room-max-age` | 6 h | rooms are closed after this, in play or not |
| `--relay-pps` | 400 | relayed datagrams per second per member |
| `--room-bps` | 196608 | relayed bytes per second per room (a 4-player relay-only room at RTT 200 ms needs about 90 KB/s; 98304 before mp-online-hw) |
| `--stats-every` | 60 s | a stats log line (`0` = off), plus an `abuse` line naming the top rate-limited sources |
| `--json` / `--quiet` | | log format |
| `--pending-timeout` | 10 s | a room / member whose address has not sent an authenticated message yet (a spoofed CREATE / JOIN) is dropped after this |
| `--ip-pps` | 2000 | datagrams per second from one address, all types |
| `--allow-v1` (`--no-require-cookie`) | off | legacy escape hatch: also admit GXRV v1 CREATE / JOIN (development builds from before the cookie). Off, a v1 console reads "version mismatch: host runs newer-version(GXRV-v2):PLEASE-UPDATE". `--require-cookie` (the old opt-in) is accepted and is the default |
| `--min-client-proto` | this release's (9) | a v2 console below this client proto gets UPDATE ("UPDATE REQUIRED - this server needs version N (you have M)") |
| `--max-client-proto` | this release's (9) | a v2 console above it is told the server needs an update; `0` = no ceiling |
| `--update-url` | (none) | up to 40 printable ASCII characters shown with UPDATE REQUIRED (where to get the update) |
| `--log-ip` | `full` | source addresses in the log: `full`, `prefix` (/24), `hash` (keyed, the key changes daily and at restart) or `none` |
| `--metrics-port` / `--metrics-bind` | off / 127.0.0.1 | Prometheus text on `/metrics`, `/healthz` (`-1` = any port, with `--metrics-port-file`) |
| `--loki-url` / `--loki-host` | | push the event lines to a Loki `/loki/api/v1/push` endpoint (bounded queue, drops counted, backs off while Loki is down) |
| `--drain-max` | 1800 s | after SIGUSR1: no new rooms, exit 75 when the last room is gone or after this long |
| `--version` | | print the version and the file's sha256 |
| *test only:* `--allow-custom-codes` | off | honour a CREATE's requested code (scripted tests) |
| *test only:* `--force-relay` | off | every room relay-only |
| *test only:* `--sim-symmetric SLOTS` | | advertise a dead public port and no LAN address for those slots, as a symmetric NAT would look |
| *test only:* `--relay-delay-ms` / `--relay-jitter-ms` / `--relay-loss-pct` | 0 | turn the relay into a latency/loss proxy |
| *test only:* `--seed` | | deterministic codes and proxy |

## Security model

Every datagram is hostile until proven otherwise.

**Shape checks.** Exact sizes per message type. Magic, version and type are checked, and the
reserved bytes must be zero. Build ids must be printable ASCII; names and codes come from fixed
whitelists. A private endpoint is accepted only if it is RFC 1918, link-local or loopback.

**No amplification.** No reply to an unauthenticated request is larger than the request:

- a v2 CREATE / JOIN (104 B) gets a 28 B COOKIE, then (cookie checked) a 48 B WELCOME, a 56 B
  ERROR or a 60 B UPDATE; a v1 one (84 B) a 48 B WELCOME or a 56 B ERROR;
- a keepalive whose authentication fails (36 B) gets a 16 B short error;
- the roster (up to 136 B) goes only to members who have authenticated with their 64-bit
  member key.

**Rate limits and blocklist:**

| what | limit |
|---|---|
| requests per IP | 4/s, burst 12 |
| failed joins per IP | 10/min, then `RATE_LIMITED` (stops code guessing: 32^5 is about 3.4 x 10^7 codes, the user picked 5 for typing. A guesser on one IP finds SOME live room about once a day on a full 2000-room server; what it gets is a seat in a stranger lobby, which the host sees and can lock or leave. Codes are a convenience, not a password) |
| junk per source | more than 50 in 10 s blocks that source for 60 s |
| control messages per member | 20/s |
| relay per member | 400 datagrams/s |
| relay per room | a byte budget |

All tables are bounded. Idle IP state is evicted (least recently used first), and members and
rooms expire.

**Relay.** A relayed payload must be a G-Diffuser game datagram (GXMP or GXLB) whose sender
field is the relaying member's own slot. A member therefore cannot speak for another one, and
the relay cannot be pointed at arbitrary hosts: it only reaches members of the same room.

**Consoles trust nothing either:**

- **Lobby packets.** Every GXLB carries the room secret, and its sender must be in the roster.
- **Endpoints.** A peer's address is bound only from an authenticated GXLB. GXMP is accepted
  only from a bound address or from the relay, with a matching slot.
- **Load.** Per-source token buckets and a small blocklist limit what one peer can make a
  console parse.
- **Pads.** Pads are clamped to what an N64 pad can produce.
- **Session token.** A random 64-bit token, the build id and a content hash of both `.o2r`
  archives are folded into the session digest, so a datagram from any other session, build or
  game data is refused.

**Desync.** If two consoles ever disagree about the game state, the per-tick state digest
catches it. The race ends on every console with "out of sync", and everyone returns to the
lobby.

**Not provided:** encryption, and protection from a malicious *member* of your own room. A
room member can always quit, lag or desync the race. Share codes with people you play with.

## Protocol

All integers are little-endian. Strings are fixed-width and NUL-padded.

### GXRV — console ↔ server

Every message starts with this header:

| off | size | field |
|---|---|---|
| 0 | 4 | `GXRV` |
| 4 | 1 | version (1, or 2: see "Version 2" below) |
| 5 | 1 | type |
| 6 | 2 | reserved, must be 0 |

The message types:

| type | name | dir | size |
|---|---|---|---|
| 1 | CREATE | c→s | 84 |
| 2 | JOIN | c→s | 84 |
| 3 | KEEPALIVE | c→s | 36 |
| 4 | LEAVE | c→s | 28 |
| 5 | RELAY | c→s | 28 + payload (≤ 1200) |
| 6 | PUNCH_REQ | c→s | 28 |
| 7 | LOCK | c→s | 28 (host only) |
| 0x81 | WELCOME | s→c | 48 |
| 0x82 | ROSTER | s→c | 24 + 28 per member |
| 0x83 | PUNCH_NOW | s→c | 20 |
| 0x84 | ERROR | s→c | 56 (16 for the unauthenticated keepalive answer) |
| 0x85 | KA_ACK | s→c | 16 |
| 0x86 | COOKIE | s→c | 28 (version 2 only) |
| 0x87 | UPDATE | s→c | 60 (version 2 only) |

**CREATE / JOIN:**

| off | size | field |
|---|---|---|
| 8 | 4 | nonce (a retransmit with the same nonce gets the same answer) |
| 12 | 4 | content hash |
| 16 | 4 | private IPv4 |
| 20 | 2 | private port |
| 22 | 1 | max players (CREATE) |
| 23 | 1 | flags (bit 0: relay only) |
| 24 | 40 | build id |
| 64 | 12 | name |
| 76 | 8 | code (JOIN: required; CREATE: empty = server picks) |

A JOIN is refused with a readable reason when the build or the content hash differs from the
host's. The ERROR detail field carries the host's build id; to a v2 joiner (server 2.1.0) bytes
14..15 of the ERROR carry the host's client proto, so the console says which side should update
("version mismatch: host runs X (update this console)" / "(the host needs an update)" / "(same
version, other build)").

A console that restarts and JOINs again from the same address (with a new nonce) gets its old
slot back once that slot has been silent for 5 s; a live member can never be evicted this way.
The slot's epoch changes, so the others treat it as a new player (a race in progress is aborted). A
host that restarts and CREATEs its own code again (only with `--allow-custom-codes`) takes slot 0
back the same way.

**Authenticated prefix** (KEEPALIVE, LEAVE, RELAY, PUNCH_REQ, LOCK):

| off | size | field |
|---|---|---|
| 8 | 8 | code |
| 16 | 1 | slot |
| 17 | 1 | arg (RELAY: destination slot mask; PUNCH_REQ: target slot; LOCK: 0/1; else 0) |
| 18 | 2 | reserved, 0 |
| 20 | 8 | member key (from WELCOME) |

KEEPALIVE adds the roster version the console holds (u32 at 28) and its clock (u32 at 32). The
KA_ACK echoes the clock, which gives the RTT to the server. If the roster version is stale,
the current ROSTER follows.

**WELCOME:**

| off | size | field |
|---|---|---|
| 8 | 4 | nonce |
| 12 | 8 | code |
| 20 | 1 | slot |
| 21 | 1 | max players |
| 22 | 1 | flags (bit 0: relay only) |
| 23 | 1 | 0 |
| 24 | 8 | member key |
| 32 | 8 | room secret |
| 40 | 4 | your public IPv4 |
| 44 | 2 | your public port |
| 46 | 2 | 0 |

**ROSTER:**

The header:

| off | size | field |
|---|---|---|
| 8 | 8 | code |
| 16 | 4 | version |
| 20 | 1 | count |
| 21 | 1 | flags (bit 0: locked, bit 1: relay only) |
| 22 | 2 | 0 |

Then `count` entries of 28 bytes each:

| off | size | field |
|---|---|---|
| 0 | 1 | slot |
| 1 | 1 | flags (bit 0: host) |
| 2 | 2 | epoch (u16: changes when a different console takes this slot) |
| 4 | 4 | public IPv4 |
| 8 | 2 | public port |
| 10 | 2 | private port |
| 12 | 4 | private IPv4 |
| 16 | 12 | name |

**ERROR:**

| off | size | field |
|---|---|---|
| 8 | 4 | nonce |
| 12 | 1 | code |
| 13 | 1 | 0 |
| 14 | 2 | host client proto (BUILD / CONTENT mismatch to a v2 joiner; else 0) |
| 16 | 40 | detail (printable ASCII; a v1 client keeps it only without spaces) |

The error codes are:

| code | meaning |
|---|---|
| 1 | bad request |
| 2 | room not found |
| 3 | room full |
| 4 | build mismatch |
| 5 | content mismatch |
| 6 | rate limited |
| 7 | server full |
| 8 | room locked |
| 9 | auth (membership expired) |
| 10 | host left |
| 11 | code taken |

**Version 2: the return-routability cookie (required by default since server 2.1.0).** Version 2
is version 1 plus a cookie and the client protocol version on CREATE and JOIN; every other message
is unchanged and may carry either version in its header, and the server answers each member in the
version it joined with. A v2 CREATE / JOIN is 104 bytes:

| off | size | field |
|---|---|---|
| 0 | 84 | the v1 CREATE / JOIN, header version 2 |
| 84 | 16 | cookie (zeros on the first try) |
| 100 | 2 | client proto (`GDX_MPO_PROTO`, port/gdx_mp_online.h) |
| 102 | 2 | 0 |

1. the console sends it with 16 zero bytes;
2. the server answers COOKIE: header (version 2, type 0x86), u32 nonce at 8, 16 cookie bytes at 12
   (28 B);
3. the console sends the same request again at once (same nonce) with those 16 bytes; within 30 s
   it gets the usual WELCOME / ERROR, in version 2, or UPDATE (below).

The cookie is a u32 issue time and the first 12 bytes of HMAC-SHA256(server secret, source IPv4,
source port, nonce, type, time): stateless, and only the address it was sent to can echo it. So a
server never keeps state for a spoofed v2 request, and a v2 request runs on per-address state that
spoofed v1 traffic cannot poison (a victim's address that spoofed junk got blocked can still join
with v2). A stale or forged cookie just gets a fresh COOKIE. A server restart changes the secret.

**The console side** (`port/gdx_mp_online.c` `gdx_rv_cookie_*`, driven by
`port/3ds/net/gdx3ds_mp_p2p.c` through the socket thread like every other datagram), lever
`[net] mp_rv_cookie`:

| value | what the console sends |
|---|---|
| `auto` (default) | v2 first. If two v2 tries (about 1 s) get no answer at all, v1 for the rest of the session (a server before 2.0.0 drops version 2 as junk; two tries stay far below its 50-junk block). If that v1 is refused with "needs GXRV v2" (a requiring server whose first COOKIEs were lost), straight back to v2. A server that has answered in version 2 once is never given up on |
| `1` | always v2 (a server before 2.0.0 never answers: "no answer from the server") |
| `0` | always v1 (old servers, tests); a requiring server refuses it: "version mismatch: this server needs GXRV v2 (set [net] mp_rv_cookie = auto)" |

`auto` is the default because it costs a 2.1.0 server nothing (the COOKIE comes back in one round
trip, so the fallback never triggers) and still reaches a self-hosted server older than 2.0.0
after about 2 s. A console-hosted room (NEARBY / `mp_direct`) is always v1: the host console is
the server there. Receipts: `[mpo] rv cookie: v2 request -> COOKIE -> resend ok (rtt=N ms,
cookies=1, total=N ms)`, `[mpo] rv cookie: auto: 2 v2 requests unanswered (N ms) -> fallback to v1
for this session`, `[mpo] rv cookie: auto: v1 refused (server needs GXRV v2) -> back to v2`,
`[mpo] rv cookie: v1 request -> ok (mp_rv_cookie=..., fell back)`.

**What a build before the cookie sees** against a requiring server: its v1 CREATE / JOIN gets
ERROR `BUILD_MISMATCH` with the detail `newer-version(GXRV-v2):PLEASE-UPDATE` (no spaces: those
builds drop a detail containing one), so its log says `version mismatch: host runs
newer-version(GXRV-v2):PLEASE-UPDATE` and its bottom screen opens the CAN'T JOIN / VERSION dialog
("... RUNS ANOTHER BUILD. EVERYONE NEEDS THE SAME BUILD"). Nothing is created for it. The host runs
`--allow-v1` while such builds must still play. (No public release ever spoke v1.)

**Version gating (server 2.1.0).** The client proto is one number, `GDX_MPO_PROTO` in
`port/gdx_mp_online.h`, mirrored as `CLIENT_PROTO` in `gdx_rendezvous.py` (`test_rendezvous.py`
fails when they differ). It bumps with any GXRV / GXLB / GXMP-online or sim-affecting change, and
it is also in the lobby's HELLO and the sim-ABI word. A cookie-proven v2 request whose proto is
below `--min-client-proto` or above `--max-client-proto` (both default to this release's;
`--max-client-proto 0` = no ceiling) gets UPDATE instead of a room:

| off | size | field |
|---|---|---|
| 8 | 4 | nonce |
| 12 | 2 | minimum client proto |
| 14 | 2 | maximum client proto (0 = none) |
| 16 | 2 | the proto the request carried |
| 18 | 2 | 0 |
| 20 | 40 | text: the server's `--update-url` (printable ASCII, may be empty) |

The console logs `[mpo] server requires client proto>=N (have M)` (or `[mpo] server supports client
proto<=N (have M): the server needs an update`) and its bottom screen shows UPDATE REQUIRED ("THIS
SERVER NEEDS VERSION N. THIS 3DS HAS VERSION M. UPDATE THE GAME, THEN TRY AGAIN." and the text) or
SERVER NEEDS AN UPDATE. In a room, consoles still have to match exactly (proto, build, content and
sim-ABI, checked in the lobby); the lobby's reason names the side: "a player runs a newer version:
update this console", "... an older version: they need an update", "another build of this version:
everyone needs the same one", "the other edition (EK / shipped)".

Policy: a release that bumps `GDX_MPO_PROTO` ships a server whose `CLIENT_PROTO` matches; once a
host upgrades, its default minimum refuses the previous release with UPDATE REQUIRED. To let older
consoles play during a transition, run it with `--min-client-proto <older>` (and v1 clients need
`--allow-v1`); raise it again (or drop the flag) once players have updated. `--version` prints
the server's `client_proto`.

**Unconfirmed rooms (version 1).** Without the cookie, a v1 CREATE / JOIN still creates state, but a
room or member stays *unconfirmed* until the first authenticated message (the KEEPALIVE a console
sends right after its WELCOME) arrives from its address. Unconfirmed rooms live at most
`--pending-timeout`, have their own bounded table (the oldest is evicted first), never count
against `--max-rooms`, and are not written to the access log. A spoofed-source CREATE flood can
no longer fill the room table.

**Probe ping.** A 36-byte KEEPALIVE with an all-zero code field (no console sends one) gets the
16-byte E_AUTH answer like any failed keepalive, but is not counted as junk.
(`gdx_rv_probe.py` uses it.)

### GXLB — console ↔ console (direct or relayed), the lobby

The header:

| off | size | field |
|---|---|---|
| 0 | 4 | `GXLB` |
| 4 | 1 | version (1) |
| 5 | 1 | type (1 HELLO, 2 START, 3 ABORT) |
| 6 | 1 | from slot |
| 7 | 1 | to slot (0xFF = all) |
| 8 | 8 | room secret |
| 16 | 4 | send clock |
| 20 | 4 | echo of the peer's newest send clock |
| 24 | 2 | hold ms (how long that echo sat here; 0xFFFF = nothing to echo) |
| 26 | 2 | 0 |

The message bodies:

| type | size | body |
|---|---|---|
| HELLO | 108 B | protocol, state, ready, content hash, newest START generation acknowledged, machine, skin, engine, bot flag, the slots heard directly (mask), measured RTT to every slot, build id, name |
| START | 84 B | sent only by slot 0: generation, players, course, delay, hash cadence, VS settings, the four RNG seeds/masks, the 64-bit session token, and per player (slot, machine, skin, engine), plus the bot mask |
| ABORT | 40 B | generation, reason (quit, desync, connection lost, suspended, launch failed), culprit player, tick |
| RM_POLL / RM_HAVE / RM_SET | 172 B | removing a player mid-race: generation, player, reason, tick, and up to 32 of that player's pads (`port/gdx_mp_remove.h` has the protocol) |

The exact byte layouts are in `port/gdx_mp_online.c`.

### GXMP — console ↔ console, the lockstep pads

The pad exchange is described in `port/gdx_mp_lockstep.h`. Online sessions add two header
bytes:

- byte 33 holds the flags (bit 0: ABORT);
- byte 34 holds the sender's room slot + 1, which the relay and every receiver check.

## Same-WiFi rooms (NEARBY) need no server

On one network the **host console is the server**: it runs one room of this same GXRV protocol
on its game socket (`port/gdx_mp_lan.c`, a C port of the room logic here, with the same
validation, rate limits, rejoin rule and roster epochs). Joiners use the host's address as their
"rendezvous", so the lobby, lockstep and removal are the online code unchanged. They find the host
by its **GXLN beacon** (UDP, ~1 Hz, to ports 27210-27213, broadcast plus an optional unicast list):

| field | bytes |
|---|---|
| `GXLN`, version 1, type (1 BEACON, 2 PING, 3 PONG), 0, 0 | 8 |
| BEACON: host id, content hash, room port, players, max, model (1 O3DS, 2 N3DS), flags (1 racing, 2 full), protocol, 0, code (8), host name (12), build (40) | 84 total |
| PING / PONG: host id, nonce, send time (a PONG echoes the PING byte for byte) | 20 total |

A beacon is display data only: the NEARBY list greys out a room whose build, content hash or
protocol differs, and the JOIN is checked by the host's room anyway.

A joiner pair that cannot reach each other directly talks through the host's room relay (the
host forwards their datagrams verbatim, as this server does). Before mp-online-hw a joiner refused
those forwarded datagrams (they come from the host's address but name another sender), so such a
pair never heard each other; `[net] mp_lan_relay_rx = 0` restores that for an A/B.

## Serverless online: one console hosts (`[net] mp_direct`)

The NEARBY room above works across the internet too, as long as the joiners can reach the host's
game port. No server is involved: the joiner types the host's room code and its JOIN goes straight
to the host's address.

```ini
; the host (its router forwards UDP 27300 to it, for joiners outside its network)
[net]
mp_lan = 1
mp_direct = host        ; a WiFi room it hosts shows ROOM <code>; the log prints its address

; a joiner
[net]
mp_direct = 203.0.113.7:27300   ; the host's public IPv4 (its LAN IPv4 on the same network)
```

The host opens **MULTIPLAYER -> NEARBY -> HOST A ROOM -> WIFI**. Its lobby title reads `ROOM <code>`.
The joiner opens **PLAY ONLINE -> JOIN BY ADDRESS**, types the code, and joins. Everything after
the JOIN is the NEARBY room's code: code, build and content checks, the room secret, the roster,
relay for joiner pairs, failed-join throttling.

When it works, and when the server is still needed:

| host's network | serverless | server needed for |
|---|---|---|
| home router, UDP 27300 forwarded (or UPnP, not implemented) | works; links DIRECT | nothing |
| home router, nothing forwarded | the JOIN never arrives | the rendezvous (both sides dial out, hole punching) |
| behind carrier-grade NAT (most phone hotspots / mobile data, some ISPs) | cannot be forwarded at all | the rendezvous, and its relay when both sides are behind symmetric NATs |

A server is always the fallback for two consoles that are both behind NAT, because hole punching
needs a third party both can reach to learn each other's public endpoints; the relay is the fallback
when punching fails.

UPnP IGD (the console asking the router to open the port itself) is not implemented. It needs SSDP
discovery (UDP multicast to 239.255.255.250:1900), an HTTP fetch of the router's description XML,
and a SOAP `AddPortMapping` call over TCP: roughly 600-900 lines of C on soc:U, run off the main
thread (blocking TCP with timeouts), plus lease renewal and `DeletePortMapping` on exit. It helps
only when the router has UPnP enabled (often off by default), is not behind a second NAT, and the
ISP gives a public IPv4 (CGNAT defeats it). NAT-PMP / PCP (UDP 5351 on the gateway) is smaller
(about 150 lines) but fewer home routers speak it.

## Developer notes: testing on real consoles

**Stage A, one home network.** Run the server on a computer on the same LAN, in the foreground
(`python3 tools/mp-rendezvous/gdx_rendezvous.py --json`; on macOS / Linux, `serve-home.sh <log
dir>` does that and prints the machine's LAN addresses; Ctrl-C stops it). Allow incoming UDP 27200
in that computer's firewall.

Both consoles: `[net] rendezvous = <that computer's LAN IPv4>:27200`. Over one LAN the server sees
both consoles at their LAN addresses, so the links come up DIRECT. For a relay race in the same
launch, `[net] mp_relay_hold_l = 1` and hold **L** while tapping **HOST ONLINE ROOM**: the room is
relay-only for everyone (the CREATE flag; no server switch), and the lobby badge reads RELAY.

**Stage B, a real internet path (needs a second network, such as a phone hotspot; nothing here
sets it up).** One console on the hotspot, one at home.

- *Via the home server:* the router must forward UDP 27200 to the server's computer, and the
  hotspot console's `[net] rendezvous` must be the home's public IPv4. If the router does NAT
  hairpinning, give the home console the public address too (the server then sees both at their
  public endpoints and they can punch); otherwise the home console keeps the server's LAN address
  and the link is expected to end up RELAY (a valid test of the relay over the internet).
- *Serverless:* the router forwards UDP 27300 to the hosting console instead; the hotspot console's
  `[net] mp_direct = <home public IPv4>:27300`.
- A public server (a small VM) removes the router forward: see "Hosting it for real".

## Levers

| key | default | meaning |
|---|---|---|
| `[net] mp_rv_cookie` | auto | GXRV version of online CREATE / JOIN: `auto` (v2, v1 after two silent tries, back to v2 on "needs GXRV v2"), `1` (always v2), `0` (always v1). See "Version 2" above |
| `[net] mp_rv_proto` | `GDX_MPO_PROTO` | test lever: the client proto a v2 request carries (lower it to see UPDATE REQUIRED; the lobby keeps the real one) |
| `[net] mp_direct` | (none) | `host`: a WiFi room shows its code (serverless host); `a.b.c.d[:port]`: ONLINE's JOIN BY ADDRESS joins that console's room |
| `[net] mp_relay_hold_l` | 0 | 1: L held on HOST ONLINE ROOM = a relay-only room; 2: always (scripted tests) |
| `[net] mp_lan_relay_rx` | 1 | 0: a LAN joiner refuses the host room's relayed datagrams (before mp-online-hw) |
| `[net] mp_online_disc_off` | 1 | 0: the NEARBY discovery socket is still serviced every frame in an online-server room |
| `[net] mp_ready_skip` | 1 | 0: a socket-room race tick drains the socket again even when every pad is in |
| `[net] mp_soc_thread` | 1 | a service thread makes every sendto / recvfrom of the room socket, the main thread only queues (`port/3ds/net/gdx3ds_mp_socthr.h`); 0: inline calls, as before |
| `[net] mp_soc_tx_min_ms` | 33 Old 3DS / 16 New 3DS | a lockstep INPUT at most this often per destination and stream, only the newest (it carries every unacked pad); 0: every packet at once |
| `[net] mp_soc_rx_poll_ms` | 8 Old 3DS / 4 New 3DS | the thread drains the socket (recvfrom until EAGAIN) this often; a local wireless room: `mp_uds_soc_poll_ms` (250) |
| `[net] mp_soc_thread_core` | -2 | the thread's core (-2: the app core) |
| `[net] mp_soc_tx_repeat` | 2 | steady send cadence: a new INPUT goes out again every `mp_soc_tx_min_ms`, up to this many times, while nothing newer replaces it (a render hitch); a resend of unchanged pads (a stall) earns none; 0: off |
| `[net] mp_soc_block_poll` | 0 | 1: in a race, once nothing else touches SOC (dev net thread quiet with no link), the thread blocks in `poll()` up to `mp_soc_rx_poll_ms` instead of a timed wait + `recvfrom` (wakes on arrival; a queued send waits for the poll) - an A/B lever |
| `[net] mp_wait_quiet_ms` | 500 | a race tick waiting on a player silent this long only heartbeats (100 ms) instead of resending; 0: never |
| (fixed) | | a race tick's resends back off: every 16 ms for the first 100 ms of a wait, then 50 ms, past 500 ms only the 100 ms heartbeat; the wait sleeps on the thread's "datagram queued" event (<= 8 ms) instead of 0.5 ms polls |
| `[net] mp_race_disc_ms` | 1000 | the NEARBY discovery socket (beacons, pings) is serviced once this often in a WiFi race, so the room socket's thread is the main SOC user; 0: every frame |
| `[net] mp_timesync_max_ms` | 3 | time sync's stretch ceiling per tick, 0..16 ms (`port/3ds/net/gdx3ds_mp_timesync.h`); `ab` (or -1): per-race A/B, 3 vs 16 ms (`port/gdx_mp_netab.h`, bit 0: flips every race). With either lever on A/B every START logs `[netab] race gen= k= \| net.mp_timesync_max_ms=ab/A=3 net.mp_ndm_exclusive=2/B=0` |
| `[net] mp_ndm_exclusive` | 1 | an online / LAN room holds `NDMU_EnterExclusiveState(INFRASTRUCTURE)` + `NDMU_SuspendDaemons(CEC, BOSS, NIM)`; left when the room closes, at exit and across HOME / sleep (`port/3ds/net/gdx3ds_mp_radio.h`); 0: daemons untouched; 2: per-race A/B, held in arm-A races and left in arm-B ones (`port/gdx_mp_netab.h`, bit 1: flips every 2 races) |
| `[net] mp_net_quiet` | 1 | while a room is up the dev `[net]` thread stops relinking and listening; a live log link drains every 500 ms; 0: as before |
| `[net] mp_rate_ms` | 1000 | the `[mprate]` per-second race receipt; 0: off |
| `[debug] filelog_mp_reserve_kb` | cap / 4 | while a room is up, the last part of the log's size cap only takes MP receipts (`[mpo]`, `[mpipc]`, `[socthr]`, `[mprate]`, `[net]`, ...) and fatal lines; 0: off |
| `[net] relink_backoff` | 1 | the dev log link's reconnect wait doubles after 3 failures, up to 30 s (each attempt polls the shared soc:U session for 1.5 s); 0: every 2 s |

Receipts in the log: `[mpo] delay pick: worst_rtt= worst_jitter= -> D ... relay_only= | sN:direct|punch|relay rtt= jit=`
(host, at START), `[mpo] race end ... stall_ms_per_min=` followed by a `[mpo] stats` line (link,
RTT last/max, tx direct/relay), and `[mpcost]` / `[mpipc]` (per-call SOC send/recv cost) every 600 frames:
`soc_send` / `soc_recv` are the main thread's calls (none with the socket thread), `st_send` / `st_recv` the
thread's, `q_push` / `q_pop` the main thread's ring copies; then `[socthr] queued= sent= coalesced= ... rx= empty=
rxfull= ... txq=now/max rxq=now/max` (the thread's counters since the socket opened). A stall of 3 s or more ends
with `[mpo] stall episode over at tick T: N ms from tick T0 (W waits, M ms waiting) for sK (name)`. Every second of a
race: `[mprate] lt= ms= rx=direct/relay tx=direct/relay stalls= stall_ms= bars=(WiFi 0-3) ndm=(exclusive held)
srv_rtt=`; `[mpo] route sN=<state> <ip>:<port> (pub|priv|learned)` when a peer's direct endpoint changes; `[mpradio]
ndm enter|leave ... state= cec= boss=` (daemon status 3 = suspended). The stall banner names a player as `P<n> <name>`
when two racers share a name.

## Tests

```sh
python3 tools/mp-rendezvous/test_rendezvous.py
tools/mp-rendezvous/emu_matrix.py <3dsx> --players 4 --case p4 --run-root <dir>
```

- **`test_rendezvous.py`** is the server's unit tests. It covers every message type, fuzzing,
  truncation, spoofing, amplification, floods, code guessing, table bounds, expiry and the
  proxy; the v2 cookie, the required-cookie default and `--allow-v1`, version gating (UPDATE both
  ways, the host proto on a mismatch, the client-proto mirror), unconfirmed rooms, metrics, the
  event log, Loki shipping, and the real process (`/metrics`, SIGTERM, SIGUSR1 drain). Its interop
  tests run `port/tests/gdx_mp_online_wire_probe.c` (the console's own wire code and its
  `mp_rv_cookie` state machine) over loopback against a real default server (`v2`: the happy path;
  `refused`: what a pre-cookie build shows, auto recovering a lossy first second, UPDATE REQUIRED and
  SERVER NEEDS AN UPDATE), an `--allow-v1` one (`v1`) and a stand-in for a server before 2.0.0
  that drops every v2 datagram (`old`: auto falls back). `tools/ci-3ds.sh` runs it.
- **`emu_matrix.py`** runs a local server plus N emulator instances that race a VS race with
  bots. Options: `--relay force`, `--sim-symmetric`, `--sim-nat`, `--relay-delay-ms`,
  `--relay-loss-pct`, `--desync-at`, `--leave-at` / `--vanish-at` (a console walks out or loses
  power mid-race: the others must remove it at one agreed tick and finish), `--lan` (NEARBY:
  no server, the host console hosts and the others find it by beacon), and `--direct` (serverless
  online: the others join the host console's room by its address and code). The server it starts
  requires the cookie; `--server-arg=--allow-v1` admits a build before it, and `--expect-message
  REGEX` turns a run into a refusal check (PASS when every console logs a matching `[mpo]
  message`); `--prefix` names the instances.
