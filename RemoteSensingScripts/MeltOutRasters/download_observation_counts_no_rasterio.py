#!/usr/bin/env python3
"""Download S2/Landsat valid observation counts from GEE without rasterio.

This is a narrow fallback for environments where importing rasterio hangs. It
uses an explicit grid transform from a UE/GIS metadata JSON instead of reading
the target grid from a GeoTIFF.
"""

from __future__ import annotations

import argparse
import io
import json
import sys
import zipfile
from pathlib import Path

import ee
import requests

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from s2_meltout_base import (  # noqa: E402
    authenticate_and_initialize,
    build_landsat_collection,
    build_s2_collection,
    load_aoi,
)


def read_grid_json(path: Path, force_crs: str | None) -> dict[str, object]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    width = int(payload["GridWidth"])
    height = int(payload["GridHeight"])
    cell = float(payload.get("CellSizeMeters", payload.get("CellSize", 10.0)))
    x0 = float(payload["ProjectedOriginX"])
    y0 = float(payload["ProjectedOriginY"])
    crs = force_crs or payload.get("Projection") or payload.get("crs")
    if not crs:
        raise ValueError(f"No CRS found in {path}; pass --crs")
    return {
        "crs": crs,
        "crs_transform": [cell, 0.0, x0, 0.0, -cell, y0],
        "dimensions": [width, height],
    }


def download_raster(url: str, out_path: Path) -> Path:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    resp = requests.get(url, timeout=300)
    resp.raise_for_status()
    content = resp.content
    if content[:2] == b"PK":
        with zipfile.ZipFile(io.BytesIO(content), "r") as zf:
            names = [name for name in zf.namelist() if name.lower().endswith((".tif", ".tiff"))]
            if not names:
                raise RuntimeError("Zip download did not contain a TIFF")
            zf.extract(names[0], out_path.parent)
            extracted = out_path.parent / names[0]
            if extracted != out_path:
                extracted.replace(out_path)
            return out_path
    out_path.write_bytes(content)
    return out_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aoi_geojson", required=True)
    parser.add_argument("--grid_json", required=True, help="Metadata JSON with GridWidth/GridHeight/ProjectedOrigin/CellSize.")
    parser.add_argument("--out_tif", required=True)
    parser.add_argument("--start_date", default="2017-10-01")
    parser.add_argument("--end_date", default="2018-08-01")
    parser.add_argument("--project_id", default="industrial-silo-470310-i8")
    parser.add_argument("--s2_max_cloud_pct", type=float, default=90.0)
    parser.add_argument("--landsat_max_cloud_pct", type=float, default=90.0)
    parser.add_argument("--ndsi_threshold", type=float, default=0.4)
    parser.add_argument("--red_min_reflectance", type=float, default=0.12)
    parser.add_argument("--swir1_max_reflectance", type=float, default=0.16)
    parser.add_argument(
        "--disable_reflectance_guards",
        action="store_true",
        help="Use only the NDSI threshold, without red/SWIR1 guard thresholds.",
    )
    parser.add_argument("--crs", default=None)
    args = parser.parse_args()

    print(f"Using AOI: {args.aoi_geojson}")
    print(f"Using grid JSON: {args.grid_json}")
    authenticate_and_initialize(project_id=args.project_id)

    aoi = load_aoi(args.aoi_geojson)
    end_date_inclusive = ee.Date(args.end_date).advance(1, "day")
    start_date_ee = ee.Date(args.start_date)
    s2_col = build_s2_collection(
        aoi=aoi,
        start_date=args.start_date,
        end_date_inclusive=end_date_inclusive,
        start_date_ee=start_date_ee,
        ndsi_threshold=args.ndsi_threshold,
        max_cloud_pct=args.s2_max_cloud_pct,
        red_min_reflectance=args.red_min_reflectance,
        swir1_max_reflectance=args.swir1_max_reflectance,
        use_reflectance_guards=not args.disable_reflectance_guards,
    )
    ls_col = build_landsat_collection(
        aoi=aoi,
        start_date=args.start_date,
        end_date_inclusive=end_date_inclusive,
        start_date_ee=start_date_ee,
        ndsi_threshold=args.ndsi_threshold,
        max_cloud_pct=args.landsat_max_cloud_pct,
        red_min_reflectance=args.red_min_reflectance,
        swir1_max_reflectance=args.swir1_max_reflectance,
        use_reflectance_guards=not args.disable_reflectance_guards,
    )
    merged = s2_col.merge(ls_col)
    count_s2 = s2_col.map(lambda img: img.select("snow_mask").mask().rename("obs_count_s2").unmask(0).toInt16()).sum().rename("obs_count_s2")
    count_ls = ls_col.map(lambda img: img.select("snow_mask").mask().rename("obs_count_landsat").unmask(0).toInt16()).sum().rename("obs_count_landsat")
    count_total = merged.map(lambda img: img.select("snow_mask").mask().rename("obs_count_total").unmask(0).toInt16()).sum().rename("obs_count_total")
    qc = count_s2.addBands(count_ls).addBands(count_total).clip(aoi).toInt16()

    params = {"format": "GEO_TIFF", "filePerBand": False}
    params.update(read_grid_json(Path(args.grid_json), args.crs))
    url = qc.getDownloadURL(params)
    saved = download_raster(url, Path(args.out_tif))
    print(f"Saved observation-count raster: {saved}")


if __name__ == "__main__":
    main()
