import argparse
import io
import json
import re
import sys
import time
import zipfile
from datetime import datetime, timezone
from pathlib import Path

import ee
import numpy as np
import requests
import rasterio
from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parent
MELTOUT_DIR = SCRIPT_DIR / "MeltOutRasters"
if str(MELTOUT_DIR) not in sys.path:
    sys.path.insert(0, str(MELTOUT_DIR))

from s2_meltout_base import authenticate_and_initialize, get_export_params, load_aoi


DEFAULT_COLLECTION = "Switzerland/SWISSIMAGE/orthos/10cm"
DEFAULT_PROJECT_ID = "industrial-silo-470310-i8"


def _slugify(value: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9]+", "_", value.strip())
    return slug.strip("_") or "orthophoto"


def _find_latest_file(directory: Path, pattern: str):
    if not directory.exists():
        return None
    matches = list(directory.glob(pattern))
    if not matches:
        return None
    matches.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return matches[0]


def _resolve_default_aoi(map_tag: str):
    meltout_dir = Path(f"analysis_results/Maps/{map_tag}/RS_Data/Meltout")
    aoi = _find_latest_file(meltout_dir, f"AOI_{map_tag}_from_Terrain*.geojson")
    if aoi is not None:
        return aoi
    return _find_latest_file(meltout_dir, "*.geojson")


def _resolve_default_ref_dem(map_tag: str, aoi_geojson: Path):
    if aoi_geojson and aoi_geojson.exists():
        try:
            payload = json.loads(aoi_geojson.read_text(encoding="utf-8"))
            features = payload.get("features") or []
            if features:
                props = features[0].get("properties") or {}
                source_dem = props.get("SourceDEM")
                if source_dem:
                    source_dem_path = Path(source_dem)
                    if source_dem_path.exists():
                        return source_dem_path
        except Exception:
            pass

    terrain_dir = Path(f"analysis_results/Maps/{map_tag}/Terrain")
    preferred = [
        "*_float32.tif",
        "*_u16.tif",
        "*.tif",
    ]
    for pattern in preferred:
        latest = _find_latest_file(terrain_dir, pattern)
        if latest is not None:
            return latest
    return None


def _download_raster(url: str, out_path: Path):
    out_path.parent.mkdir(parents=True, exist_ok=True)

    content = None
    last_err = None
    for attempt in range(1, 5):
        try:
            resp = requests.get(url, timeout=300)
            resp.raise_for_status()
            content = resp.content
            break
        except Exception as exc:
            last_err = exc
            if attempt < 4:
                time.sleep(2 * attempt)
    if content is None:
        raise RuntimeError(f"Failed to download raster after retries: {last_err}")

    if content[:2] == b"PK":
        with zipfile.ZipFile(io.BytesIO(content), "r") as zf:
            tif_names = [name for name in zf.namelist() if name.lower().endswith(".tif")]
            if not tif_names:
                raise RuntimeError("Downloaded ZIP did not contain a .tif.")
            tif_name = tif_names[0]
            zf.extract(tif_name, out_path.parent)
            extracted = out_path.parent / tif_name
            if extracted != out_path:
                if out_path.exists():
                    out_path.unlink()
                extracted.replace(out_path)
            return out_path

    out_path.write_bytes(content)
    return out_path


def _detect_rgb_bands(image: ee.Image):
    band_names = image.bandNames().getInfo()
    candidates = [
        ["R", "G", "B"],
        ["red", "green", "blue"],
        ["B4", "B3", "B2"],
        ["SR_B4", "SR_B3", "SR_B2"],
    ]
    for bands in candidates:
        if all(b in band_names for b in bands):
            return bands
    if len(band_names) >= 3:
        return band_names[:3]
    raise RuntimeError(f"Could not detect RGB bands from: {band_names}")


def _select_rgb_image(args, aoi):
    coll = ee.ImageCollection(args.collection).filterBounds(aoi)

    if args.year is not None:
        start = f"{args.year:04d}-01-01"
        end = f"{args.year + 1:04d}-01-01"
        coll = coll.filterDate(start, end)
    elif args.start_date and args.end_date:
        end_inclusive = ee.Date(args.end_date).advance(1, "day")
        coll = coll.filterDate(args.start_date, end_inclusive)
    elif args.start_date:
        coll = coll.filterDate(args.start_date, "2100-01-01")
    elif args.end_date:
        end_inclusive = ee.Date(args.end_date).advance(1, "day")
        coll = coll.filterDate("1900-01-01", end_inclusive)

    count = int(coll.size().getInfo())
    if count == 0:
        raise RuntimeError(
            f"No imagery found for collection '{args.collection}' over AOI/date filter."
        )

    selected = ee.Image(coll.sort("system:time_start", False).first())
    rgb_bands = _detect_rgb_bands(selected)
    rgb = selected.select(rgb_bands)

    mode = args.value_mode
    if mode == "auto":
        mode = "uint8" if "SWISSIMAGE" in args.collection.upper() else "reflectance_1e4"

    if mode == "reflectance_1":
        vis = rgb.subtract(args.vis_min).divide(args.vis_max - args.vis_min)
        vis = vis.clamp(0, 1).pow(1.0 / args.gamma)
        rgb8 = vis.multiply(255).toUint8()
    elif mode == "reflectance_1e4":
        vis = rgb.multiply(0.0001).subtract(args.vis_min).divide(args.vis_max - args.vis_min)
        vis = vis.clamp(0, 1).pow(1.0 / args.gamma)
        rgb8 = vis.multiply(255).toUint8()
    else:
        rgb8 = rgb.toUint8()

    props = selected.toDictionary(
        ["system:id", "system:index", "system:time_start"]
    ).getInfo()
    return rgb8.clip(aoi), rgb_bands, props, count


def _to_uint8_rgb(arr):
    if arr.dtype == np.uint8:
        return arr

    out = np.zeros(arr.shape, dtype=np.uint8)
    for band_idx in range(arr.shape[2]):
        band = arr[:, :, band_idx].astype(np.float32)
        lo, hi = np.nanpercentile(band, [2, 98])
        if not np.isfinite(lo) or not np.isfinite(hi) or hi <= lo:
            lo = float(np.nanmin(band))
            hi = float(np.nanmax(band))
            if not np.isfinite(lo) or not np.isfinite(hi) or hi <= lo:
                lo, hi = 0.0, 1.0
        scaled = (band - lo) / (hi - lo)
        out[:, :, band_idx] = np.clip(scaled * 255.0, 0, 255).astype(np.uint8)
    return out


def _save_png(path: Path, rgb: np.ndarray):
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgb, mode="RGB").save(path)


def _build_pow2_texture(rgb: np.ndarray, target_size: int):
    h, w = rgb.shape[0], rgb.shape[1]
    if target_size < h or target_size < w:
        raise ValueError(
            f"Requested --ue_texture_size={target_size} is smaller than source {w}x{h}."
        )

    pad_y = target_size - h
    pad_x = target_size - w
    top = pad_y // 2
    bottom = pad_y - top
    left = pad_x // 2
    right = pad_x - left

    padded = np.pad(
        rgb,
        pad_width=((top, bottom), (left, right), (0, 0)),
        mode="edge",
    )

    uv_scale_x = float(w) / float(target_size)
    uv_scale_y = float(h) / float(target_size)
    uv_offset_x = float(left) / float(target_size)
    uv_offset_y = float(top) / float(target_size)

    return padded, {
        "target_size_px": target_size,
        "padding_px": {"left": left, "right": right, "top": top, "bottom": bottom},
        "material_uv": {
            "scale_x": uv_scale_x,
            "scale_y": uv_scale_y,
            "offset_x": uv_offset_x,
            "offset_y": uv_offset_y,
            "note": "Use UV = LandscapeUV * Scale + Offset when sampling the padded texture.",
        },
    }


def main():
    parser = argparse.ArgumentParser(
        description="Download a GEE orthophoto and export UE-friendly textures aligned to a DEM grid."
    )
    parser.add_argument("--map_tag", default="Totalp")
    parser.add_argument("--aoi_geojson", help="AOI GeoJSON path.")
    parser.add_argument("--ref_dem_tif", help="Reference DEM for grid-aligned export.")
    parser.add_argument("--collection", default=DEFAULT_COLLECTION)
    parser.add_argument("--project_id", default=DEFAULT_PROJECT_ID)
    parser.add_argument("--year", type=int, help="Exact year filter (e.g. 2020).")
    parser.add_argument("--start_date", help="YYYY-MM-DD")
    parser.add_argument("--end_date", help="YYYY-MM-DD")
    parser.add_argument(
        "--value_mode",
        choices=["auto", "uint8", "reflectance_1", "reflectance_1e4"],
        default="auto",
    )
    parser.add_argument("--vis_min", type=float, default=0.02)
    parser.add_argument("--vis_max", type=float, default=0.35)
    parser.add_argument("--gamma", type=float, default=1.2)
    parser.add_argument("--scale", type=float, default=2.0, help="Used only if no ref DEM is provided.")
    parser.add_argument("--crs", default="EPSG:2056", help="Used only if no ref DEM is provided.")
    parser.add_argument("--ue_texture_size", type=int, default=2048)
    parser.add_argument("--out_dir", help="Output directory.")
    parser.add_argument("--out_prefix", help="Prefix for output filenames.")
    args = parser.parse_args()

    default_aoi = _resolve_default_aoi(args.map_tag)
    if not args.aoi_geojson:
        if default_aoi is None:
            raise FileNotFoundError(
                f"Could not find default AOI GeoJSON under analysis_results/Maps/{args.map_tag}/RS_Data/Meltout."
            )
        args.aoi_geojson = str(default_aoi)

    if not args.ref_dem_tif:
        ref_dem = _resolve_default_ref_dem(args.map_tag, Path(args.aoi_geojson))
        if ref_dem is not None:
            args.ref_dem_tif = str(ref_dem)

    if not args.out_dir:
        args.out_dir = f"analysis_results/Maps/{args.map_tag}/RS_Data/Orthophoto"

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not args.out_prefix:
        args.out_prefix = f"{args.map_tag}_{_slugify(args.collection)}"

    ts = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    out_tif = out_dir / f"{args.out_prefix}_{ts}_aligned.tif"
    out_png_exact = out_dir / f"{args.out_prefix}_{ts}_UE_RGB_exact.png"
    out_png_pow2 = out_dir / f"{args.out_prefix}_{ts}_UE_RGB_{args.ue_texture_size}.png"
    out_meta = out_dir / f"{args.out_prefix}_{ts}_metadata.json"

    print(f"AOI: {args.aoi_geojson}")
    print(f"Reference DEM: {args.ref_dem_tif or 'None (using region/scale)'}")
    print(f"GEE collection: {args.collection}")

    authenticate_and_initialize(project_id=args.project_id)
    aoi = load_aoi(args.aoi_geojson)

    rgb8_img, rgb_bands, image_props, image_count = _select_rgb_image(args, aoi)

    download_params = {
        "format": "GEO_TIFF",
        "filePerBand": False,
    }
    alignment = {}
    if args.ref_dem_tif:
        ref = get_export_params(args.ref_dem_tif, force_crs=None)
        download_params["crs"] = ref["crs"]
        download_params["crs_transform"] = ref["crsTransform"]
        download_params["dimensions"] = ref["dimensions"]
        alignment = ref
    else:
        download_params["region"] = aoi
        download_params["scale"] = args.scale
        download_params["crs"] = args.crs
        alignment = {"crs": args.crs, "scale": args.scale}

    url = rgb8_img.getDownloadURL(download_params)
    _download_raster(url, out_tif)
    print(f"Saved aligned orthophoto GeoTIFF: {out_tif}")

    with rasterio.open(out_tif) as src:
        if src.count < 3:
            raise RuntimeError(f"Expected at least 3 bands, got {src.count} in {out_tif}")
        rgb = src.read([1, 2, 3])
        rgb = np.moveaxis(rgb, 0, -1)
        rgb = _to_uint8_rgb(rgb)
        source_width = int(src.width)
        source_height = int(src.height)

    _save_png(out_png_exact, rgb)
    print(f"Saved UE exact-size texture PNG: {out_png_exact}")

    pow2_rgb, pow2_meta = _build_pow2_texture(rgb, args.ue_texture_size)
    _save_png(out_png_pow2, pow2_rgb)
    print(f"Saved UE power-of-two texture PNG: {out_png_pow2}")

    sys_time = image_props.get("system:time_start")
    iso_time = None
    if isinstance(sys_time, (int, float)):
        iso_time = datetime.fromtimestamp(float(sys_time) / 1000.0, tz=timezone.utc).isoformat()

    metadata = {
        "map_tag": args.map_tag,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "source": {
            "collection": args.collection,
            "available_images_after_filters": image_count,
            "selected_image_system_id": image_props.get("system:id"),
            "selected_image_system_index": image_props.get("system:index"),
            "selected_image_time_utc": iso_time,
            "rgb_bands_used": rgb_bands,
            "citation_note": "Contains modified Copernicus data and/or swisstopo data. Keep provider attribution in publications.",
        },
        "filters": {
            "year": args.year,
            "start_date": args.start_date,
            "end_date": args.end_date,
            "value_mode": args.value_mode,
            "vis_min": args.vis_min,
            "vis_max": args.vis_max,
            "gamma": args.gamma,
        },
        "alignment": {
            "aoi_geojson": str(Path(args.aoi_geojson)),
            "ref_dem_tif": str(Path(args.ref_dem_tif)) if args.ref_dem_tif else None,
            "export_grid": alignment,
            "exact_texture_size_px": {"width": source_width, "height": source_height},
        },
        "outputs": {
            "aligned_geotiff": str(out_tif),
            "ue_texture_png_exact": str(out_png_exact),
            "ue_texture_png_pow2": str(out_png_pow2),
            "ue_pow2_texture_info": pow2_meta,
        },
    }

    out_meta.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(f"Saved metadata JSON: {out_meta}")


if __name__ == "__main__":
    main()
