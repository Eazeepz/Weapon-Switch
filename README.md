# Weapon Switch

Quick Weapon Switching for Saints Row 1! Keybind, scroll wheel, DPad. Weapon wheel no longer locks movement

Works with keyboard and mouse or a controller, follows your in-game controller layout, and every key and button can be changed. Single player only.

## Install

1. Download `WeaponSwitch-2.0`
2. Install into Saints Reborn's `dist\mods` folder, so you have `dist\mods\WeaponSwitch\`
3. Start `WhompaysModLoader.exe` and make sure **Weapon Switch** is enabled.

To uninstall, delete the `mods\WeaponSwitch` folder. The game's own files are
never changed.

## Controls

### Keyboard and mouse

| Input | What it does |
|---|---|
| `1` | Fists |
| `2` | Melee |
| `3` | Pistols |
| `4` | SMGs |
| `5` | Shotguns |
| `6` | Rifles |
| `7` | Special (rocket launchers) |
| `8` | Thrown (grenades, molotovs) |
| Mouse wheel | Next / previous weapon |
| Arrow keys `→` / `←` | Next / previous weapon |
| Tap `Q` | Next weapon |
| Hold `Q` | Weapon wheel: select weapon with mouse |

The number pad works too (`NUMPAD1` – `NUMPAD8`).

### Controller

| Input | What it does |
|---|---|
| D-pad right / left | Next / previous weapon (on foot) |
| Tap `B` | Next weapon (on foot) |
| Hold `B` | Weapon wheel: point with the right stick, keep moving with the left stick |

If you picked the third controller layout in the game's Options, the weapon
wheel button is `RB` instead of `B`. The mod follows whichever layout you use.

### Switching order

Fists, then melee, pistols, SMGs, shotguns, rifles, special and thrown, wrapping
back round to fists. Only weapons you carry are visited.

### In vehicles

In a vehicle the D-pad and the weapon wheel button do what the game normally
does (the radio, the drive-by wheel). The number keys and mouse wheel still
switch weapons. With the wheel open you keep driving.

## Changing the controls

Open `mods\WeaponSwitch\mod.ini` in Notepad, change a line, save, and restart
the game. Each setting has a comment above it.

### Writing keys and buttons

| | Names you can use |
|---|---|
| Keys | letters, digits, `F1`–`F24`, `NUMPAD0`–`NUMPAD9`, `SPACE`, `TAB`, `SHIFT`, `CTRL`, `ALT`, `ENTER`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `MOUSE4`, `MOUSE5`, `MMB` |
| Controller buttons | `A` `B` `X` `Y` `LB` `RB` `LS` `RS` `START` `BACK` `DPAD_UP` `DPAD_DOWN` `DPAD_LEFT` `DPAD_RIGHT` |

- Several keys for one thing: separate them with commas, e.g. `key_3 = 3, Q`.
- Turn something off: leave it empty, e.g. `key_5 =`.
- Upper or lower case doesn't matter.

### All settings

| Setting | Default | What it does |
|---|---|---|
| `key_1` … `key_8` | `1, NUMPAD1` … | Keys for each weapon category (see the table above) |
| `wheel` | `true` | Mouse wheel switches weapons |
| `wheel_dir` | `-1` | `-1`: scroll down for the next weapon. `1`: scroll up |
| `next_key` / `prev_key` | *(empty)* | Extra keys for next / previous weapon |
| `pad_next` / `pad_prev` | `DPAD_RIGHT` / `DPAD_LEFT` | Controller buttons for next / previous weapon, on foot |
| `wheel_key` | `Q` | Keyboard weapon wheel key |
| `pad_wheel` | `auto` | Controller weapon wheel button. `auto` follows your controller layout |
| `tap_switch` | `true` | Tapping the weapon wheel button gives the next weapon |
| `hold_ms` | `200` | How long to hold before the wheel opens, in milliseconds |
| `move_with_wheel` | `true` | Keep moving while the wheel is open. `false` = the game stops, as normal |
| `debug_key` | `F8` | Lists your weapons in `mods\wml.log` |

### Examples

```ini
; Side mouse buttons for next / previous
next_key = MOUSE5
prev_key = MOUSE4

; Bumpers instead of the D-pad on a controller
pad_next = RB
pad_prev = LB

; Pistols on 3 and E, no shotgun key
key_3 = 3, E
key_5 =

; The weapon wheel opens straight away, no tap switching
tap_switch = false
```

## Good to know

- **The wheel opens a little later.** The mod waits `hold_ms` (0.2 s) to tell a
  tap from a hold. Lower `hold_ms`, or set `tap_switch = false` to open it
  instantly.

- **D-pad left / right are taunts in the game.** With the defaults they switch
  weapons on foot instead. Set `pad_next =` and `pad_prev =` to get the taunts
  back.
- **A weapon gets skipped?** Press `F8`, then open `mods\wml.log`. Weapons the mod
  doesn't know are marked there. Please open an issue with the name.
- **Something isn't working?** `mods\wml.log` shows what the mod set up when the
  game started, including the keys it read from `mod.ini`. A typo shows up there
  as `off`.

## Building from source

You need [CMake](https://cmake.org/download/) and Visual Studio 2022 or newer with
**Desktop development with C++**. Then run:

```bat
build.bat
```

It downloads [MinHook](https://github.com/TsudaKageyu/minhook) on the first run,
builds the DLL, copies `WeaponSwitch.dll` into the `WeaponSwitch` folder and
makes the release zip.
