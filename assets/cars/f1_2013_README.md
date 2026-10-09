# The 2013 cars (f1_2013_01 .. 11)

Eleven cars from Dave Love's 2013 F1 pack (see CREDITS.md), each in its own folder: `body.glb`, `drs_flap.glb`
(the rear wing's upper element, hinged: `car.json` "drs"), `car.json`, and the livery sheets (`livery_default.png`
the car's paint, `livery_mask.png`, `livery_template.png`, `livery_guide.png`, `livery_views.json`).
`f1_2013_02` (the Mercedes) also has the wheels, which every car uses. How a livery is made:
[f1_2013_02/README.md](f1_2013_02/README.md).

In the game a team drives one of them in its own livery, or the Mercedes in any colour (setup menu: click a team).
`car.json` "team_colour" is the livery's colour on the HUD and minimap.

Rebuilding a folder from the pack: `split.py` (in the pack), then `tools/rig_drs.py`, then
`tools/unwrap_livery.py` (`--mask-threshold -1` for the nine that are not recoloured: it skips the black-parts panel,
which would shrink their dark liveries). Only the Mercedes is recoloured, so only it has that panel.
