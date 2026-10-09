# f1_2013_02

The 2013 Mercedes from Dave Love's 2013 F1 pack (CC BY 4.0, see CREDITS.md), drawn by viewer v2
(`apps/viewer2/car_render.cpp`). Metres, +Y up, +Z forward, +X = car's left, origin on the ground
at the centre of the wheelbase. Wheel hubs and the livery material are in `car.json`.

## DRS flap

The rear wing's upper element is its own file, `drs_flap.glb`, cut out of `body.glb` by
`tools/rig_drs.py`. Its `"drs"` block in `car.json`:

| key | value | |
|---|---|---|
| `node` | `drs_flap` | the only node in `drs_flap.glb` |
| `pivot` | (-0.0282, 1.0038, -2.100) | the hinge, at the flap's trailing edge (top rear), body frame |
| `axis` | (-1, 0, 0) | right-handed: a positive turn lifts the leading edge |
| `max_angle_deg` | 32.47 | fully open |
| `slot_gap_closed_mm` / `_open_mm` | 11.4 / 50.0 | main plane to flap, in side view |

The node's translation is the pivot, so the flap's vertices are relative to the hinge (raylib bakes
the translation in, so `LoadModel` gives the flap closed, in the body frame). To open it by
`open` in 0..1: turn about `axis` through `pivot` by `open * max_angle_deg`.
`CarRender::setDrsOpen(open)` does this in viewer v2.

The angle is set by the 2013 rule (Technical Regulations 3.18.3: a slot gap of 10 to 15 mm closed,
no more than 50 mm open). This model's flap has a chord of only 100 mm, so a 50 mm slot takes
about 32 degrees.

`drs_flap.glb` also has a glTF animation, `drs_open`, whose time is the open amount (0 s closed,
1 s open), for tools that play glTF animations.
