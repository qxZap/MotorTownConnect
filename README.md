# MotorTown Connect

Motor Town networking plugin: lifts Steam's per-player send cap on a dedicated server and logs what the
network and the server tick are doing.

Motor Town sends all multiplayer traffic through Steam networking, and Steam caps every player's
connection at **256 KB/s**. The engine itself allows 1 MB/s, so Steam's number is the real ceiling.
Most of the time a player needs far less (about 30 KB/s on a 51-player server), but every minute a few
players burst to 150-280 KB/s for 5-15 seconds, for example when a busy area comes into view. At the cap
those updates queue, cars arrive late and uneven, and you see rubber-banding. MotorTown Connect raises the
cap to 1 MB/s.

It changes no game data and touches no file but its own settings, so players join exactly as before. Clients need nothing.

## Install (dedicated server)

Grab the zip from [Releases](../../releases), stop the server, extract it into the server folder (the one
with `RunDedicatedServer.bat`) and start the server. It contains:

- `MotorTown\Binaries\Win64\version.dll`: [Plugin Loader](https://github.com/qxZap/mt-plugin-loader)
- `MotorTown\Binaries\Win64\mt_loader.ini`: logging on, exports off
- `MotorTown\Binaries\Win64\plugins\mt_plugin_MotortownConnect.dll` and `motortown_connect.ini`

Already running the loader? Only the two files in `plugins\` are needed.

## Settings: `plugins\motortown_connect.ini`

| Key | Default | |
|---|---|---|
| `[Steam] SendRateMax` | 1048576 on a server, 0 on a client | Per-player ceiling in bytes/s. 0 leaves Steam's default (256 KB/s). Above the engine's own 1 MB/s it does nothing. |
| `[Steam] SendRateMin` | 0 (Steam's 256 KB/s) | The lowest Steam may back off to. With many players on an upload below ~300 Mbit/s, set 65536. |
| `[General] Disabled` | 0 | 1 = the plugin does nothing. |

Budget the upload: players x SendRateMax is the worst case (100 players x 1 MB/s = 800 Mbit/s). It is a
ceiling, not a target; players only use what the game has to send.

Hosting from single player instead? Set `SendRateMax=1572864` on the host's PC.

## What it logs: `plugins\logs\MotortownConnect\<date>.log`

- at start: `steam sockets: ... SendRateMax 1048576 set N ms after start, before the server's socket opens`.
  Steam copies the value into the listen socket when the server opens it and every player inherits from
  there, so it has to be set first. If the log says `set only now`, restart the server.
- every 10 s: `fps` -- the server tick (mean, worst frame, frames over 33 ms). A driven car replicates at
  most once a tick, so this is also the update rate ceiling.
- every 30 s: the net driver's limits, client connections with their net speed, vehicles by update rate,
  and the vehicle sync console variables.
- client side: frames over 100 ms with how the live object count moved (garbage collection vs loading).

What to watch on a full server: `fps` well under 60 means the tick is the limit (lower NPC traffic first);
the host's upload near its limit means lower `SendRateMax`.

## Build

`build.bat` (MSVC 2022, x64) writes `out\mt_plugin_MotortownConnect.dll`. Needs the loader at api v6.
