# Menu Button Layout

The **Menu Button Layout** setting (`menu_button_layout` in `sotb.ini`: `western` or `original`) chooses the
Confirm/Back layout of the game's menus. It is read once, before the guest starts, and applies at the next game
start. Its default is `western`.

| Setting | Menus |
| --- | --- |
| Western (`western`) | Cross/A confirms, Circle/B goes back on every supported build. Default of the port for PC and Xbox controllers. |
| Original (`original`) | Keeps each build's native layout: Circle/B confirms on `CUSA03762` (Korea `KR_BD`), Cross/A confirms on `CUSA02871` (USA) and `CUSA00951` (Europe). |

The supported builds are `CUSA03762-1.00` (Korea, `SKU=KR_BD`), `CUSA02871-1.00` (USA, `SKU=US`) and `CUSA00951-1.00`
(Europe, `SKU=EU`). Korea's original behavior is Circle/B to confirm and Cross/A to go back; USA and Europe already use
Cross/A to confirm and Circle/B to go back natively. **Menu Button Layout** defaults to **Western** so every build uses
a consistent PC/Xbox convention while **Original** leaves the executable untouched to preserve each region's native
behavior.

The setting is independent of **Controller prompts**:

- Xbox + Western: A confirms, B goes back.
- Xbox + Original: B confirms, A goes back.
- PlayStation + Western: Cross confirms, Circle goes back.
- PlayStation + Original: Circle confirms, Cross goes back.

The prompt art is chosen from the key that the game actually binds to its menu actions, so the glyph always follows
the action.

Combat controls never change: Jump, attacks, dodge, abilities, shortcuts and every other gameplay binding keep
their keys. Controller rebinding and keyboard bindings are not touched either.

## How it works

The game detects its region (`-SKU=...` in `ue4commandline.txt`) and registers `MenuSelect` and `MenuBack` in the input
settings. One branch registers Cross as Select and Circle as Back (`SKU::EU`, `SKU::US`); the other registers the same
two keys swapped (`SKU::JP`, `SKU::AS`, `SKU::KR`, `SKU::KR_BD`).

`src/sotb_menu_buttons.c` verifies the build key (`CUSA03762-1.00`, `CUSA02871-1.00` or `CUSA00951-1.00`) before writing
anything: the site at `0x1659c4`, the call to the region lookup, the reference to `MenuSelect`, the conditional branch,
and the two pairs of key registrations in the branches. It also requires the site to appear exactly once. With
**Western**, the conditional branch becomes an unconditional jump to the Western registration. With **Original**, nothing
is written and each build's native SKU behavior is preserved.

An unsupported build or altered executable is not patched, nothing is written, and the log reports why:

    Menu button layout: Western requested but unavailable for CUSA03762-1.00: the menu layout code does not match

The successful startup line is one of:

    Menu button layout: Western, Cross/A confirms and Circle/B cancels
    Menu button layout: Original, Circle/B confirms and Cross/A cancels

`SOTB_MENU_BUTTON_LAYOUT=western` or `SOTB_MENU_BUTTON_LAYOUT=original` overrides the saved setting for one run.

The port's own system-dialog overlays (the save list and the text-entry box) are host UI, not the game's menu
state; they keep Cross/A=OK and Circle/B=cancel in both layouts.
