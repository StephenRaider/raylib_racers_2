# Submitting a team to the General Championship

Send **one zip** named after your team's short name in lowercase, for example `mec.zip`
(letters, digits, `_` and `-` only). Do not show it to other teams.

## What goes in the zip

```
team.json
livery_1.png            2048 x 2048, car 1
livery_2.png            2048 x 2048, car 2
bots/
  first/                C or C++ source of the bot for car 1
  second/               the bot for car 2 (or point both drivers at the same folder)
```

You can also zip the folder itself (`mec/team.json`, ...): both work.

## team.json

```json
{
  "gc_version": 1,
  "team": "Mechanical Engineering",
  "short": "MEC",
  "model": "ferrari",
  "stats": "top_speed=7,downforce=6,tire_management=2",
  "drivers": [
    { "number": 7,  "livery": "livery_1.png", "bot": "bots/first" },
    { "number": 77, "livery": "livery_2.png", "bot": "bots/second" }
  ]
}
```

| field | rule |
|---|---|
| `team` | your team's name, up to 40 characters, unique |
| `short` | 2 to 4 capital letters or digits, unique. The drivers are shown as `MEC 1` and `MEC 2` |
| `model` | one of `sauber mercedes redbull forceindia lotus williams mclaren ferrari tororosso caterham marussia` |
| `stats` | your preferred team stats as `key=points,...`; leave a stat out for 5 (the stock car). Each is 0 to 10 and the total over all stats may not go over the budget (40 with 8 stats, more if the event uses 10); you may spend less. Leave `""` for all stock |
| `drivers` | exactly two. `number`: 1 to 99, unique across the whole event, and painted on your livery yourself. `livery`: a PNG in the zip. `bot`: a folder in the zip |

The stats you can set: `tire_management top_speed acceleration downforce handling pit_stop fuel_efficiency brakes`
(the host's rules file is the final word; the build prints a clear message if a key is wrong).

## Liveries

Paint on the template of the **model you picked**: every car has its own layout, so a sheet for one car does
not fit another. Templates (`<model>_template.png` on a top layer, `_guide.png`, `_default.png` to start
from) come with this page. Save an opaque 2048 x 2048 PNG. For "Mercedes in any colour" use `mercedes` and
paint it a plain colour.

## Bots

Plain C or C++ source against `rr/robot_api.h` (see `docs/ROBOTS.md`). The example bots show how
(`bots/simple/simple.c`). Include only your own files, the standard library, `rr/robot_api.h` and the
helpers as `#include "../common/rr_safety.h"` (also `rr_params.h`, `rr_awareness.h`, `rr_recovery.h`).
No compiled files, threads, files or sockets (the sandbox blocks them and a car that tries is retired).
Keep `drive()` under 2 ms. Each driver runs the bot separately, so keep state in the pointer you return
from `create()`, not in globals.

## Check before you send

With the repository, put your folder in `GC/teams/<name>/` and run `GC/rr_gc check`; it lists what is wrong,
or run `Build_GC` for the full check including a sandboxed test lap.
