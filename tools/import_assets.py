#!/usr/bin/env python3
"""Imports the CC0 PBR materials and HDRI skies RR2 uses into assets/.

Materials (ambientCG zips) become assets/materials/<key>/:
    albedo.jpg   sRGB base colour
    normal.jpg   OpenGL-convention tangent-space normal map
    orm.jpg      R = ambient occlusion, G = roughness, B = metalness (linear)
HDRI skies (Poly Haven .exr) become assets/sky/<key>.png plus <key>.json:
    the PNG holds RGBE (shared-exponent HDR) in RGBA, equirectangular, row 0 at the zenith,
    u = 0.5 + atan2(x, -z) / 2pi for a direction (x, y, z) with y up;
    the json gives the sun's direction and irradiance, which are taken out of the image
    (the renderer lights the scene with an analytic sun instead).

Needs numpy, Pillow and opencv-python (for reading OpenEXR). Run from the repo root:

    python3 tools/import_assets.py "D:/RR2 assets"
"""
import io
import json
import math
import os
import sys
import zipfile

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import numpy as np
from PIL import Image

# key: (ambientCG id, texture size, what it is used for)
MATERIALS = {
    "asphalt": ("Road012A", 1024, "racing surface"),
    "asphalt_worn": ("Road012B", 1024, "pit lane and aprons"),
    "concrete": ("Concrete046", 1024, "walls and barriers"),
    "grass": ("Ground037", 1024, "grass verges and terrain"),
    "gravel": ("Ground062L", 1024, "gravel traps"),
    "dirt": ("Ground082S", 1024, "forest floor and dirt patches"),
    "metal": ("Metal055A", 1024, "armco and posts"),
}
SKIES = {
    "kloofendal_partly_cloudy": "kloofendal_48d_partly_cloudy_puresky_2k.exr",
    "mud_road": "mud_road_puresky_2k.exr",
    "overcast_soil": "overcast_soil_puresky_2k.exr",
}


def read_map(z, name, size, mode):
    with z.open(name) as f:
        im = Image.open(io.BytesIO(f.read()))
        im.load()
    return im.convert(mode).resize((size, size), Image.LANCZOS)


def material(src, key, acg, size):
    path = os.path.join(src, f"{acg}_2K-JPG.zip")
    out = os.path.join("assets", "materials", key)
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        def find(suffix):
            return next((n for n in names if n.endswith(f"_{suffix}.jpg")), None)
        read_map(z, find("Color"), size, "RGB").save(os.path.join(out, "albedo.jpg"), quality=90)
        read_map(z, find("NormalGL"), size, "RGB").save(os.path.join(out, "normal.jpg"), quality=92)
        rough = np.asarray(read_map(z, find("Roughness"), size, "L"))
        ao = np.asarray(read_map(z, find("AmbientOcclusion"), size, "L")) if find("AmbientOcclusion") else np.full_like(rough, 255)
        metal = np.asarray(read_map(z, find("Metalness"), size, "L")) if find("Metalness") else np.zeros_like(rough)
        Image.fromarray(np.dstack([ao, rough, metal])).save(os.path.join(out, "orm.jpg"), quality=92)
    return out


def rgbe(rgb):
    """float32 HxWx3 -> uint8 HxWx4 Radiance RGBE."""
    m = rgb.max(axis=2)
    e = np.zeros(m.shape, np.int32)
    mant, e = np.frexp(m)
    scale = np.where(m > 1e-32, mant * 256.0 / np.maximum(m, 1e-32), 0.0)
    out = np.zeros(rgb.shape[:2] + (4,), np.uint8)
    out[..., :3] = np.clip(rgb * scale[..., None], 0, 255).astype(np.uint8)
    out[..., 3] = np.where(m > 1e-32, e + 128, 0).astype(np.uint8)
    return out


def directions(h, w):
    """Unit direction of each texel of an h x w equirectangular map (see the module doc)."""
    th = (np.arange(h) + 0.5) / h * math.pi
    ph = ((np.arange(w) + 0.5) / w - 0.5) * 2 * math.pi
    th, ph = np.meshgrid(th, ph, indexing="ij")
    return np.stack([np.sin(th) * np.sin(ph), np.cos(th), -np.sin(th) * np.cos(ph)], axis=-1)


def downsample(rgb, h):
    while rgb.shape[0] > h:
        rgb = 0.25 * (rgb[0::2, 0::2] + rgb[1::2, 0::2] + rgb[0::2, 1::2] + rgb[1::2, 1::2])
    return rgb


def sh9(rgb, domega):
    """Irradiance as 9 spherical-harmonic coefficients (RGB), already convolved with the
    cosine lobe, so E(n) = sum c_i Y_i(n) (Ramamoorthi & Hanrahan)."""
    small = downsample(rgb, 128)
    h, w = small.shape[:2]
    d = directions(h, w)
    dw = ((2 * math.pi / w) * (math.pi / h) * np.sin((np.arange(h) + 0.5) / h * math.pi))[:, None]
    x, y, z = d[..., 0], d[..., 1], d[..., 2]
    Y = [0.282095 + 0 * x, 0.488603 * y, 0.488603 * z, 0.488603 * x, 1.092548 * x * y, 1.092548 * y * z,
         0.315392 * (3 * z * z - 1), 1.092548 * x * z, 0.546274 * (x * x - y * y)]
    band = [math.pi] + [2 * math.pi / 3] * 3 + [math.pi / 4] * 5
    return [(small * (Yi * dw)[..., None]).sum((0, 1)) * a for Yi, a in zip(Y, band)]


GROUND_ALBEDO = np.array([0.16, 0.19, 0.10])  # what the land below the horizon reflects
SPEC_W = 512  # atlas width; level k is (512 >> k) x (256 >> k), stacked from the top
SPEC_LEVELS = 6


def specular_atlas(rgb):
    """GGX-prefiltered copies of the sky (N = V = R), roughness k / (levels - 1) for level k,
    stacked in one 512 x 512 image. The renderer blends between levels by roughness."""
    atlas = np.zeros((SPEC_W, SPEC_W, 3), np.float32)
    y = 0
    for k in range(SPEC_LEVELS):
        w, h = SPEC_W >> k, (SPEC_W // 2) >> k
        r = k / (SPEC_LEVELS - 1)
        if k == 0:
            level = downsample(rgb, h)
        else:
            src = downsample(rgb, max(32, min(128, h * 2)))
            sh, sw = src.shape[:2]
            sd = directions(sh, sw).reshape(-1, 3)
            sdw = np.repeat((2 * math.pi / sw) * (math.pi / sh) * np.sin((np.arange(sh) + 0.5) / sh * math.pi), sw)
            srgb = src.reshape(-1, 3)
            od = directions(h, w).reshape(-1, 3)
            a2 = max(r * r, 1e-3) ** 2
            level = np.zeros((h * w, 3))
            for c in range(0, len(od), 256):
                ndl = (od[c:c + 256] @ sd.T).astype(np.float32)
                nl = np.clip(ndl, 0, None)
                D = a2 / (math.pi * (nl * nl * (a2 - 1) + 1) ** 2)
                wgt = D * nl * sdw[None, :]
                level[c:c + 256] = (wgt @ srgb) / np.maximum(wgt.sum(1, keepdims=True), 1e-12)
            level = level.reshape(h, w, 3)
        atlas[y:y + h, :w] = level
        y += h
    return atlas


def sky(src, key, fname):
    import cv2
    im = cv2.imread(os.path.join(src, fname), cv2.IMREAD_UNCHANGED)
    if im is None:
        raise SystemExit(f"cannot read {fname} (opencv without OpenEXR?)")
    rgb = im[..., :3][..., ::-1].astype(np.float64)  # BGR(A) -> RGB
    h, w = rgb.shape[:2]
    v = (np.arange(h) + 0.5) / h
    theta = v * math.pi                       # from the zenith
    domega = (2 * math.pi / w) * (math.pi / h) * np.sin(theta)  # solid angle per pixel, by row
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722])
    # the sun: everything far brighter than the sky around it
    cut = max(np.percentile(lum, 99.9) * 4.0, 60.0)
    mask = lum > cut
    out = {"source": fname, "licence": "CC0 (Poly Haven)"}
    if mask.any():
        ys, xs = np.nonzero(mask)
        wgt = lum[ys, xs] * domega[ys]
        # direction of each sun pixel, weighted
        u = (xs + 0.5) / w
        th = (ys + 0.5) / h * math.pi
        phi = (u - 0.5) * 2 * math.pi
        d = np.stack([np.sin(th) * np.sin(phi), np.cos(th), -np.sin(th) * np.cos(phi)], axis=1)
        sd = (d * wgt[:, None]).sum(0)
        sd /= np.linalg.norm(sd)
        E = (rgb[ys, xs] * domega[ys][:, None]).sum(0)  # irradiance on a surface facing the sun
        out["sun_dir"] = [round(float(c), 5) for c in sd]
        out["sun_irradiance"] = [round(float(c), 4) for c in E]
        out["sun_pixels"] = int(mask.sum())
        # take the sun out: replace it by the sky just around it
        ring = np.zeros_like(mask)
        pad = 6
        y0, y1, x0, x1 = max(0, ys.min() - pad), min(h, ys.max() + pad + 1), max(0, xs.min() - pad), min(w, xs.max() + pad + 1)
        ring[y0:y1, x0:x1] = True
        ring &= ~mask
        fill = np.median(rgb[ring], axis=0) if ring.any() else np.array([cut] * 3)
        rgb[mask] = fill
    # sky irradiance on an upward-facing surface, for reference
    up = np.clip(np.cos(theta) * domega, 0, None)
    sky_up = (rgb * up[:, None, None]).sum((0, 1))
    out["sky_irradiance_up"] = [round(float(c), 4) for c in sky_up]
    # Below the horizon these skies hold a dim mirror of the sky; put lit ground there
    # instead (moorland grass), so the bounce light and low reflections look right.
    e_ground = sky_up + (np.array(out["sun_irradiance"]) * max(out["sun_dir"][1], 0.0) if "sun_dir" in out else 0.0)
    ground = GROUND_ALBEDO * e_ground / math.pi
    below = np.clip((theta - math.pi / 2) / math.radians(6.0), 0.0, 1.0)
    below = below * below * (3 - 2 * below)
    rgb = rgb * (1 - below[:, None, None]) + ground[None, None, :] * below[:, None, None]
    out["ground_radiance"] = [round(float(c), 4) for c in ground]
    os.makedirs(os.path.join("assets", "sky"), exist_ok=True)
    Image.fromarray(rgbe(rgb.astype(np.float32)), "RGBA").save(os.path.join("assets", "sky", f"{key}.png"), optimize=True)
    out["sh9"] = [[round(float(c), 5) for c in row] for row in sh9(rgb, domega)]
    Image.fromarray(rgbe(specular_atlas(rgb)), "RGBA").save(os.path.join("assets", "sky", f"{key}_spec.png"), optimize=True)
    with open(os.path.join("assets", "sky", f"{key}.json"), "w") as f:
        json.dump(out, f, indent=2)
        f.write("\n")
    return out


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    for key, (acg, size, _) in MATERIALS.items():
        print("material", key, "<-", acg, "->", material(src, key, acg, size))
    for key, fname in SKIES.items():
        info = sky(src, key, fname)
        print("sky", key, info.get("sun_dir"), info.get("sun_irradiance"), "sky up", info["sky_irradiance_up"])


if __name__ == "__main__":
    main()
