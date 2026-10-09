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
| `max_angle_deg` | 42.47 | fully open |
| `rule_angle_deg` | 32.47 | where the slot reaches 50 mm |
| `slot_gap_closed_mm` / `_open_mm` | 11.4 / 59.6 | main plane to flap, in side view |

The node's translation is the pivot, so the flap's vertices are relative to the hinge (raylib bakes
the translation in, so `LoadModel` gives the flap closed, in the body frame). To open it by
`open` in 0..1: turn about `axis` through `pivot` by `open * max_angle_deg`.
`CarRender::setDrsOpen(open)` does this in viewer v2.

The angle starts from the 2013 rule (Technical Regulations 3.18.3: a slot gap of 10 to 15 mm
closed, no more than 50 mm open). This model's flap has a chord of only 100 mm, so a 50 mm slot
takes about 32 degrees; the flap opens 10 degrees past that (`EXTRA_OPEN_DEG` in the tool),
which looked right on screen.

`drs_flap.glb` also has a glTF animation, `drs_open`, whose time is the open amount (0 s closed,
1 s open), for tools that play glTF animations.

## Making a livery

The car's paint is mapped as a blueprint: six orthographic views (left, right, top, bottom, front,
rear) on one 2048 x 2048 sheet at the same scale, 3.2 mm a texel. Each part of the car sits in the
view it faces most, so a flat decal painted on the sheet lands on the car the way it looks in that
view. Nothing is baked in but the colours: sponsor logos are just paint.

| file | what it is |
|---|---|
| `livery_default.png` | the car's original Mercedes paint on this layout: start from this |
| `livery_template.png` | transparent: panel frames and names, a 0.5 m grid, the wheel circles, the centre line; keep it on a layer above the painting and hide it when you save |
| `livery_guide.png` | the same on a solid colour per view; save a copy as `livery.png` to see where each view lands on the car |
| `livery_views.json` | where each view's panel is on the sheet, in texels, and its size in metres |
| `livery.png` | **your livery**, if you make one (not in the repo) |

1. Open `livery_default.png` (or start blank) in any paint program, 2048 x 2048, and put
   `livery_template.png` on a layer above it.
2. Paint. The side views are turned upright (nose at the top, the car's top towards the right); the
   panel's name says how each one is turned. Paint a little past the edge of the car's silhouette,
   because the sheet is filtered and anything right at the edge picks up its neighbour.
3. Hide the template layer and save as `assets/cars/f1_2013_02/livery.png`, opaque, same size.
   Viewer v2 loads it in place of the default the next time it starts. Delete it to go back.

Where two surfaces overlap in a view (a sidepod's outer face over the floor's edge, say), they share
paint: paint what shows from that side and the hidden one comes along unseen. Surfaces that bend
away from a view stretch (up to about 1.7x); that is where the colour on `livery_guide.png` changes
over to the next view.

`tools/unwrap_livery.py` makes all of this (after `tools/rig_drs.py`): it re-maps the body's paint
material and the DRS flap, rebakes the old paint, and points `body.glb` and `drs_flap.glb` at
`livery_default.png`. `--size 4096` gives 1.6 mm a texel. The same files are in each of the other ten
cars of the 2013 pack (`D:\RR2 assets\f1_2013_split`).

`liveries/solid_*.png`: ten plain single-colour liveries (red, orange, yellow, green, teal, blue, navy,
purple, white, black). Copy one to `livery.png` to use it.
