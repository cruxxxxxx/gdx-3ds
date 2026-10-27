# Hosting your own G-Diffuser rendezvous server

The rendezvous server lets G-Diffuser 3DS consoles find each other over the internet: it hands
out room codes, tells every console where the others are, helps them punch through their routers,
and relays traffic for the few that cannot. The race itself runs on the consoles. One small
machine is plenty: a Raspberry Pi at home, a spare PC, or the smallest cloud VM.

It is one Python file (`gdx_rendezvous.py`, Python 3.8 or newer, standard library only) and it
needs **one UDP port**, 27200 by default.

## 1. Run it

**Plain Python (any OS):**

```sh
python3 gdx_rendezvous.py --json                 # listens on 0.0.0.0:27200
python3 gdx_rendezvous.py --version              # what you are running
```

Leave it running (a terminal, `tmux`, or a service manager). Ctrl-C stops it; a stop drops every
room, and consoles in a room see "room closed" and can host again.

**Docker Compose (Linux):** from this directory,

```sh
docker compose up -d                             # the server alone
docker compose logs -f rendezvous                # its log: one JSON line per event
```

The compose file uses host networking so the server sees each console's real address (needed for
hole punching). Docker Desktop on macOS / Windows hides those addresses: use plain Python there.

**As a Linux service (systemd):** copy the file to `/opt/gdx/` and use the unit in README.md
("Hosting it for real"), or the hardened one in [`gdx-rendezvous.service`](gdx-rendezvous.service).

## 2. Open the port

| where it runs | what to do |
|---|---|
| a cloud VM | allow inbound **UDP 27200** in its firewall / security group. Nothing else is needed: no TCP, no outbound rules |
| at home | in the router: forward **UDP 27200** to the machine running the server (often under "port forwarding" or "virtual server"); give that machine a fixed LAN address (a DHCP reservation) |

Behind carrier-grade NAT (many mobile and some home ISPs) a forward cannot work: your router does
not have a public IPv4 of its own. If the router's WAN address differs from what
<https://ifconfig.me> shows, you are behind CGNAT; use a cheap VM instead.

Check it from another network (a phone hotspot is fine):

```sh
python3 gdx_rv_probe.py YOUR.PUBLIC.IP:27200              # {"ok": true, "rtt_ms": ...}
python3 gdx_rv_probe.py YOUR.PUBLIC.IP:27200 --mode room  # a whole room create / keepalive / leave
```

## 3. Point the consoles at it

Every player puts the server's **public IPv4** into `sdmc:/3ds/gdiffuser/gdiffuser.ini`:

```ini
[net]
rendezvous = 203.0.113.7:27200   ; your server's public IPv4 (dotted, not a hostname)
mp_name = PLAYER                 ; optional: the name the others see
```

This replaces the release's default (the project's hosted server, used when the ini has no
`rendezvous` line). The consoles take a dotted IPv4 only, so the server needs a stable address (a
cloud VM's static IP, or a home connection whose IP rarely changes). Then: **VS** on the main menu
-> **PLAY ONLINE -> HOST ONLINE ROOM**, share the code, friends **JOIN WITH CODE** with it (player
guide: `docs/MULTIPLAYER.md`).

Consoles on the same WiFi as the server: if your router does not support NAT hairpinning, those
consoles need the server's **LAN** address instead (`rendezvous = 192.168.1.20:27200`).

## 4. Limits and flags

The defaults suit a public server. The ones you might change:

| flag | default | what it limits |
|---|---|---|
| `--port` | 27200 | the UDP port |
| `--max-rooms` | 2000 | rooms at once |
| `--max-rooms-per-ip` | 8 | rooms one public address may host |
| `--relay-pps` | 400 | relayed datagrams per second per player |
| `--room-bps` | 196608 | relayed bytes per second per room (a 4-player all-relay room needs about 90-120 KB/s) |
| `--ip-pps` | 2000 | datagrams per second from one address |
| `--member-timeout` | 30 s | a silent player is dropped |
| `--log-ip` | full | `full`, `prefix` (/24), `hash` or `none`: how player addresses appear in the log |
| `--metrics-port` | off | Prometheus metrics on 127.0.0.1 (`/metrics`, `/healthz`) |

Versions (since 2.1.0):

| flag | default | what it does |
|---|---|---|
| `--min-client-proto` | this release's | consoles older than this get **UPDATE REQUIRED** instead of a room |
| `--max-client-proto` | this release's | consoles newer than this are told **SERVER NEEDS AN UPDATE**; `0` = no ceiling |
| `--update-url` | (none) | up to 40 characters shown with UPDATE REQUIRED: where players get the update |
| `--allow-v1` | off | also admit development builds from before the cookie (see section 6) |

`python3 gdx_rendezvous.py --help` lists the rest. Never use the "test-only" flags on a public
server. Bandwidth: a race whose links punched through costs the server almost nothing; a fully
relayed 4-player race is about 120 KB/s each way (about 0.4 GB per hour).

The log holds players' IP addresses (personal data in many places). Keep it only as long as you
need it (the compose file rotates Docker's log at 3 x 10 MB; journald: `MaxRetentionSec=14day`),
or run with `--log-ip hash` or `--log-ip none`.

## 5. Optional: dashboards and alerts

```sh
cp .env.example .env                       # then edit; RV_LOKI_URL ships the access log to Loki
openssl rand -base64 18 > grafana-admin-password
mkdir -p data/prometheus data/loki data/grafana && sudo chown -R 65534:65534 data
docker compose --profile monitoring up -d
```

This adds Prometheus (metrics, 30 days), Loki (the room / join / leave log, 14 days), Grafana with
the "G-Diffuser rendezvous" dashboard and alert rules, and a probe that pings the server every
15 s. Everything listens on 127.0.0.1 only; open Grafana through an SSH tunnel:

```sh
ssh -L 3000:127.0.0.1:3000 you@your-server     # then http://127.0.0.1:3000, user admin
```

Alerts: add a contact point (email, Discord, a webhook...) in Grafana under Alerting -> Contact
points and make it the default policy. The stack wants about 1 GB of RAM.

## 6. Keep it healthy

- **Updates.** Replace `gdx_rendezvous.py` and restart. To restart without cutting races short:
  `kill -USR1 <pid>` stops new rooms and exits once the last room closes (exit code 75); start it
  again after that. Under Compose, `docker compose kill -s SIGUSR1 rendezvous` does the same and
  Docker starts the new file by itself.
- **Game updates and old consoles.** Every console sends its client version (a small number,
  `client_proto`; `--version` prints the one this server was released with). A new release of
  the game that changes the multiplayer protocol comes with a new `gdx_rendezvous.py`: once you
  run it, consoles still on the previous release see **UPDATE REQUIRED - this server needs
  version N (you have M)** (plus your `--update-url`), and consoles newer than your server see
  **SERVER NEEDS AN UPDATE** (update the file). To give players time, start the new server with
  `--min-client-proto <the previous number>` and drop the flag later; consoles in one room must
  still run the same build, and the lobby says which side should update.
- **Builds before the cookie** (GXRV v1: development builds only, no public release spoke it).
  The server refuses them by default: their screen shows the CAN'T JOIN / VERSION dialog (the log: `version mismatch: host runs
  newer-version(GXRV-v2):PLEASE-UPDATE`). If some of your players cannot update yet, add
  `--allow-v1` (in `.env`: `RV_ARGS=--allow-v1`); their requests then skip the spoofing check the
  cookie adds, so remove it when they have updated. Current consoles use v2 either way.
- **Abuse.** Floods and junk are rate-limited and dropped by the server itself; the `abuse` log
  line every minute names the noisiest sources. A firewall rule that drops a source is the next
  step if one keeps going.
- **Shutting down for good.** Consoles still pointing at your address just get "no answer from
  the server". Tell your players to clear or change `rendezvous =`.
