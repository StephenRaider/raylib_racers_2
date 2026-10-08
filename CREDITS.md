# Credits

Third-party assets in this repository, by folder. Everything here is CC0 (public domain)
or CC BY 4.0; CC BY items need the attribution below wherever the game is shown or shipped.

## assets/materials (PBR textures)

From [ambientCG](https://ambientcg.com), CC0 1.0. Resized to 1024 px and repacked
(ambient occlusion, roughness and metalness in one `orm.jpg`) by `tools/import_assets.py`.

| Folder | Source material |
|---|---|
| `asphalt` | [Road012A](https://ambientcg.com/view?id=Road012A) |
| `asphalt_worn` | [Road012B](https://ambientcg.com/view?id=Road012B) |
| `concrete` | [Concrete046](https://ambientcg.com/view?id=Concrete046) |
| `grass` | [Ground037](https://ambientcg.com/view?id=Ground037) |
| `gravel` | [Ground062L](https://ambientcg.com/view?id=Ground062L) |
| `dirt` | [Ground082S](https://ambientcg.com/view?id=Ground082S) |
| `metal` | [Metal055A](https://ambientcg.com/view?id=Metal055A) |
| `rubber` | [Rubber004](https://ambientcg.com/view?id=Rubber004) |

`chainlink` (the catch fences) is the wire texture of Poly Haven's
[Modular Chainlink Fence](https://polyhaven.com/a/modular_chainlink_fence), CC0 1.0,
with its alpha mask packed in.

## assets/sky (HDRI skies)

From [Poly Haven](https://polyhaven.com), CC0 1.0. Converted to
RGBE PNG with the sun taken out (the renderer draws it), the ground below the horizon
replaced by lit grass, and prefiltered for reflections, by `tools/import_assets.py`.

| File | Source HDRI |
|---|---|
| `kloofendal_partly_cloudy` | [Kloofendal 48d Partly Cloudy (Pure Sky)](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky) |
| `mud_road` | [Mud Road (Pure Sky)](https://polyhaven.com/a/mud_road_puresky) |
| `overcast_soil` | [Overcast Soil (Pure Sky)](https://polyhaven.com/a/overcast_soil_puresky) |

## assets/cars/f1_2013_02

The Mercedes from "[FREE!] 2013 F1 Pack" (https://skfb.ly/pCRIA) by Dave Love, licensed
CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). Changes: the pack's single model
split into one car (body and wheels, metres, RR layout; see its `car.json`).

## assets/sound

`exhaust_f1v10.wav` is `smooth_35.wav` from [engine-sim](https://github.com/ange-yaghi/engine-sim)
by Ange Yaghi (AngeTheGreat), MIT licence (copy in `assets/sound/ENGINE_SIM_LICENSE.txt`): the exhaust
impulse response of its F1 V10 engine. The V8 sound runs its firing pulses through it.

## assets/scenery

See [assets/scenery/CREDITS.md](assets/scenery/CREDITS.md) (Low Poly Forest Tree Pack, CC BY 4.0;
also the trees in `rr_trackview`).

## assets/fonts

DejaVu fonts, see [assets/fonts/DEJAVU_LICENSE.txt](assets/fonts/DEJAVU_LICENSE.txt).
