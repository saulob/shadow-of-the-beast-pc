# Offline local save

The port is an offline title. Nothing reaches the internet, PSN or any Sony or community server. The host stays
physically offline: every socket call fails as unreachable, HTTP and SSL transfers fail, and Matching2, WebAPI, NP Auth,
NP Lookup and friends or presence calls return the signed-out error.

## Portable root

`game\`, `user\` and `logs\` sit side by side next to the executable. On Linux the root is `SOTB_PORTABLE_DIR` when set,
else the AppImage's folder when it is writable, else `$XDG_DATA_HOME/ShadowOfTheBeastPC`.
A development build resolves the same three to the repository's `game\prepared\<build>`, `user` and `build\logs`
(`launcher\paths.cpp`). Nothing the player generates is written under `game\`.

Deleting `user\` gives a first boot: no save, no scores, no TUS data, no trophies.

## Save data

`src/runtime_savedata.c` implements `libSceSaveData` on host directories:

- `user\savedata\<user id>\<title id>\<dir name>\` holds the files the game writes.
- `<dir name>.sce_sys\` holds `param.bin` and `icon0.png`.
- Writes go to a temporary file that is flushed and renamed over the target.
- Directory names that contain `/`, `\` or `..` are rejected.

Campaign progress and settings live in one slot, `BeastSaveGame`, whose single file is `ue4savegame.ps4.sav`. The game
creates and rewrites it; the runtime never fabricates content.

## Why the game asks for a connection

The game's connection flow (`ConnectionNoConnection`, "Unable to establish a network connection") needs two answers
from its online object: network connected (`sceNetCtlGetState` is IP obtained) and signed in (`sceNpGetState` is signed
in). It then runs a sign-in check (NP availability, PS Plus, parental control). With either answer missing it reports
the error and offers TRY AGAIN. Save data, Score and TUS play no part in it.

## Social Features

Shadow of the Beast PC is offline: Social Features are permanently off and nothing of the Live Feed is offered.
`src/guest_patches.c` applies a small set of checked patches to the eboot before it runs (each one is skipped when the
code it replaces differs):

- The save object's `ShadowRealmSocialSetting` is Off and `bShadowRealmSocialInitialPopupShown` is true, for a new
  save (defaults) and for an existing one (normalized whenever the save object is read or written, so an old
  Social Features = On is loaded and saved as Off). Nothing else in the save is touched.
- The first-run Social Features popup (screen 37) is not opened by the map or save-flow screens, and the show-screen
  entry refuses screen 37 and the Live Feed (screen 14). The Sign In screen is not blocked.
- Login never switches Social Features on, and the popup and Options code no longer write the setting.
- The Options screen takes the Social Features button out of its navigation chain (Audio goes straight to Tutorials)
  and collapses it, using the screen's own navigation routine.

With the setting off, the map hides its Live Feed hint, and the Live Feed action requires the setting. Game assets are
not modified.

## Targeted local compatibility

`src/runtime_np_local.c` answers those checks locally, only for the call sites the game uses for them
(`local_gate_allows`, matched on the guest return address):

| Call | Local answer |
| --- | --- |
| `sceNetCtlGetState` from the online object's state check | IP obtained |
| `sceNpGetState` from the same check | signed in |
| `sceNpRegisterStateCallback` / `sceNpCheckCallback` from the Toolkit's callback pump | the callback is told once that the local user is signed in, with the local NP id |
| `sceNpCheckNpAvailability`, `sceNpCheckPlus`, `sceNpGetParentalControlInfo` from the sign-in check | available, authorized, adult with no restrictions |
| `sceNpGetNpId` from the Toolkit's user lookup | the local NP id |

Every other caller gets the offline answers of `src/runtime_services.c`. This means "local services available", not a
PSN session: no authentication, no ticket, no account, no remote request.

The local identity is user 1 with the UserService name (`Player`, or `SOTB_USER_NAME`) as the online id and the fixed
account id `0x0000000100000001`. It is the same on every boot and is not stored.

## Score

Backed by `<user>\local_services\<title id>\scores.toml` (`src/local_np_store.c`). Boards are the game's own 104 ids
(`LeaderboardsPS4` in its engine config): 0-31 and 96-103 rank higher-is-better (points, blood marks), 32-95 rank
lower-is-better (speed runs).

- `sceNpScoreRecordScoreAsync` keeps the best score per board. A score that does not improve gets the not-best-score
  error, which the game treats as success; the game info blob is stored with the score.
- `sceNpScoreGetRankingByNpIdAsync` returns the local player's score as rank 1 when asked for the local NP id; any
  other id has no data.
- Range and friends tables are empty. No other players are invented.
- Writes are temporary file plus rename. A file that does not parse reads as empty and is kept as `scores.toml.corrupt`
  before the next write.

## TUS

Backed by one file per slot under `<user>\local_services\<title id>\tus\` (`slot_<n>.bin`, size and CRC checked). The
game uses its slot 1 ("Highscores" user cloud file) through `sceNpTusGetMultiSlotDataStatusAsync`,
`sceNpTusGetDataAsync`, `sceNpTusSetDataAsync` and `sceNpTusDeleteMultiSlotDataAsync`. Only the local online id has
data. A slot that fails its checks reads as empty.

## Online gate for local data

The overrides in the table above are needed for local data, not for Social Features: the game accepts a ranking
submission only once its online state machine reports signed in, and that state is what they provide. They stay
limited to the listed callers; Live Feed, friends, presence, Matching2, WebAPI, Auth and commerce keep the offline
answers.

TUS slot 1 (the "Highscores" user cloud file) is personal high-score data and is kept as local game data. Friends data
and title storage are not served.

## Requests

Score and TUS contexts and requests are local handles starting at 0x5000, bounded (96), and shared by both services.
Operations complete when started; `PollAsync` returns the stored result, and an aborted request reports an aborted
result. Deleted or foreign handles are rejected.

## Not implemented

Trophies, the Toolkit's synchronous Score and TUS calls, title small storage (TSS), friends data, and everything
that needs a server stay offline.
