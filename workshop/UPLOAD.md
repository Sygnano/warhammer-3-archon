# Uploading Archon Mode to the Steam Workshop

`build.ps1` fills `out/workshop/`:

| File | Use |
|---|---|
| `archon_mode.pack` | The mod |
| `archon_mode.png` | The thumbnail. Must keep the pack's name |
| `workshop_description.txt` | Steam BBCode, to paste into the item's description |

Suggested title: **Archon Mode - Share One Faction in Multiplayer**

## Upload

CA's newer manager, `twmm`, cannot upload to the Workshop (checked on 0.8.4224151), so this goes through the original launcher. The launcher's exact menus were not checked on this install. If your screens differ, follow them instead.

1. Copy `archon_mode.pack` and `archon_mode.png` into `<game>\data\`.
2. With Steam running, start the game from Steam and choose the original launcher, or open `<game>\launcher\launcher.exe`. Go to its mod manager, and upload `archon_mode`.
3. On the item's Steam page, set visibility to **Hidden** or **Friends only**. Do this first.
4. Paste `workshop_description.txt` into the description, and set the title.

## Before making it public

So far the mod has only been tested from local copies. The Workshop copy loads from a different folder, and a different launcher can change where the DLL is written. Test the real thing:

1. On **both** PCs, remove the local `archon_mode.pack` and `archon_mode.png` from `<game>\data\`, otherwise the local copy can hide a broken upload.
2. Subscribe on both PCs. The second account needs to be able to see the item, so use Friends only or add it as a contributor.
3. Check that both PCs show the same version on the lobby label.
4. Play one session: same faction, a real-time battle with a gift, an end turn, a save, a reload.
5. Repeat it starting the game from `twmm` if you normally use the old launcher, or the other way round. In `twmm` the DLL and logs may land in `<game>\twmm\`.
6. Test once on a Windows setup with default antivirus, if you can. The DLL is unsigned.

Then set visibility to **Public**.

## Updating later

1. Change `$version` in `build.ps1`, then match it in `archon_native.c`, the boot loader's `BIN_PATH` and `DLL_NAME`, and the frontend and campaign `VERSION`. The build stops if the first two disagree.
2. Run the build and repeat the upload for the same item.
3. Both players must update together. A mismatch shows a warning on each PC when the campaign starts.
