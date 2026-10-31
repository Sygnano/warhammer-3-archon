# Archon Mode

A Total War: WARHAMMER III mod that lets two or more players share one campaign faction in multiplayer: same treasury, same armies, same turn, each with their own camera and screen.

The game allows one human per faction. Archon Mode lifts that limit with a small native DLL, and adds what a shared faction needs:

- Both players pick the same faction in the multiplayer campaign lobby.
- End Turn waits until everyone on the faction has pressed it.
- Both players join battles and control every unit, and the game's gift button splits the army between them.
- Shared saves reload with everyone back on the faction.
- If a player leaves the campaign, the remaining player keeps the faction.

**Warning:** this is an unofficial mod, not made or supported by Creative Assembly, SEGA or Games Workshop. It runs native code and changes game code in memory while the game runs, which may go against the game's terms of use. Use it at your own risk. If you do not want that on your PC, do not install it.

Tested with two players on two PCs, on game version 9.0.4, in Immortal Empires. Windows with Steam only.

## Using it

Both players need the same version. See [workshop/workshop_description.txt](workshop/workshop_description.txt) for how to start, known limits and troubleshooting.

## How it works

The pack holds Lua scripts and the DLL as an embedded blob. At boot, the Lua loader writes the DLL next to the game and loads it with `package.loadlib`. The DLL finds each target by signature, in non-writable code only, and writes nothing to the exe on disk.

| Patch | Purpose |
|---|---|
| A, B, C | Lobby: allow several players on one faction |
| D | Keep the faction human when a partner leaves |
| E | Reseat every returning player on their saved faction |
| F | End Turn waits for everyone on the faction |
| G | Battle gifts work both ways, recall makes a unit shared, players can take units for themselves |

A patch whose signatures do not match stays off and says why in `archon_native_log.txt`. A game version the mod was not tested on is allowed, at the player's own risk, and the lobby label says so.

| Path | Contents |
|---|---|
| `native/` | The DLL source, and the Lua 5.1 headers it compiles against |
| `src/` | The pack's contents: Lua scripts and one UI layout |
| `tools/` | Build helpers, and the offline signature test |
| `workshop/` | Thumbnail, Steam description, upload steps |

## Building

You need:

- Windows, PowerShell 7 and Python 3.
- Visual Studio 2022 Build Tools with the C++ workload.
- [rpfm_cli](https://github.com/Frodo45127/rpfm) 4.x.

```powershell
pwsh -NoProfile -File build.ps1 -RpfmCli <path to rpfm_cli.exe>
```

The result is in `out/`, and `out/workshop/` is ready to upload. The build uses `/Brepro`, so the same compiler and source produce the same DLL.

The offline signature test checks every patch target against a copy of `Warhammer3.exe`, without running the game. It needs `capstone` for Python, and it expects the tested game build. Pass the copy with `-ExeCopy <path>`. The build never writes into the game install.

## License

[MIT](LICENSE). The Lua 5.1 headers in `native/lua/` keep their own license, in [LICENSE-lua-headers.md](native/lua/LICENSE-lua-headers.md).
