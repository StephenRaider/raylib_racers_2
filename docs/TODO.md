# To do

Physics and environment, in the order agreed:

1. **Separate air and track temperature.** `--ambient` is one number for both today. The track surface runs
   10-25 C above the air in sun. Track temperature should drive tyre heating, tyre grip and the cooling of
   tyres and brakes on the road side; air temperature should drive engine cooling and (later) air density.
2. **Rubber on track (track evolution).** Grip builds up along the racing line as cars lay rubber, and falls
   away off the line. Over a weekend that is about 1 s in real F1. Needs a per-track grip map along s and
   lateral position that cars write to and read from.
3. **Engine temperature model.** Water and oil temperature, cooling from airflow (so following closely and
   a hot day cost something), power loss when overheating. There is no engine temperature in the sim today.
4. **Brake temperature model, checked.** Brake temperatures already exist (carbon discs, 350-1000 C window,
   fade above it, poor bite below it, and heat soaking disc -> rim -> tyre). Review that the brake force
   really follows disc temperature in every case, that the heat path to the tyres is realistic (rim
   temperatures about 100-250 C), and that cooling ducts and speed behave sensibly.
5. **Team stats (development tokens): done for the 2013 car.** Ten stats and 50 points (`specs/development.json`),
   including KERS, DRS and a gearbox with a shift time. Still open: an engine-cooling stat (needs item 3's engine
   temperature model), effects that need track evolution (item 2), and a calibration pass with more bots and tracks
   to check that no setup wins everywhere.

6. **First-lap and cold-tyre caution for the racingline bots (John F One, Spongebob, Dave, Granny).** They plan
   speeds from a grip model and only correct it after a stretch goes wrong, so lap 1 (and qualifying out
   laps, and testing starts) spin or understeer, worst on cold soft tyres: Circuit Raylib turn 1 after the
   long straight, Highmoor at about 1490 m, Brands at about 3630 m. Start the fast stretches a few percent
   under the model's limit until the lap has been learnt (John F Wan does this: 5% under on stretches
   planned above about 62 m/s, with the grip learning earning it back) and take the tyre temperature
   (`axle_grip` against `tire_grip`) into the first lap's plan. Check with testing-mode runs on every track,
   2013 car, soft and hard, for flagged laps and lap time.
7. **Circuit Raylib: gentler first corner.** The 660 m straight runs into turn 1 (s 664-1162), whose radius
   falls from about 100 m to 30 m. Shorten the straight or add a gentle bend before the braking zone, make
   turn 1 a steadier radius (about 70 m) and give it run-off (gravel or paved), so a lap-1 mistake costs
   time rather than a spin. Do after item 6, and check whether it is still needed.
8. **Revamp all tracks with real elevation, banking and camber, and update their 3D models.** The circuits
   are mostly flat or lightly sloped today. Rework each one from its real counterpart (or a believable
   invented one): proper height profile along the lap, banked corners where the real track has them, road
   camber, crests and compressions, with the control-point heights and banks in the `.trk` files (and
   `tools/trackgen.py` where a track is generated). Then update the viewer's track models to match: road
   mesh, kerbs, barriers, run-off, pit lane, terrain and scenery following the new heights, so the picture
   and the sim agree. The bots read the shape through `RRTrackPoint3` already (grade, bank, vertical
   curvature); re-tune their planning where a track becomes much more three-dimensional, and re-check lap
   times, kerb and gravel-trap placement, and the first-lap behaviour of item 6 on every revamped track.

Not planned (judged too small to matter): ride-height aero, camber, tyre pressure, suspension geometry,
MGU-H.
