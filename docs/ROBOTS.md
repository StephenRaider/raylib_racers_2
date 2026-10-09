# Writing a robot

A robot is a shared library that drives one or more cars. The whole contract is
one C header, [`include/rr/robot_api.h`](../include/rr/robot_api.h), so a robot
can be written in C, C++, or anything else that can export a C function.

The design follows TORCS: the host owns the physics, calls your `drive()` at a
fixed rate (50 Hz by default) with a sensor snapshot, and applies the controls
you return until the next call.

## The smallest robot

```c
#include "rr/robot_api.h"
#include <stdlib.h>

static void* create(const RRTrackInfo* track, const RRCarSpec* car, int index,
                    const char* params, RRRobotConfig* config) {
    return calloc(1, 1);  /* your per-car state */
}

static void drive(void* self, const RRSensors* in, RRControl* out) {
    out->steer = 3.0f * (-in->angle - 0.5f * in->track_pos);  /* line up with the track, stay central */
    out->accel = in->speed_x < 25 ? 0.5f : 0.0f;  /* gently: full throttle in 1st gear spins the car */
}

static void destroy(void* self) { free(self); }

static const RRRobotApi api = {RR_ABI_VERSION, "tiny", "me", create, drive, destroy, NULL, NULL};
RR_EXPORT const RRRobotApi* rr_robot_entry(void) { return &api; }
```

Build it and race it:

```sh
cc -O2 -shared -fPIC -I path/to/raylib-racers/include tiny.c -o tiny.so
./rr_race --car ./tiny.so --car racingline
```

Or put it in the project as `bots/tiny/tiny.c` and rebuild. Every folder under
`bots/` (except `common/`) that holds `.c`/`.cpp` files is built as a robot named
after the folder, with no CMake edits; it lands in `build/bots/` and can be
named as `--car tiny`. Re-run the build after adding the folder (CMake
re-scans `bots/` by itself).

## Lifecycle

| Call | When | Notes |
|---|---|---|
| `rr_robot_entry()` | library load | return a static `RRRobotApi`; set `abi_version` to `RR_ABI_VERSION` (ABI 9; the host also loads robots built for ABI 2 and later) |
| `create(track, car, index, params, config)` | once per car | return your state, or `NULL` to refuse. Plan here: you get the full track geometry and car spec. |
| `drive(self, sensors, control)` | every 1/robot-hz s | `control` arrives zeroed except `gear`. Fill it in. |
| `destroy(self)` | end of race | free your state |
| `debug_path(self, xy, max)` | viewer frames (optional) | write up to `max` (x, y) points; the viewer draws them in the car's colour |
| `session_end(self, summary)` | end of the session, before `destroy` (optional, ABI 8) | how the session went (`RRSessionSummary`: laps, best lap, lap times, tyres, fuel); the last chance to write notes to the weekend memory |

The same library can drive several cars in one race (`--car racingline --car
racingline`), so keep state in the pointer you return from `create()`, not in
globals.

## Race weekends (ABI 8)

A weekend is three sessions, and every car gets a fresh robot (`create()` to
`destroy()`) for each one:

| Session | `config->session` | What happens |
|---|---|---|
| Practice | `RR_SESSION_PRACTICE` | alone on track, up to `config->session_laps` laps (15 by default). Any tyres, pits open, no tyre-set limit: try compounds, measure wear and fuel |
| Qualifying | `RR_SESSION_QUALIFYING` | alone: an out lap and two flying laps. The best lap sets the grid |
| Race | `RR_SESSION_RACE` | everyone together |

Testing mode runs with `RR_SESSION_TEST`. `sensors->session` repeats the
session while driving.

**Weekend memory.** `config->memory` points at `config->memory_size`
(`RR_SESSION_MEMORY`, 256 KiB) bytes that belong to your car for the whole
weekend: zeroed before practice, the same bytes in qualifying and the race,
thrown away afterwards. It is the only thing that carries from one session to
the next (robots may not keep files), so put your notes there: braking points,
grip per corner, how many laps a set of softs lasted. The pointer stays valid
until `destroy()`. A quick race with no weekend gets a fresh, zeroed memory.

```c
typedef struct { unsigned magic; int practice_laps; float soft_wear_per_lap; } Notes;
Notes* notes = (Notes*)config->memory;
if (notes->magic != 0x4E4F5445) { memset(notes, 0, sizeof *notes); notes->magic = 0x4E4F5445; }
```

**Turns.** `track->turns` lists the corners (`track->num_turns` of them),
numbered from the start line like a real circuit's: `RRTurn` gives each one's
direction, start, apex and end distance, tightest radius and the angle it turns
through. They come from the track's shape, so the ids are the same in every
session of the weekend, which makes them good keys for per-corner notes. While
driving, `sensors->turn` is the turn you are in (0 on a straight),
`sensors->next_turn` the next one ahead and `sensors->next_turn_ds` the
distance to its start.

## Competition rules

Contests run robots with `--sandbox --cpu-cap MS` (see
[COMPETITION.md](COMPETITION.md)). Then your robot runs in its own process
and cannot open files, sockets or threads, so keep everything in memory and
carry notes between sessions in the weekend memory. Each `drive()` call has a
CPU budget: an answer over it is ignored (the car keeps its last controls), and
more than 50 of those retire the car. Do expensive planning in `create()`.
Anything you `printf` goes to the simulator's stderr.

## Sensors (`RRSensors`)

SCR / TORCS-style sensors:

| Field | Meaning |
|---|---|
| `angle` | car heading minus track direction, rad. Positive: nose points left of the track axis |
| `track_pos` | 0 on the centreline, +1 at the left tarmac edge, -1 at the right. Beyond ±1 you are off the tarmac |
| `track[19]` | range finders to the tarmac edge, m (max 200). Directions come from `config->track_sensor_angles` (degrees, positive left), which you may change in `create()`. All -1 when off the tarmac |
| `opponents[36]` | nearest car in each 10° sector, m (200 if none). Sector `i` covers [-180+10i, -170+10i) degrees; 0° is straight ahead |
| `speed_x`, `speed_y`, `yaw_rate` | body-frame velocity (x forward, y left) and yaw rate |
| `rpm`, `gear`, `wheel_spin` | `wheel_spin` > 0 means the rear tyres are past their grip: use it for traction control |
| `damage` | accumulated collision damage, repaired in the pits. Growing linearly to 8000 it costs up to 35% of the downforce, 12% of the engine power and 8% of the mechanical grip, and adds 10% drag |

Race state: `dist_from_start`, `dist_raced`, `lap`, `race_laps`, `race_pos`,
`num_cars`, `cur_lap_time`, `last_lap_time`, `best_lap_time`.

Ground truth, like a TORCS robot gets: world pose `x`, `y`, `yaw`, the nearest
centreline sample `track_index`, and `on_track`.

Consumables and pit:

| Field | Meaning |
|---|---|
| `fuel` | litres left. Fuel weighs `fuel_density` kg/l; at 0 the engine stops (5 s stopped with an empty tank is a DNF) |
| `tire_wear[2]` | front, rear: 0 new .. 1 worn out |
| `tire_grip` | grip multiplier from compound and wear (both axles averaged), 1 = new medium. Leaves out temperature: see `axle_grip` |
| `tire_compound`, `laps_on_tires` | `RR_TIRE_SOFT` / `MEDIUM` / `HARD`, laps since they were fitted |
| `pit_state` | `RR_PIT_NONE`, `RR_PIT_LANE` (speed limited), `RR_PIT_SERVICE` (in the box, controls ignored), `RR_PIT_DONE` (serviced, still in the lane) |
| `pit_stops`, `pit_box_s`, `service_time_left` | completed stops, where this car's box is (track `s`), time left while serviced |

What the tyres are doing (ABI 3), the signals a driver feels through the seat:

| Field | Meaning |
|---|---|
| `grip_use[2]` | front, rear: force asked of the axle over what it can give, worst wheel. ~0.9-1.0 is the limit; past ~1.1 the axle is sliding. Front high and rear low = understeer; rear high off throttle = oversteer; rear high on throttle = wheelspin |
| `slip_angle[2]` | front, rear slip angles, rad. Rear larger than front = the rear is stepping out |
| `accel_x`, `accel_y` | body-frame acceleration, m/s² (y + = left), as the suspension feels it |
| `wheel_load[4]` | N on each wheel: front left, front right, rear left, rear right |

Tyre temperature and other cars' air (ABI 4):

| Field | Meaning |
|---|---|
| `tire_temp[2]` | front, rear tyre temperature, °C. Tyres leave the warmers at 80 °C (race start and after a stop) |
| `tire_temp_window[2]` | the fitted compound's working window, low and high, °C: soft 85-105, medium 95-115, hard 105-125. Below it the tyre loses 0.25% grip per °C and grains (wear +1.5% per °C); above it 0.2% per °C and blisters (wear +6% per °C, so 17 °C over doubles the wear) |
| `tire_temp_wheel[4]` | (ABI 7) each tyre's temperature, °C: front left, front right, rear left, rear right. Each tyre's grip follows its own temperature; `tire_temp` is the mean of each axle's two |
| `brake_temp[4]` | (ABI 7) each brake disc's temperature, °C, same order. Braking heats the discs; their heat soaks through the wheel rims into the tyres with a lag of a minute or so, so heavy braking warms the tyres a little over the following laps |
| `brake_temp_window[2]` | (ABI 7) 350-1000 °C. Below it the brakes bite less (75% of full force when stone cold), above it they fade (0.2% per °C, down to 60%) |
| `axle_grip[2]` | front, rear grip multiplier right now: compound x wear x temperature |
| `ambient_temp` | air and track temperature (`--ambient`, default 25 °C) |
| `slipstream` | drag reduction from the car ahead, 0 .. 0.45 (strongest right behind it, gone 60 m back or 3.5 m to the side) |
| `dirty_air` | downforce lost to the car ahead, 0 .. 0.10, more of it at the front (gone 40 m back or 3 m to the side) |

Heat comes from the same sliding work that wears the tyres (cornering slip
in full, braking and traction slip in part) plus rolling under load, and the
airflow takes it away, more at speed. Driving harder or following closely
(dirty air makes the front slide) heats the tyres; lifting or a cleaner line
cools them.

Flags (ABI 3):

| Field | Meaning |
|---|---|
| `blue_flag`, `blue_flag_car`, `blue_flag_ds` | a car that is lapping you is within `RR_BLUE_FLAG_RANGE` (60 m or 1.2 s) behind: let it by. Holding it up within 30 m for more than `RR_BLUE_FLAG_LIMIT` (8 s) costs a `RR_BLUE_FLAG_PENALTY` (5 s) time penalty, added to your race time (once per lapping car) |
| `penalties`, `penalty_time` | time penalties so far and the seconds they add |

Other cars, for racecraft: `nearby[num_nearby]` lists up to 8 cars, nearest
first by track distance. Each `RROpponent` has `ds` (track distance, + ahead),
`lateral` (their offset from the centreline, m), `speed`, their position and
heading in your body frame (`rel_x`, `rel_y`, `rel_yaw`), `race_pos`,
`pit_state` and `laps_ahead` (negative: a backmarker you are lapping).

## Controls (`RRControl`)

| Field | Range | Notes |
|---|---|---|
| `steer` | -1 .. 1 | +1 is full left; multiply by `car->max_steer` for the road-wheel angle |
| `accel`, `brake` | 0 .. 1 | |
| `gear` | -1 .. num_gears | ignored with automatic gears (the default) except `-1`, which selects reverse once the car is nearly stopped. Set `config->auto_gear = 0` in `create()` to shift yourself |
| `status` | 64 chars | shown in the viewer's car panel |
| `debug[8]` | floats | written to the telemetry CSV as `d0..d7` |
| `pit_request` | 0 / 1 | keep it set while driving to your box |
| `pit_fuel` | litres | fuel to add, clamped to the tank |
| `pit_tires` | 0 or `RR_TIRE_*` | 0 keeps the tyres |
| `pit_repair` | 0 / 1 | repair all damage |
| `pit_window[2]` | laps | optional (ABI 5): earliest and latest lap of your next planned stop, 0 = none. The viewer shows it |
| `pit_plan_tires` | 0 or `RR_TIRE_*` | optional (ABI 5): the compound you plan to fit then |

## Fuel, tyres and pit stops

The physics is in `src/core/car.cpp`; the numbers a strategy needs are:

- **The car** is modelled on a 2004-2010 F1 car: 605 kg with the driver, a
  19,000 rpm V10 of about 660 kW, 7 gears, roughly 2.5x its weight in downforce
  at 300 km/h. A good lap of the circuit is about 53 s, ~305 km/h at the end of
  the straight, ~4.4 g in fast corners.
- **Fuel** burns in proportion to engine work: about 2.3 l per lap of the
  circuit at racing speed. The tank holds 65 l on every track, which is about
  28 laps of the circuit and fewer on longer tracks. A full tank adds 49 kg. `--fuel-rate X` scales consumption.
- **Tyres** wear in proportion to sliding work (cornering, braking, wheelspin).
  A medium loses about 0.02-0.03 per lap of the circuit, and a worn tyre slides
  more, so the rate grows through a stint. Grip falls 7% from new to wear 0.7,
  then off a cliff (-0.8 per unit of wear beyond 0.7). Softs grip 5% more and
  wear 2x faster; hards grip 3.5% less and wear 0.72x. Temperature outside
  the compound's window costs grip and adds wear (see ABI 4 above).
  `--wear-rate X` scales wear (handy for forcing stops in short races).
- **Slipstream**: a car up to 60 m behind another and within 3.5 m of its
  line has up to 45% less drag, so a faster car can close up on a straight
  and pull out to pass.

Starting fuel and tyres are set in `create()` through `config->initial_fuel`
and `config->tire_compound`. When the team has picked the starting tyres
(`--tires`, or the viewer's Grid page) `config->starting_compound_set` is 1,
`tire_compound` already holds that choice and changing it has no effect: plan
the fuel around it. `config` also tells you the race (ABI 5): `race_laps`,
`two_compound_rule`, the `fuel_rate` and `wear_rate` multipliers and
`ambient_temp`. In testing sessions (ABI 6) `pits_closed` is 1 (also in the
sensors): pit requests are ignored, so plan to run to the flag; and when the
team has chosen the starting fuel (`--fuel`, the viewer's Testing setup)
`starting_fuel_set` is 1 and `initial_fuel` already holds it.

**Testing an algorithm.** The viewer's Testing session runs one car alone and
graphs everything (see the README). The same recording works without a window:

```sh
./rr_race --car myrobot --params "grip=0.8" --laps 10 --no-pits --fuel 25 \
          --tires medium --quiet --test-log test_runs
```

saves `test_runs/run_NNNN/summary.json` (lap and sector times, fuel and wear
per lap, tyre temperatures, top and minimum speed, throttle and brake use,
and the moments worth a look: off track, contact, oversteer, understeer,
wheelspin, spin, stopped, with the lap and distance) and `telemetry.csv` (the
car 25 times a second). `test_runs/runs.json` collects every run's setup and
times, so a script or an AI tool can compare parameter sweeps.

**The pit lane.** `track->pit` describes it: `side` (+1 left), the stretch of
track it runs along (`entry_s` -> `lane_start_s` -> `lane_end_s` -> `exit_s`), the
lateral centre of the fast lane (`lane_offset`) and of the boxes (`box_offset`),
and the `speed_limit`. Between `lane_start_s` and `lane_end_s` a wall separates
the lane from the track, so you must have moved across before `lane_start_s`.

A stop, step by step:

1. Before `entry_s`, decide to stop and steer off the racing line towards
   `lane_offset`, braking to the limit by `lane_start_s`.
2. In the lane (`pit_state == RR_PIT_LANE`) the host's limiter cuts throttle
   above `speed_limit`. Set `pit_request` and the order fields.
3. Move to `box_offset` and stop within about 2.5 m of `pit_box_s`. The crew
   starts: `pit_state` becomes `RR_PIT_SERVICE` and the car is held still for
   `RR_PIT_SERVICE_BASE` (2 s) + max(fuel / `RR_PIT_FUEL_RATE` (2.5 l/s),
   `RR_PIT_TIRE_CHANGE` (3.5 s) if changing tyres) + `RR_PIT_REPAIR_PER_1000`
   (1 s) per 1000 damage when repairing, all times the car's
   `pit_service_scale`.
4. When `pit_state` turns to `RR_PIT_DONE`, drive back to `lane_offset`, then
   rejoin the track after `lane_end_s`.

**The timing screen and race rules (ABI 5).** Every team sees the same
timing screen: `timing[num_timing]`, in race order, one `RRTimingEntry` per
car with its position, laps, `gap` to you in seconds (+ ahead, laps
included), last and best laps, `pit_state`, `pit_stops`, the compound fitted,
its age (`laps_on_tires`) and the compounds it has used. Track positions and
other cars' wear and fuel are not on it. `two_compound_rule` says whether the
race requires two different compounds (by default races over 20 laps;
`--two-compounds on|off|auto`); finishing without them costs
`RR_TWO_COMPOUND_PENALTY` (30 s). `compounds_used` is your own mask.

With that a robot can be its own strategist: compare the cost of each number
of stops, place a stop where the car rejoins in clear air (another car's gap
minus your pit loss), stop a lap early to undercut the car ahead, stay out
when it has stopped (overcut), or answer a rival's stop.

`bots/racingline` does all of this (`bots/racingline/strategist.hpp`):

- **Model.** A lap costs the reference lap, plus the compound's pace, plus
  the grip lost to wear, plus 0.023 s per kg of fuel. A stop costs the
  measured pit lane loss plus the service time plus 4 s for the risk of
  traffic and contact on the way in and out, so an extra stop has to earn it.
  Compound priors depend on how hot the car runs its tyres (`heat`): for a
  hot driver the soft barely gains anything and wears 2.75 times as fast as
  the medium; for a cool one it is 2.5% quicker and wears twice as fast. The
  hard wears 0.72 times as fast and is 1.5-3.5% slower. It starts from
  priors (the planned line's lap time, fuel from the track length with a 6%
  margin, wear from `heat`) and replaces them with what it measures: fuel and
  wear per lap since the last stop, clean lap times, the real time lost in
  the pit lane.
- **Plan.** Once a lap, a few hundred metres before the pit entry, it searches
  0 to 3 more stops, the lap of the next one and every compound order, checks
  fuel and tyre life (a planned stint ends by wear 0.7, the current one may
  stretch a little past it) and the two-compound rule, and keeps the fastest.
  The window is every lap for the next stop that costs at most a second more.
- **Racecraft.** Within a few laps of the planned stop it undercuts a car
  less than 1.5 s ahead, covers a car within 3 s behind that has just
  stopped, stays out when the car ahead has just stopped (overcut), and waits a
  lap if it would rejoin less than 1.5 s behind someone.
- **Fuel saving.** When the car is a little short of fuel for the finish
  (less than 13% of what it needs) and the tyres and tyre rule do not need a
  stop anyway, it lifts and coasts into the braking zones instead of making a
  splash stop. Coasting the last 75 m before a braking zone saves about 10%
  fuel for 0.2 s a lap. `save=0..1` forces it (0.3 coasts 75 m, 0.6 150 m).
- **Damage.** It stops to repair only when the time the damage will cost to
  the flag (up to 8% of a lap at 8000 damage) is clearly more than the stop.
- **Must stops.** Not enough fuel to the next pit entry or tyres past the
  cliff force a stop whatever the plan says.
- It publishes the window and the next compound in `pit_window` and
  `pit_plan_tires`; `RL_DEBUG=1` prints its plan each lap and every decision
  to stderr. Its `status` while pitting is the reason for the stop, which the
  race log records with each stop.

It drives a blended path into and out of its box.

## Racecraft

`racingline` also shows a simple approach to wheel-to-wheel racing with the
`nearby` list:

- **Follow**: never close on a car in your path faster than the gap allows.
- **Overtake**: when closing on the car ahead, move to the side that is
  cheapest through the next corners (checked with the speed the track allows
  at that offset), keep the side once alongside, and slow for the tighter or
  wider line.
- **Defend**: when a car on the same lap closes within 25 m behind, cover the
  inside of the next corner with one move, hold it for a few seconds, then
  return to the line.
- **Space**: never steer into a car that is alongside, or across one that is
  closing from behind (`rr_side_limits`). The line, a pass and a defence are
  all clamped to that space; when a pass is squeezed shut the car drops back
  behind instead of forcing it.
- **Aggression**: `attack` (default 1, 0.5 to 2) scales the following gap and
  how early a pass starts; the `spongebob` robot (racingline with other defaults) uses 1.4.
- **Blue flags**: keep to the side away from the lapping car, never attack
  while being lapped, and lift when it is close and the road ahead is not a
  slow corner (`rr_blue_flag_side`).
- **Tyres**: the speed profile assumes tyres in their window and scales with
  `axle_grip` when they are cold or hot; past `heat` °C over the window
  (default 5) it backs off on purpose to cool them.
- **Starts**: it holds its grid lane and eases onto the line over the first
  400 m instead of diving across the field.
- **The limit**: it learns how much grip each 20 m of track really has. A
  stretch where the tyres slid (`grip_use` past 1.1) or the car went off gets
  slower, and so does the braking zone before it; stretches driven well inside
  the limit on clean laps get a little faster each lap (up to `push`). On top
  of that `rr_grip_guard` manages wheelspin and catches oversteer.

Once a car has taken the flag the host takes over gradually: the robot keeps
steering at a reduced pace until the host takes it into the pit lane and parks
it.

Its `pass=0` and `defend=0` parameters switch the behaviours off for
comparison.

## Track and car

`RRTrackInfo` holds the centreline sampled about every metre: position,
direction, distance from the start, half width and signed curvature. That is
enough to plan a racing line (see `bots/racingline`).

Tracks can have hills and banking (ABI 9). `points3` runs parallel to
`points` with the road height `z`, the bank angle (`bank`, + = left edge
higher), the `grade` (dz/ds) and its rate of change `vert_curvature`; all 0 on a
flat track. Distances and curvature stay measured in plan view. The car feels
three things: gravity pulls it back by `g * grade` uphill and pushes it on
downhill (so braking distances change); a banked road holds it into a turn
towards the low side and presses the tyres into the road; and the tyre load
is `g + v² * vert_curvature`, so a compression gives grip and a crest takes it
away, all of it once `v² * -vert_curvature` reaches `g`. `RRSensors` also
gives `z`, `grade` and `bank` under the car. `racingline` folds all three into
its speed plan (`cornerSpeed()`); `rr_stopping_factor()` in `bots/common`
scales braking for the grade underfoot.

`RRCarSpec` gives mass, dimensions, steering lock, tyre friction, aero
coefficients (`drag = drag_coeff * v²`, `downforce = downforce_coeff * v²`),
gearing and brake force, so a planner can estimate cornering and braking limits.
Since ABI 3 it also has the CG height, track widths, aero balance, brake bias,
peak power and the multipliers its team stats give it (tyre wear, fuel
use, pit crew time): cars in one race can differ (see "Car specs and team stats" in the README).

## Parameters and experiments

Everything after `--params` reaches `create()` unchanged. The examples read
`key=value,key=value` with `bots/common/rr_params.h`:

```sh
for g in 0.75 0.8 0.85; do
  ./rr_race --car racingline --params "grip=$g" --laps 3 --quiet --json grip_$g.json
done
```

Races are deterministic: the same command line (and `--seed` when `--noise` is
used) gives the same result, so differences come from your change, not chance.
`--telemetry DIR` writes one CSV per car at the robot rate.

## Helpers in `bots/common`

- `rr_params.h`: read numbers out of the params string.
- `rr_recovery.h`: drop-in "get unstuck" behaviour (U-turn when facing the
  wrong way, reverse out of a barrier). Call `rr_recover()` first in `drive()`.
- `rr_awareness.h`: racecraft and grip helpers all three examples use.
  `rr_side_limits()` narrows the lateral range you may move into around cars
  alongside or closing from behind; `rr_follow_speed()` is a speed cap for not
  running into the car ahead; `rr_blue_flag()` says where to go and how much
  to lift under a blue flag; `rr_grip_guard()` (call it last) is traction
  control plus oversteer and understeer handling from `grip_use` and
  `slip_angle`.

## The example robots

| Robot | Language | Uses | Idea |
|---|---|---|---|
| `simple` | C | SCR sensors + nearby cars | align with the track axis, target speed from the free distance ahead (never pits) |
| `gapfollow` | C++ | SCR sensors + nearby cars | steer towards the longest forward range finder, dodge cars ahead (never pits) |
| `racingline` | C++ | track geometry + pose + nearby cars | minimum-curvature line, friction-limited speed profile re-planned for fuel and tyres and learnt per stretch of track, pure pursuit, pit strategy, overtaking and defending |

All three use `rr_awareness.h` for side awareness, following, blue flags and
the grip guard.
