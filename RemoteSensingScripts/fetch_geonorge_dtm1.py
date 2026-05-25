#!/usr/bin/env python3
"""
Fetch a fresh DTM1 DEM from Geonorge/Kartverket for a target area.

This script can:
1) Resolve DTM1 tiles from Geonorge ATOM feed for a requested AOI/bbox.
2) Stream-clip only the required area from remote tiles (default), or download tiles first.
3) Mosaic and reproject to a target CRS/resolution (default: EPSG:25832, 1 m).
4) Save a Float32 DEM and metadata report for reproducibility.
"""

from __future__ import annotations

import argparse
import json
import math
import time
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
import rasterio
from rasterio.crs import CRS
from rasterio.enums import Resampling
from rasterio.merge import merge
from rasterio.transform import Affine
from rasterio.warp import reproject, transform_bounds


DEFAULT_FEED_URL = "https://nedlasting.geonorge.no/geonorge/ATOM/hoydedata/datasett/DTM1.atom"
DEFAULT_REFERENCE_DEM = "analysis_results/Terrain/DTM1_Finse_IntegerClipped_AOI_EPSG25832_NN2000_1m_u16.tif"
DEFAULT_TARGET_CRS = "EPSG:25832"
DEFAULT_OUT_DEM = "analysis_results/Terrain/processed/finse_dtm1_geonorge_outer2km_epsg25832_float32.tif"
DEFAULT_OUT_META = "analysis_results/Terrain/processed/finse_dtm1_geonorge_outer2km_metadata.json"
DEFAULT_TILES_DIR = "analysis_results/Terrain/raw_geonorge_dtm1"
DEFAULT_OUT_NODATA = -9999.0


def _parse_bbox(values: Sequence[float]) -> Tuple[float, float, float, float]:
    xmin, ymin, xmax, ymax = [float(v) for v in values]
    if xmin >= xmax or ymin >= ymax:
        raise ValueError("Invalid bbox order. Expected xmin < xmax and ymin < ymax.")
    return xmin, ymin, xmax, ymax


def _bbox_intersects(
    a: Tuple[float, float, float, float], b: Tuple[float, float, float, float]
) -> bool:
    return not (a[2] < b[0] or a[0] > b[2] or a[3] < b[1] or a[1] > b[3])


def _load_text(url: str, timeout: int = 90) -> str:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode("utf-8")


def _load_binary(url: str, timeout: int = 90) -> bytes:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def _load_json(path: Path) -> Optional[Dict[str, Any]]:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _save_json(path: Path, obj: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2), encoding="utf-8")


def _parse_georss_polygon_bbox(text: str) -> Optional[Tuple[float, float, float, float]]:
    raw = text.strip().split()
    if not raw or len(raw) < 6 or len(raw) % 2 != 0:
        return None
    vals = [float(v) for v in raw]
    lats = vals[0::2]
    lons = vals[1::2]
    return min(lons), min(lats), max(lons), max(lats)


def _fetch_feed_entries(
    feed_url: str,
    cache_path: Optional[Path],
    refresh: bool,
    timeout_s: int,
) -> Dict[str, Any]:
    if cache_path is not None and not refresh:
        cached = _load_json(cache_path)
        if cached is not None and "entries" in cached:
            return cached

    xml_text = _load_text(feed_url, timeout=timeout_s)
    root = ET.fromstring(xml_text)
    ns = {"a": "http://www.w3.org/2005/Atom", "g": "http://www.georss.org/georss"}

    entries: List[Dict[str, Any]] = []
    for e in root.findall("a:entry", ns):
        title = e.findtext("a:title", default="", namespaces=ns).strip()
        link_el = e.find("a:link", ns)
        href = (link_el.get("href") or "").strip() if link_el is not None else ""
        poly = e.findtext("g:polygon", default="", namespaces=ns)
        bbox4326 = _parse_georss_polygon_bbox(poly)
        if not href or bbox4326 is None:
            continue
        entries.append(
            {
                "title": title,
                "href": href,
                "bbox4326": [float(v) for v in bbox4326],
                "filename": href.split("/")[-1],
            }
        )

    payload = {
        "feed_url": feed_url,
        "fetched_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "entry_count": len(entries),
        "entries": entries,
    }
    if cache_path is not None:
        _save_json(cache_path, payload)
    return payload


def _maybe_head_content_length(url: str, timeout_s: int = 30) -> Optional[int]:
    try:
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req, timeout=timeout_s) as r:
            val = r.headers.get("Content-Length")
            return int(val) if val is not None else None
    except Exception:
        return None


def _download_file(url: str, dst: Path, timeout_s: int, overwrite: bool = False) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and not overwrite:
        return
    with urllib.request.urlopen(url, timeout=timeout_s) as src, open(dst, "wb") as out:
        while True:
            chunk = src.read(1024 * 1024)
            if not chunk:
                break
            out.write(chunk)


def _get_bbox_from_reference(
    ref_dem: Path,
    reference_crs: Optional[str],
    target_crs: CRS,
) -> Tuple[Tuple[float, float, float, float], CRS]:
    if not ref_dem.exists():
        raise FileNotFoundError(f"Reference DEM not found: {ref_dem}")
    with rasterio.open(ref_dem) as ds:
        b = ds.bounds
        src_crs = ds.crs

    if reference_crs:
        src_crs = CRS.from_user_input(reference_crs)
    if src_crs is None:
        raise ValueError(
            "Reference DEM has no CRS. Provide --reference-crs (e.g. EPSG:25832)."
        )

    bbox = (float(b.left), float(b.bottom), float(b.right), float(b.top))
    if src_crs != target_crs:
        bbox = transform_bounds(src_crs, target_crs, *bbox, densify_pts=21)
    return bbox, src_crs


def _as_resampling(name: str) -> Resampling:
    table = {
        "nearest": Resampling.nearest,
        "bilinear": Resampling.bilinear,
        "cubic": Resampling.cubic,
        "average": Resampling.average,
    }
    key = name.strip().lower()
    if key not in table:
        raise ValueError(f"Unsupported resampling method: {name}")
    return table[key]


def _stats(arr: np.ndarray) -> Dict[str, float]:
    valid = np.isfinite(arr)
    if not np.any(valid):
        raise ValueError("No valid pixels in result DEM.")
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
    parser = argparse.ArgumentParser(
        description="Fetch and prepare fresh Geonorge DTM1 DEM for Finse/HORAYZON workflows."
    )
    parser.add_argument("--feed-url", default=DEFAULT_FEED_URL, help="DTM1 ATOM feed URL.")
    parser.add_argument(
        "--feed-cache",
        default="analysis_results/Terrain/processed/geonorge_dtm1_feed_cache.json",
        help="Cache path for parsed feed entries.",
    )
    parser.add_argument("--refresh-feed", action="store_true", help="Force feed refresh.")
    parser.add_argument(
        "--reference-dem",
        default=DEFAULT_REFERENCE_DEM,
        help="Reference DEM to define inner bbox (simulation area).",
    )
    parser.add_argument(
        "--reference-crs",
        default=DEFAULT_TARGET_CRS,
        help="Override CRS for reference DEM if authority info is missing.",
    )
    parser.add_argument(
        "--bbox",
        nargs=4,
        type=float,
        metavar=("XMIN", "YMIN", "XMAX", "YMAX"),
        help="Alternative inner bbox; if set, --reference-dem is ignored.",
    )
    parser.add_argument(
        "--bbox-crs",
        default=DEFAULT_TARGET_CRS,
        help="CRS of --bbox coordinates (default EPSG:25832).",
    )
    parser.add_argument(
        "--buffer-m",
        type=float,
        default=2000.0,
        help="Boundary buffer around inner bbox in meters (for horizon search).",
    )
    parser.add_argument("--target-crs", default=DEFAULT_TARGET_CRS, help="Output DEM CRS.")
    parser.add_argument("--target-res-m", type=float, default=1.0, help="Output resolution in meters.")
    parser.add_argument(
        "--resampling",
        default="bilinear",
        choices=["nearest", "bilinear", "cubic", "average"],
        help="Resampling for reprojection.",
    )
    parser.add_argument(
        "--mode",
        default="stream",
        choices=["stream", "download"],
        help=(
            "stream: read remote tiles via /vsicurl and clip; "
            "download: download full tiles first, then clip."
        ),
    )
    parser.add_argument("--tiles-dir", default=DEFAULT_TILES_DIR, help="Tile cache/download directory.")
    parser.add_argument("--out-dem", default=DEFAULT_OUT_DEM, help="Output DEM path.")
    parser.add_argument("--out-meta", default=DEFAULT_OUT_META, help="Output metadata JSON path.")
    parser.add_argument("--out-nodata", type=float, default=DEFAULT_OUT_NODATA, help="Output nodata value.")
    parser.add_argument(
        "--max-pixels",
        type=int,
        default=120_000_000,
        help="Safety limit for output pixel count.",
    )
    parser.add_argument("--timeout-s", type=int, default=90, help="Network timeout in seconds.")
    parser.add_argument("--dry-run", action="store_true", help="Resolve tiles and stop before DEM generation.")
    args = parser.parse_args()

    target_crs = CRS.from_user_input(args.target_crs)
    bbox_crs = CRS.from_user_input(args.bbox_crs)
    out_dem = Path(args.out_dem).resolve()
    out_meta = Path(args.out_meta).resolve()
    tiles_dir = Path(args.tiles_dir).resolve()
    feed_cache = Path(args.feed_cache).resolve() if args.feed_cache else None
    ref_dem = Path(args.reference_dem).resolve()

    out_dem.parent.mkdir(parents=True, exist_ok=True)
    out_meta.parent.mkdir(parents=True, exist_ok=True)
    tiles_dir.mkdir(parents=True, exist_ok=True)

    if args.target_res_m <= 0.0:
        raise ValueError("--target-res-m must be > 0.")
    if args.buffer_m < 0.0:
        raise ValueError("--buffer-m must be >= 0.")

    if args.bbox is not None:
        inner_bbox_input = _parse_bbox(args.bbox)
        inner_bbox_target = (
            inner_bbox_input
            if bbox_crs == target_crs
            else transform_bounds(bbox_crs, target_crs, *inner_bbox_input, densify_pts=21)
        )
        bbox_source_note = "user_bbox"
        reference_crs_text = str(bbox_crs)
    else:
        inner_bbox_target, src_ref_crs = _get_bbox_from_reference(
            ref_dem=ref_dem,
            reference_crs=args.reference_crs,
            target_crs=target_crs,
        )
        bbox_source_note = "reference_dem"
        reference_crs_text = str(src_ref_crs)

    outer_bbox_target = (
        float(inner_bbox_target[0] - args.buffer_m),
        float(inner_bbox_target[1] - args.buffer_m),
        float(inner_bbox_target[2] + args.buffer_m),
        float(inner_bbox_target[3] + args.buffer_m),
    )

    width = int(round((outer_bbox_target[2] - outer_bbox_target[0]) / args.target_res_m))
    height = int(round((outer_bbox_target[3] - outer_bbox_target[1]) / args.target_res_m))
    if width <= 0 or height <= 0:
        raise ValueError("Computed output grid is empty. Check bbox/buffer/resolution.")
    if (not args.dry_run) and (width * height > args.max_pixels):
        raise ValueError(
            f"Output grid {width}x{height} exceeds max-pixels={args.max_pixels}. "
            "Reduce area/buffer or increase resolution."
        )

    outer_bbox_4326 = transform_bounds(target_crs, "EPSG:4326", *outer_bbox_target, densify_pts=21)

    feed = _fetch_feed_entries(
        feed_url=args.feed_url,
        cache_path=feed_cache,
        refresh=args.refresh_feed,
        timeout_s=args.timeout_s,
    )
    entries: List[Dict[str, Any]] = feed["entries"]
    selected = [e for e in entries if _bbox_intersects(tuple(e["bbox4326"]), outer_bbox_4326)]
    selected.sort(key=lambda x: x["filename"])

    if not selected:
        raise RuntimeError("No DTM1 tiles from feed intersect the requested AOI.")

    total_head_bytes = 0
    for e in selected:
        size = _maybe_head_content_length(e["href"], timeout_s=min(45, args.timeout_s))
        e["content_length"] = size
        if size is not None:
            total_head_bytes += size

    if args.dry_run:
        report = {
            "mode": "dry_run",
            "bbox_source": bbox_source_note,
            "target_crs": str(target_crs),
            "inner_bbox_target": [float(v) for v in inner_bbox_target],
            "outer_bbox_target": [float(v) for v in outer_bbox_target],
            "outer_bbox_4326": [float(v) for v in outer_bbox_4326],
            "selected_tile_count": len(selected),
            "selected_tiles": selected,
            "estimated_total_bytes": total_head_bytes if total_head_bytes > 0 else None,
        }
        _save_json(out_meta, report)
        print(f"Dry-run complete. Selected {len(selected)} tiles.")
        print(f"Metadata: {out_meta}")
        return

    source_paths: List[str] = []
    if args.mode == "download":
        for e in selected:
            dst = tiles_dir / e["filename"]
            _download_file(e["href"], dst, timeout_s=args.timeout_s, overwrite=False)
            source_paths.append(str(dst))
    else:
        source_paths = ["/vsicurl/" + e["href"] for e in selected]

    srcs = [rasterio.open(p) for p in source_paths]
    try:
        src_crs = srcs[0].crs
        if src_crs is None:
            raise RuntimeError("Source DTM1 tile CRS missing.")
        src_res = srcs[0].res
        for s in srcs[1:]:
            if s.crs != src_crs:
                raise RuntimeError("Selected tiles have mixed CRS; aborting.")
        outer_bbox_src = transform_bounds(target_crs, src_crs, *outer_bbox_target, densify_pts=21)
        # Read only as fine as needed for final output resolution.
        # For target_res_m > source resolution, this avoids pulling full 1 m rasters.
        merge_res = (
            max(abs(float(src_res[0])), float(args.target_res_m)),
            max(abs(float(src_res[1])), float(args.target_res_m)),
        )
        mosaic, mosaic_transform = merge(
            srcs,
            bounds=outer_bbox_src,
            res=merge_res,
            nodata=np.nan,
        )
    finally:
        for s in srcs:
            s.close()

    src_arr = mosaic[0].astype(np.float32)
    src_arr[~np.isfinite(src_arr)] = np.nan

    dst_transform = Affine(
        float(args.target_res_m),
        0.0,
        float(outer_bbox_target[0]),
        0.0,
        -float(args.target_res_m),
        float(outer_bbox_target[3]),
    )
    dst_arr = np.full((height, width), np.nan, dtype=np.float32)

    reproject(
        source=src_arr,
        destination=dst_arr,
        src_transform=mosaic_transform,
        src_crs=src_crs,
        dst_transform=dst_transform,
        dst_crs=target_crs,
        src_nodata=np.nan,
        dst_nodata=np.nan,
        resampling=_as_resampling(args.resampling),
    )

    valid = np.isfinite(dst_arr)
    dst_save = np.where(valid, dst_arr, float(args.out_nodata)).astype(np.float32)
    profile: Dict[str, Any] = {
        "driver": "GTiff",
        "height": height,
        "width": width,
        "count": 1,
        "dtype": "float32",
        "crs": target_crs,
        "transform": dst_transform,
        "nodata": float(args.out_nodata),
        "compress": "deflate",
        "predictor": 3,
    }
    with rasterio.open(out_dem, "w", **profile) as dst:
        dst.write(dst_save, 1)

    stats = _stats(dst_arr)
    max_search_km = float(args.buffer_m / 1000.0)

    metadata = {
        "source": {
            "provider": "Kartverket / Geonorge",
            "dataset_feed": args.feed_url,
            "feed_cache": str(feed_cache) if feed_cache else None,
            "feed_fetched_utc": feed.get("fetched_utc"),
            "entry_count_in_feed": feed.get("entry_count"),
            "selected_tile_count": len(selected),
            "selected_tiles": selected,
            "estimated_total_tile_bytes": total_head_bytes if total_head_bytes > 0 else None,
            "mode": args.mode,
            "tiles_dir": str(tiles_dir),
        },
        "area": {
            "bbox_source": bbox_source_note,
            "reference_dem": str(ref_dem) if bbox_source_note == "reference_dem" else None,
            "reference_crs": reference_crs_text,
            "inner_bbox_target": [float(v) for v in inner_bbox_target],
            "outer_bbox_target": [float(v) for v in outer_bbox_target],
            "outer_bbox_4326": [float(v) for v in outer_bbox_4326],
            "buffer_m": float(args.buffer_m),
        },
        "output": {
            "dem_path": str(out_dem),
            "meta_path": str(out_meta),
            "crs": str(target_crs),
            "resolution_m": float(args.target_res_m),
            "width": width,
            "height": height,
            "nodata": float(args.out_nodata),
        },
        "statistics_m": stats,
        "horizon_guidance": {
            "max_search_km_if_inner_equals_reference_bbox": max_search_km,
            "note": (
                "Choose horizon search distance <= buffer distance for edge-safe inner-domain results."
            ),
        },
        "vertical_datum_note": (
            "Geonorge DTM1 tiles are provided with NN2000 vertical reference; "
            "apply additional vertical transformation only if your workflow expects a different datum."
        ),
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }

    _save_json(out_meta, metadata)

    print("Geonorge DTM1 DEM fetch completed.")
    print(f"Selected tiles: {len(selected)}")
    print(f"Output DEM: {out_dem}")
    print(f"Output metadata: {out_meta}")
    print(f"Output grid: {width} x {height} @ {args.target_res_m} m ({target_crs})")
    print(f"Estimated max safe horizon search distance: {max_search_km:.3f} km")


if __name__ == "__main__":
    main()
