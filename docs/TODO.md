# To do

Physics and environment, in the order agreed:

1. **Separate air and track temperature.** `--ambient` is one number for both today. The track surface runs
   10-25 C above the air in sun. Track temperature should drive tyre heating, tyre grip and the cooling of
   tyres and brakes on the road side; air temperature should drive engine cooling and (later) air density.
2. **Rubber on track (track evolution): done** (`src/core/rubber.hpp`). Tyres lay rubber in proportion to their
   wear, the racing line gains up to 3% grip, and the map is shared by a weekend's sessions. Still open: rubber
   washed off by rain (when there is weather), marbles off the line, a stat or compound effect (softs lay more
   already, through wear), and telling robots about it (`RRSensors` carries nothing; they learn it).
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

6. **First-lap and cold-tyre caution for the racingline bots: done.** With no notes from an earlier session the
   bots plan corner speeds from the grip model alone, so lap 1 gives up 5% (Spongebob 8%, `caution=` param) on
   corner-limited stretches above 45 m/s, full at 62 m/s, and replans at the line; the tyre-grip factor follows the
   coldest single tyre. Checked solo on all nine tracks, three compounds, Spongebob and racingline: the lap-1
   run-offs (Circuit Raylib turn 1 on softs, Highmoor at about 1640 m, Brands) are gone, lap 1 costs 0.1-0.4 s.
   Open: qualifying and testing starts are not specially handled, and Highmoor still has a few off-line samples
   on softs at 5% (8% clears it).
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
