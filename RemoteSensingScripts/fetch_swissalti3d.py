#!/usr/bin/env python3
"""
Fetch SwissALTI3D tiles for an AOI and build a clipped/reprojected DEM.

Designed for large horizon-search contexts (e.g., 35 km), while keeping output
resolution manageable for HORAYZON experiments.
"""

from __future__ import annotations

import argparse
import json
import math
import time
import urllib.request
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
import rasterio
from rasterio.crs import CRS
from rasterio.enums import Resampling
from rasterio.transform import Affine
from rasterio.warp import reproject, transform_bounds
from rasterio.windows import from_bounds


def _parse_bbox(values: Sequence[float]) -> Tuple[float, float, float, float]:
    xmin, ymin, xmax, ymax = [float(v) for v in values]
    if xmin >= xmax or ymin >= ymax:
        raise ValueError("Invalid bbox: expected xmin<xmax and ymin<ymax.")
    return xmin, ymin, xmax, ymax


def _bbox_intersection(
    a: Tuple[float, float, float, float], b: Tuple[float, float, float, float]
) -> Optional[Tuple[float, float, float, float]]:
    xmin = max(a[0], b[0])
    ymin = max(a[1], b[1])
    xmax = min(a[2], b[2])
    ymax = min(a[3], b[3])
    if xmin >= xmax or ymin >= ymax:
        return None
    return xmin, ymin, xmax, ymax


def _build_url(year: int, e_km: int, n_km: int, source_res: str) -> str:
    base = "https://data.geo.admin.ch/ch.swisstopo.swissalti3d"
    tile = f"{e_km}-{n_km}"
    fname = f"swissalti3d_{year}_{tile}_{source_res}_2056_5728.tif"
    return f"{base}/swissalti3d_{year}_{tile}/{fname}"


def _head_ok(url: str, timeout_s: int) -> bool:
    try:
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req, timeout=timeout_s) as r:
            return r.status == 200
    except Exception:
        return False


def _download_file(url: str, dst: Path, timeout_s: int, overwrite: bool) -> None:
    if dst.exists() and not overwrite:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=timeout_s) as src, open(dst, "wb") as out:
        while True:
            chunk = src.read(1024 * 1024)
            if not chunk:
                break
            out.write(chunk)


def _years_from_arg(s: str) -> List[int]:
    vals: List[int] = []
    for t in s.split(","):
        t = t.strip()
        if not t:
            continue
        vals.append(int(t))
    if not vals:
        raise ValueError("No years parsed from --years.")
    return vals


def _resampling_from_name(name: str) -> Resampling:
    table = {
        "nearest": Resampling.nearest,
        "bilinear": Resampling.bilinear,
        "cubic": Resampling.cubic,
        "average": Resampling.average,
    }
    k = name.lower().strip()
    if k not in table:
        raise ValueError(f"Unsupported resampling: {name}")
    return table[k]


def _tile_grid_for_bbox(
    bbox: Tuple[float, float, float, float]
) -> Tuple[int, int, int, int, List[Tuple[int, int]]]:
    xmin, ymin, xmax, ymax = bbox
    e0 = int(math.floor(xmin / 1000.0))
    e1 = int(math.ceil(xmax / 1000.0)) - 1
    n0 = int(math.floor(ymin / 1000.0))
    n1 = int(math.ceil(ymax / 1000.0)) - 1
    tiles = [(e, n) for n in range(n0, n1 + 1) for e in range(e0, e1 + 1)]
    return e0, e1, n0, n1, tiles


def _stats(arr: np.ndarray) -> Dict[str, float]:
    valid = np.isfinite(arr)
    if not np.any(valid):
        raise ValueError("No valid cells in output DEM.")
    v = arr[valid].astype(np.float64)
    return {
        "min": float(np.min(v)),
        "max": float(np.max(v)),
        "mean": float(np.mean(v)),
        "std": float(np.std(v)),
        "p01": float(np.percentile(v, 1.0)),
        "p99": float(np.percentile(v, 99.0)),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Fetch SwissALTI3D and build DEM for HORAYZON.")
    parser.add_argument(
        "--reference-dem",
        default="analysis_results/Terrain/processed/piz_ducan_dem_horayzon_ready_float32.tif",
        help="Reference DEM for inner AOI bounds.",
    )
    parser.add_argument(
        "--reference-crs",
        default="EPSG:2056",
        help="Override reference CRS if missing/incorrect.",
    )
    parser.add_argument(
        "--bbox",
        nargs=4,
        type=float,
        metavar=("XMIN", "YMIN", "XMAX", "YMAX"),
        help="Inner AOI bbox (overrides --reference-dem).",
    )
    parser.add_argument(
        "--bbox-crs",
        default="EPSG:2056",
        help="CRS of --bbox.",
    )
    parser.add_argument("--buffer-m", type=float, default=35000.0, help="Outer buffer around inner AOI [m].")
    parser.add_argument("--target-crs", default="EPSG:2056", help="Output DEM CRS.")
    parser.add_argument("--target-res-m", type=float, default=10.0, help="Output DEM resolution [m].")
    parser.add_argument(
        "--source-res",
        default="2",
        choices=["2", "0.5"],
        help="SwissALTI3D source tile resolution key in URL.",
    )
    parser.add_argument(
        "--years",
        default="2023,2022,2021,2020,2019",
        help="Comma-separated year fallback list.",
    )
    parser.add_argument("--head-check", action="store_true", help="Use HTTP HEAD to resolve year before open/download.")
    parser.add_argument(
        "--mode",
        default="stream",
        choices=["stream", "download"],
        help="stream: read /vsicurl tiles; download: save local tiles first.",
    )
    parser.add_argument(
        "--tiles-dir",
        default="analysis_results/Terrain/raw_swissalti3d",
        help="Local tile cache/download directory.",
    )
    parser.add_argument(
        "--out-dem",
        default="analysis_results/Terrain/processed/piz_ducan_swissalti_outer35km_epsg2056_10m_float32.tif",
        help="Output DEM path.",
    )
    parser.add_argument(
        "--out-meta",
        default="analysis_results/Terrain/processed/piz_ducan_swissalti_outer35km_epsg2056_10m_metadata.json",
        help="Output metadata JSON path.",
    )
    parser.add_argument("--out-nodata", type=float, default=-9999.0, help="Output nodata value.")
    parser.add_argument("--timeout-s", type=int, default=35, help="Network timeout.")
    parser.add_argument("--max-tiles", type=int, default=8000, help="Safety tile limit.")
    parser.add_argument("--overwrite-tiles", action="store_true", help="Force re-download local tiles.")
    parser.add_argument("--dry-run", action="store_true", help="Resolve tile list only.")
    parser.add_argument(
        "--resampling",
        default="bilinear",
        choices=["nearest", "bilinear", "cubic", "average"],
        help="Resampling for reprojection to output grid.",
    )
    args = parser.parse_args()

    target_crs = CRS.from_user_input(args.target_crs)
    bbox_crs = CRS.from_user_input(args.bbox_crs)
    years = _years_from_arg(args.years)
    out_dem = Path(args.out_dem).resolve()
    out_meta = Path(args.out_meta).resolve()
    tiles_dir = Path(args.tiles_dir).resolve()
    out_dem.parent.mkdir(parents=True, exist_ok=True)
    out_meta.parent.mkdir(parents=True, exist_ok=True)
    tiles_dir.mkdir(parents=True, exist_ok=True)

    if args.target_res_m <= 0.0:
        raise ValueError("--target-res-m must be > 0")
    if args.buffer_m < 0.0:
        raise ValueError("--buffer-m must be >= 0")

    if args.bbox is not None:
        inner_bbox = _parse_bbox(args.bbox)
        if bbox_crs != target_crs:
            inner_bbox = transform_bounds(bbox_crs, target_crs, *inner_bbox, densify_pts=21)
        bbox_source = "bbox"
        ref_info = None
    else:
        ref_dem = Path(args.reference_dem).resolve()
        if not ref_dem.exists():
            raise FileNotFoundError(f"Reference DEM not found: {ref_dem}")
        with rasterio.open(ref_dem) as ds_ref:
            rb = ds_ref.bounds
            src_ref_crs = ds_ref.crs
        if args.reference_crs:
            src_ref_crs = CRS.from_user_input(args.reference_crs)
        if src_ref_crs is None:
            raise ValueError("Reference CRS unavailable. Set --reference-crs.")
        inner_bbox = (float(rb.left), float(rb.bottom), float(rb.right), float(rb.top))
        if src_ref_crs != target_crs:
            inner_bbox = transform_bounds(src_ref_crs, target_crs, *inner_bbox, densify_pts=21)
        bbox_source = "reference_dem"
        ref_info = str(ref_dem)

    outer_bbox = (
        float(inner_bbox[0] - args.buffer_m),
        float(inner_bbox[1] - args.buffer_m),
        float(inner_bbox[2] + args.buffer_m),
        float(inner_bbox[3] + args.buffer_m),
    )
    e0, e1, n0, n1, tiles = _tile_grid_for_bbox(outer_bbox)
    if len(tiles) > args.max_tiles:
        raise ValueError(
            f"Tile count {len(tiles)} exceeds --max-tiles={args.max_tiles}. "
            "Increase max-tiles intentionally if this is expected."
        )

    out_w = int(round((outer_bbox[2] - outer_bbox[0]) / args.target_res_m))
    out_h = int(round((outer_bbox[3] - outer_bbox[1]) / args.target_res_m))
    if out_w <= 0 or out_h <= 0:
        raise ValueError("Output grid collapsed. Check bbox/buffer/resolution.")
    dst_transform = Affine(args.target_res_m, 0.0, outer_bbox[0], 0.0, -args.target_res_m, outer_bbox[3])

    if args.dry_run:
        meta = {
            "mode": "dry_run",
            "bbox_source": bbox_source,
            "reference_dem": ref_info,
            "inner_bbox_target": [float(v) for v in inner_bbox],
            "outer_bbox_target": [float(v) for v in outer_bbox],
            "target_crs": str(target_crs),
            "target_resolution_m": float(args.target_res_m),
            "tile_grid": {
                "e_km_min": e0,
                "e_km_max": e1,
                "n_km_min": n0,
                "n_km_max": n1,
                "tile_count": len(tiles),
            },
            "source_res": args.source_res,
            "years": years,
            "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        out_meta.write_text(json.dumps(meta, indent=2), encoding="utf-8")
        print(f"Dry-run complete. Tile count: {len(tiles)}")
        print(f"Metadata: {out_meta}")
        return

    dst = np.full((out_h, out_w), np.nan, dtype=np.float32)
    resampling = _resampling_from_name(args.resampling)
    available = []
    missing = []
    used_tiles = 0

    for i, (e_km, n_km) in enumerate(tiles, start=1):
        if i % 200 == 0 or i == 1 or i == len(tiles):
            print(f"[SwissALTI] processing tile {i}/{len(tiles)}")

        tile_success = False
        last_error = None

        for year in years:
            url = _build_url(year, e_km, n_km, args.source_res)
            if args.head_check and (not _head_ok(url, args.timeout_s)):
                continue

            try:
                if args.mode == "download":
                    local_name = f"swissalti3d_{year}_{e_km}-{n_km}_{args.source_res}_2056_5728.tif"
                    local_path = tiles_dir / local_name
                    _download_file(url, local_path, args.timeout_s, overwrite=args.overwrite_tiles)
                    src_path = str(local_path)
                else:
                    src_path = "/vsicurl/" + url

                with rasterio.open(src_path) as src:
                    src_crs = src.crs if src.crs is not None else CRS.from_epsg(2056)
                    src_bounds = (src.bounds.left, src.bounds.bottom, src.bounds.right, src.bounds.top)
                    inter = _bbox_intersection(src_bounds, outer_bbox)
                    if inter is None:
                        tile_success = True
                        available.append({"e_km": e_km, "n_km": n_km, "year": year, "url": url, "used": False})
                        break

                    win = from_bounds(*inter, transform=src.transform)
                    # Expand to integer window bounds
                    col0 = max(0, int(math.floor(win.col_off)))
                    row0 = max(0, int(math.floor(win.row_off)))
                    col1 = min(src.width, int(math.ceil(win.col_off + win.width)))
                    row1 = min(src.height, int(math.ceil(win.row_off + win.height)))
                    if col1 <= col0 or row1 <= row0:
                        tile_success = True
                        available.append({"e_km": e_km, "n_km": n_km, "year": year, "url": url, "used": False})
                        break

                    win2 = rasterio.windows.Window(col_off=col0, row_off=row0, width=col1 - col0, height=row1 - row0)
                    arr = src.read(1, window=win2, masked=False).astype(np.float32)
                    nod = src.nodata
                    if nod is not None:
                        arr[arr == float(nod)] = np.nan
                    arr[~np.isfinite(arr)] = np.nan

                    reproject(
                        source=arr,
                        destination=dst,
                        src_transform=src.window_transform(win2),
                        src_crs=src_crs,
                        dst_transform=dst_transform,
                        dst_crs=target_crs,
                        src_nodata=np.nan,
                        dst_nodata=np.nan,
                        resampling=resampling,
                        init_dest_nodata=False,
                    )

                    tile_success = True
                    used_tiles += 1
                    available.append({"e_km": e_km, "n_km": n_km, "year": year, "url": url, "used": True})
                    break

            except Exception as ex:
                last_error = str(ex)
                continue

        if not tile_success:
            missing.append({"e_km": e_km, "n_km": n_km, "error": last_error})

    valid = np.isfinite(dst)
    out_arr = np.where(valid, dst, float(args.out_nodata)).astype(np.float32)
    profile = {
        "driver": "GTiff",
        "height": out_h,
        "width": out_w,
        "count": 1,
        "dtype": "float32",
        "crs": target_crs,
        "transform": dst_transform,
        "nodata": float(args.out_nodata),
        "compress": "deflate",
        "predictor": 3,
    }
    with rasterio.open(out_dem, "w", **profile) as ds_out:
        ds_out.write(out_arr, 1)

    stats = _stats(dst)
    meta = {
        "source": {
            "dataset": "SwissALTI3D",
            "source_res": args.source_res,
            "years": years,
            "mode": args.mode,
            "head_check": bool(args.head_check),
            "tiles_dir": str(tiles_dir),
        },
        "area": {
            "bbox_source": bbox_source,
            "reference_dem": ref_info,
            "inner_bbox_target": [float(v) for v in inner_bbox],
            "outer_bbox_target": [float(v) for v in outer_bbox],
            "buffer_m": float(args.buffer_m),
            "tile_grid": {
                "e_km_min": e0,
                "e_km_max": e1,
                "n_km_min": n0,
                "n_km_max": n1,
                "tile_count_total": len(tiles),
                "tile_count_processed": used_tiles,
                "tile_count_missing": len(missing),
            },
        },
        "output": {
            "dem_path": str(out_dem),
            "meta_path": str(out_meta),
            "crs": str(target_crs),
            "resolution_m": float(args.target_res_m),
            "width": out_w,
            "height": out_h,
            "nodata": float(args.out_nodata),
        },
        "statistics_m": stats,
        "missing_tiles": missing[:500],
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    out_meta.write_text(json.dumps(meta, indent=2), encoding="utf-8")

    print("SwissALTI3D DEM build completed.")
    print(f"Output DEM: {out_dem}")
    print(f"Metadata: {out_meta}")
    print(f"Tiles used: {used_tiles}/{len(tiles)} (missing: {len(missing)})")


if __name__ == "__main__":
    main()
