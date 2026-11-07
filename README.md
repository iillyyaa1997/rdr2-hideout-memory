# Hideout Memory for Bandit Hideouts (Red Dead Redemption 2)

An ASI add-on for [Bandit Hideouts](https://www.nexusmods.com/reddeadredemption2/mods/2213). Hideout skulls stay off the map until you have been near the hideout, then they stay on the map for good, the way the map fills in as you explore. A cleared hideout still disappears until it respawns, as Bandit Hideouts does it.

## Requirements

- RDR2 1.0.1491.50 (Steam), Story Mode
- [ScriptHookRDR2 V2](https://www.nexusmods.com/reddeadredemption2/mods/1472) and an ASI loader
- Bandit Hideouts with `ALWAYS_SHOW_HIDEOUT_BLIP=1` in `Bandit_Hideouts.ini` (Bandit Hideouts then creates a skull for every hideout; Hideout Memory hides the ones you have not visited)

## Install

Copy `HideoutMemory.asi` and `HideoutMemory.ini` into the game folder. With Vortex, set the mod type that deploys into the game folder.

## Settings (`HideoutMemory.ini`)

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | |
| `Radius` | 150 | metres: how close you have to come for a hideout to count as visited |

Visited hideouts are kept in `<game>\HideoutMemory.dat` (one `x y z` per line). Delete it to start over. `HideoutMemory.log` shows what happened.

## How it works

Bandit Hideouts calls natives through three exports of `ScriptHookRDR2.dll`. Hideout Memory rewrites those import-table entries of `Bandit_Hideouts.asi` only, watches `BLIP_ADD_FOR_RADIUS` / `SET_BLIP_SPRITE` and toggles `BLIP_MODIFIER_HIDDEN` on the skulls. Every call is passed on to whatever the entry held before, so it works next to other plugins that hook the same mod, such as [RuText](https://github.com/iillyyaa1997/rdr2-rutext): it patches a few seconds after the game starts, after them, and checks its entries every tick.

## Build

`./build.sh` cross-compiles `out/HideoutMemory.asi` with mingw-w64 in a throwaway Docker container.

## License

MIT, see [LICENSE](LICENSE).

---

## По-русски

Дополнение к Bandit Hideouts: значок логова появляется на карте, когда вы подъехали к нему ближе 150 м, и остаётся навсегда. Зачищенное логово Bandit Hideouts сам убирает, пока оно не заселится снова. Нужен `ALWAYS_SHOW_HIDEOUT_BLIP=1` в `Bandit_Hideouts.ini`. `HideoutMemory.asi` и `HideoutMemory.ini` — в папку игры.
