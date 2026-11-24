# Menu Button Layout

The **Menu Button Layout** setting (`menu_button_layout` in `sotb.ini`: `western` or `original`) chooses the
Confirm/Back layout of the game's menus. It is read once, before the guest starts, and applies at the next game
start. Its default is `western`.

| Setting | Menus |
| --- | --- |
| Western (`western`) | Cross/A confirms, Circle/B goes back. Default of the port for PC and Xbox controllers. |
| Original (`original`) | Circle/B confirms, Cross/A goes back. Restores the original Korean release (`KR_BD`) layout. |

The supported build is `CUSA03762-1.00`, which is the Korean release (`SKU=KR_BD`). Its original behavior is
Circle/B to confirm and Cross/A to go back; this is intentional for that SKU, not a bug in the original game.
**Menu Button Layout** is a navigation option provided by the port, defaulting to **Western** for a natural
convention on PC and Xbox controllers.

The setting is independent of **Controller prompts**:

- Xbox + Western: A confirms, B goes back.
- Xbox + Original: B confirms, A goes back.
- PlayStation + Western: Cross confirms, Circle goes back.
- PlayStation + Original: Circle confirms, Cross goes back.

The prompt art is chosen from the key that the game actually binds to its menu actions, so the glyph always follows
the action.

Combat controls never change: Jump, attacks, dodge, abilities, shortcuts and every other gameplay binding keep
their keys. Controller rebinding and keyboard bindings are not touched either.

## How it works (CUSA03762-1.00)

The game detects its region and registers `MenuSelect` and `MenuBack` in the input settings. One branch registers
Cross as Select and Circle as Back; the other registers the same two keys swapped, giving Circle as Select and
Cross as Back. The Korean `KR_BD` build takes the Circle-confirms registration.

`src/sotb_menu_buttons.c` verifies `CUSA03762-1.00` before writing anything: the site at `0x1659c4`, the call to
the region lookup, the reference to `MenuSelect`, the conditional branch, and the two pairs of key registrations
in the branches. It also requires the site to appear exactly once. With **Western**, the conditional branch becomes
an unconditional jump to the Western registration, so every reader of `MenuSelect` and `MenuBack` uses the Western
layout and the prompt resolver follows it. With **Original**, nothing is written and the original `KR_BD` image
behavior is preserved.

Only `CUSA03762-1.00` is supported. Another build or a different executable is not patched, nothing is written, and
the log reports why:

    Menu button layout: Western requested but unavailable for CUSA03762-1.00: the menu layout code does not match

The successful startup line is one of:

    Menu button layout: Western, Cross/A confirms and Circle/B cancels
    Menu button layout: Original, Circle/B confirms and Cross/A cancels

`SOTB_MENU_BUTTON_LAYOUT=western` or `SOTB_MENU_BUTTON_LAYOUT=original` overrides the saved setting for one run.

The port's own system-dialog overlays (the save list and the text-entry box) are host UI, not the game's menu
state; they keep Cross/A=OK and Circle/B=cancel in both layouts.
