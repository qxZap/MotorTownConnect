# AGENTS.md -- MotorTown Connect

A native Motor Town plugin: one DLL loaded by
[Plugin Loader](https://github.com/qxZap/mt-plugin-loader). Source is `plugins/motortown_connect.cpp`.
`include/` is a copy of the loader's headers; refresh it from mt-plugin-loader when the api version
changes. The working copy lives in the private mt_loader tree (`plugins/motortown_connect.cpp`, formerly
`net_tuning.cpp`); copy changes here by hand.

## Build and test

`build.bat` (MSVC 2022 x64) writes `out\`. Install to `MotorTown\Binaries\Win64\plugins\` with the game or
server closed. There is no test suite: the log `plugins\logs\MotortownConnect\<date>.log` is the check. The
plugin's own `selfCheck()` covers the UserEngine.ini block logic and logs `self-check: ok`.

## Rules

- The loader owns the ProcessEvent hook and engine discovery. Use `include/mt_plugin_api.h`.
- `process_event` only on the game thread (`run_on_game_thread`); object scans run on the plugin thread.
- Never bypass or fake the game's data check (GameDataHash) or Steam authentication.
- Settings live in `Binaries\Win64\plugins\motortown_connect.ini`; an old `net_tuning.ini` value is taken
  over while the new file has none.

## This plugin

- **Steam send rate.** Steam sockets clamps each connection to SendRateMin..SendRateMax (256 KB/s each by
  default). The global value is copied into the listen socket when the server opens it, and connections
  inherit from the socket, so `earlyRate` sets it as soon as Steam's pipe answers
  (`SteamGameServer_GetHSteamPipe` / `SteamAPI_GetHSteamPipe`) -- before the world loads. Calling the
  networking-utils accessor before Steam is up returned null and kept doing so; hence the pipe check.
  Measured on a 51-player server before the fix: bursts stopping exactly at 256 KB/s.
- **DirectIP.** Writes or removes a marked block in `MotorTown\Config\UserEngine.ini`
  (`bUseSteamNetworking=false` + IpNetDriver as GameNetDriver): the game's own `ip-fallback` transport.
  Read by the engine before plugins run, so it applies from the next start. MTNet writes only its two keys
  in that file, so the two coexist.
- **Measurement.** fps/tick every 10 s, driver/connections/vehicles/cvars every 30 s, client hitches over
  100 ms with live-object-count movement.
