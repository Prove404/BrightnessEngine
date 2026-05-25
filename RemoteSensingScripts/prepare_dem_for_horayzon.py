#!/usr/bin/env python3
"""
Prepare a DEM for HORAYZON and similar terrain-parameter workflows.

The script focuses on reproducible preprocessing:
1) decode UE-style UInt16 DEMs back to meters,
2) enforce CRS metadata,
3) keep nodata handling explicit,
4) export a Float32 GeoTIFF and a JSON quality-control report.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

import numpy as np
import rasterio
from rasterio.crs import CRS
from rasterio.enums import Resampling
from rasterio.transform import Affine, array_bounds
from rasterio.windows import Window, from_bounds
from rasterio.warp import reproject


FINSE_HANDBOOK_MIN_Z_M = 1165.7548828125
FINSE_HANDBOOK_MAX_Z_M = 1365.9310302734
DEFAULT_OUT_NODATA = -9999.0


def _parse_bbox(values: Sequence[float]) -> Tuple[float, float, float, float]:
    xmin, ymin, xmax, ymax = [float(v) for v in values]
    if xmin >= xmax or ymin >= ymax:
        raise ValueError("Invalid bbox order. Expected xmin < xmax and ymin < ymax.")
    return xmin, ymin, xmax, ymax


def _clip_bbox_to_bounds(
    bbox: Tuple[float, float, float, float],
    bounds: Tuple[float, float, float, float],
) -> Tuple[float, float, float, float]:
    xmin, ymin, xmax, ymax = bbox
    bxmin, bymin, bxmax, bymax = bounds
    return max(xmin, bxmin), max(ymin, bymin), min(xmax, bxmax), min(ymax, bymax)


def _window_from_bbox(
    bbox: Tuple[float, float, float, float],
    transform: Affine,
    width: int,
    height: int,
) -> Window:
    raw_win = from_bounds(*bbox, transform=transform)
    col0 = max(0, int(math.floor(raw_win.col_off)))
    row0 = max(0, int(math.floor(raw_win.row_off)))
    col1 = min(width, int(math.ceil(raw_win.col_off + raw_win.width)))
    row1 = min(height, int(math.ceil(raw_win.row_off + raw_win.height)))
    if col1 <= col0 or row1 <= row0:
        raise ValueError("Requested bbox/window does not overlap raster cells.")
    return Window(col_off=col0, row_off=row0, width=col1 - col0, height=row1 - row0)


def _resolve_crs(
    src_crs: Optional[CRS],
    force_crs: Optional[str],
    assume_epsg_if_missing: int,
    warnings: List[str],
) -> Optional[CRS]:
    if force_crs:
        return CRS.from_user_input(force_crs)

    if src_crs is None:
        if assume_epsg_if_missing > 0:
            warnings.append(
                f"Input DEM has no CRS. Assuming EPSG:{assume_epsg_if_missing}."
            )
            return CRS.from_epsg(assume_epsg_if_missing)
        return None

    if src_crs.to_epsg() is None and assume_epsg_if_missing > 0:
        warnings.append(
            "Input CRS has no EPSG authority code. "
            f"Overriding with EPSG:{assume_epsg_if_missing}."
        )
        return CRS.from_epsg(assume_epsg_if_missing)

    return src_crs


def _auto_decode_mode(dtype: np.dtype, z_min: Optional[float], z_max: Optional[float]) -> str:
    if np.issubdtype(dtype, np.floating):
        return "none"
    if np.issubdtype(dtype, np.integer):
        if z_min is None or z_max is None:
            raise ValueError(
                "Integer DEM detected but --z-min/--z-max were not supplied. "
                "Provide the source elevation range used before UInt16 scaling."
            )
        return "linear_u16"
    raise ValueError(f"Unsupported DEM dtype for auto decode: {dtype}")


def _decode_elevation(
    raw: np.ndarray,
    dtype: np.dtype,
    decode_mode: str,
    z_min: Optional[float],
    z_max: Optional[float],
) -> Tuple[np.ndarray, str]:
    mode = decode_mode
    if mode == "auto":
        mode = _auto_decode_mode(dtype, z_min, z_max)

    if mode == "none":
        return raw.astype(np.float32), mode

    if mode != "linear_u16":
        raise ValueError(f"Unsupported decode mode: {mode}")

    if z_min is None or z_max is None:
        raise ValueError("--z-min and --z-max are required for linear_u16 decoding.")
    if z_max <= z_min:
        raise ValueError("--z-max must be larger than --z-min.")

    raw_clamped = np.clip(raw, 0.0, 65535.0).astype(np.float32)
    elev_m = z_min + (raw_clamped / 65535.0) * (z_max - z_min)
    return elev_m.astype(np.float32), mode


def _resample_same_crs(
    data: np.ndarray,
    invalid: np.ndarray,
    transform: Affine,
    crs: Optional[CRS],
    target_res_m: float,
    resampling: Resampling,
) -> Tuple[np.ndarray, np.ndarray, Affine]:
    if target_res_m <= 0.0:
        raise ValueError("--target-res-m must be > 0.")
    if crs is None:
        raise ValueError("Cannot resample without CRS metadata. Set --force-crs.")

    height, width = data.shape
    xmin, ymin, xmax, ymax = array_bounds(height, width, transform)
    out_width = max(1, int(round((xmax - xmin) / target_res_m)))
    out_height = max(1, int(round((ymax - ymin) / target_res_m)))
    out_transform = Affine(target_res_m, 0.0, xmin, 0.0, -target_res_m, ymax)

    valid = ~invalid
    if not np.any(valid):
        raise ValueError("All cells are invalid before resampling.")

    fill_val = float(np.nanmean(np.where(valid, data, np.nan)))
    src_filled = np.where(valid, data, fill_val).astype(np.float32)

    dst = np.full((out_height, out_width), np.nan, dtype=np.float32)
    reproject(
        source=src_filled,
        destination=dst,
        src_transform=transform,
        dst_transform=out_transform,
        src_crs=crs,
        dst_crs=crs,
        resampling=resampling,
    )

    src_valid = valid.astype(np.float32)
    dst_valid = np.zeros((out_height, out_width), dtype=np.float32)
    reproject(
        source=src_valid,
        destination=dst_valid,
        src_transform=transform,
        dst_transform=out_transform,
        src_crs=crs,
        dst_crs=crs,
        resampling=Resampling.nearest,
    )
    out_invalid = dst_valid < 0.5
    dst[out_invalid] = np.nan
    return dst.astype(np.float32), out_invalid, out_transform


def _stats_dict(data: np.ndarray) -> Dict[str, float]:
    valid = np.isfinite(data)
    if not np.any(valid):
        raise ValueError("No valid data values available for statistics.")
    vals = data[valid].astype(np.float64)
    return {
        "min": float(np.min(vals)),
        "max": float(np.max(vals)),
        "mean": float(np.mean(vals)),
        "std": float(np.std(vals)),
        "p01": float(np.percentile(vals, 1.0)),
        "p99": float(np.percentile(vals, 99.0)),
    }


def _parse_resampling(name: str) -> Resampling:
    key = name.strip().lower()
    table = {
        "nearest": Resampling.nearest,
        "bilinear": Resampling.bilinear,
        "cubic": Resampling.cubic,
        "average": Resampling.average,
    }
    if key not in table:
        raise ValueError(f"Unsupported resampling: {name}")
    return table[key]


def _estimate_search_capacity_km(
    width: int,
    height: int,
    transform: Affine,
    outer_bbox: Tuple[float, float, float, float],
    inner_bbox: Optional[Tuple[float, float, float, float]],
) -> Dict[str, float]:
    dx = abs(float(transform.a))
    dy = abs(float(transform.e))
    shrink_km = min((width - 1) * dx, (height - 1) * dy) / 2000.0

    out = {
        "max_search_km_if_inner_can_shrink": float(shrink_km),
        "max_search_km_if_full_domain_is_target_inner": 0.0,
    }

    if inner_bbox is not None:
        oxmin, oymin, oxmax, oymax = outer_bbox
        ixmin, iymin, ixmax, iymax = inner_bbox
        border_m = min(ixmin - oxmin, iymin - oymin, oxmax - ixmax, oymax - iymax)
        out["max_search_km_for_given_inner_bbox"] = float(max(border_m, 0.0) / 1000.0)

    return out


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Prepare DEM for HORAYZON (decode, CRS normalization, QC report)."
    )
    parser.add_argument(
        "--input-dem",
        default="analysis_results/Terrain/DTM1_Finse_IntegerClipped_AOI_EPSG25832_NN2000_1m_u16.tif",
        help="Path to input DEM.",
    )
    parser.add_argument(
        "--out-dem",
        default="analysis_results/Terrain/processed/finse_dem_horayzon_ready_float32.tif",
        help="Path to output Float32 DEM.",
    )
    parser.add_argument(
        "--out-meta",
        default="analysis_results/Terrain/processed/finse_dem_horayzon_ready_metadata.json",
        help="Path to output metadata JSON.",
    )
    parser.add_argument(
        "--decode-mode",
        default="auto",
        choices=["auto", "none", "linear_u16"],
        help="Elevation decode mode.",
    )
    parser.add_argument("--z-min", type=float, default=None, help="Min elevation in meters.")
    parser.add_argument("--z-max", type=float, default=None, help="Max elevation in meters.")
    parser.add_argument(
        "--use-finse-handbook-range",
        action="store_true",
        help=(
            "Use z-min/z-max from DEM/Landscape Handbook: "
            f"{FINSE_HANDBOOK_MIN_Z_M}..{FINSE_HANDBOOK_MAX_Z_M} m."
        ),
    )
    parser.add_argument(
        "--force-crs",
        default=None,
        help="Override CRS (example: EPSG:25832).",
    )
    parser.add_argument(
        "--assume-epsg-if-missing",
        type=int,
        default=25832,
        help="Fallback EPSG if source CRS has no authority (0 to disable).",
    )
    parser.add_argument(
        "--extra-nodata-values",
        nargs="*",
        type=float,
        default=[],
        help="Additional source values to treat as nodata.",
    )
    parser.add_argument(
        "--inner-bbox",
        nargs=4,
        type=float,
        metavar=("XMIN", "YMIN", "XMAX", "YMAX"),
        help="Projected bbox of target inner domain.",
    )
    parser.add_argument(
        "--boundary-buffer-m",
        type=float,
        default=0.0,
        help="Boundary buffer around inner bbox (meters).",
    )
    parser.add_argument(
        "--target-res-m",
        type=float,
        default=None,
        help="Optional output grid spacing in meters.",
    )
    parser.add_argument(
        "--resampling",
        default="bilinear",
        choices=["nearest", "bilinear", "cubic", "average"],
        help="Resampling algorithm if --target-res-m is set.",
    )
    parser.add_argument(
        "--vertical-offset-m",
        type=float,
        default=0.0,
        help="Constant vertical offset to add after decode (e.g., geoid correction).",
    )
    parser.add_argument(
        "--out-nodata",
        type=float,
        default=DEFAULT_OUT_NODATA,
        help="Nodata value for output raster.",
    )
    args = parser.parse_args()

    input_dem = Path(args.input_dem).resolve()
    out_dem = Path(args.out_dem).resolve()
    out_meta = Path(args.out_meta).resolve()
    out_dem.parent.mkdir(parents=True, exist_ok=True)
    out_meta.parent.mkdir(parents=True, exist_ok=True)

    if not input_dem.exists():
        raise FileNotFoundError(f"Input DEM not found: {input_dem}")

    z_min = args.z_min
    z_max = args.z_max
    warnings: List[str] = []
    if args.use_finse_handbook_range:
        if z_min is None:
            z_min = FINSE_HANDBOOK_MIN_Z_M
        if z_max is None:
            z_max = FINSE_HANDBOOK_MAX_Z_M
        warnings.append(
            "Using Finse handbook elevation range for UInt16 decode; verify this matches the source DEM."
        )

    with rasterio.open(input_dem) as src:
        raw = src.read(1, masked=False)
        raw_f = raw.astype(np.float32)
        src_dtype = np.dtype(src.dtypes[0])
        src_transform = src.transform
        src_bounds = src.bounds
        src_crs = src.crs
        src_nodata = src.nodata

        invalid = np.zeros(raw.shape, dtype=bool)
        if src_nodata is not None:
            invalid |= raw_f == float(src_nodata)
        for val in args.extra_nodata_values:
            invalid |= np.isclose(raw_f, float(val), atol=0.0)

        work_crs = _resolve_crs(src_crs, args.force_crs, args.assume_epsg_if_missing, warnings)
        decoded, decode_used = _decode_elevation(raw_f, src_dtype, args.decode_mode, z_min, z_max)
        if abs(args.vertical_offset_m) > 0.0:
            decoded = decoded + np.float32(args.vertical_offset_m)

        decoded[invalid] = np.nan

        work_transform = src_transform
        work_outer_bbox = (src_bounds.left, src_bounds.bottom, src_bounds.right, src_bounds.top)
        work_inner_bbox: Optional[Tuple[float, float, float, float]] = None

        if args.inner_bbox is not None:
            if args.boundary_buffer_m < 0.0:
                raise ValueError("--boundary-buffer-m must be >= 0.")

            inner_bbox = _parse_bbox(args.inner_bbox)
            outer_bbox_requested = (
                inner_bbox[0] - args.boundary_buffer_m,
                inner_bbox[1] - args.boundary_buffer_m,
                inner_bbox[2] + args.boundary_buffer_m,
                inner_bbox[3] + args.boundary_buffer_m,
            )
            clipped_outer = _clip_bbox_to_bounds(outer_bbox_requested, work_outer_bbox)
            if clipped_outer != outer_bbox_requested:
                warnings.append(
                    "Requested inner+bounds exceeded source DEM. Outer bbox was clipped to source extent."
                )
            if clipped_outer[0] >= clipped_outer[2] or clipped_outer[1] >= clipped_outer[3]:
                raise ValueError("Outer bbox after clipping is empty.")

            win = _window_from_bbox(clipped_outer, work_transform, src.width, src.height)
            r0 = int(win.row_off)
            c0 = int(win.col_off)
            r1 = r0 + int(win.height)
            c1 = c0 + int(win.width)
            decoded = decoded[r0:r1, c0:c1]
            invalid = invalid[r0:r1, c0:c1]
            work_transform = src.window_transform(win)
            w_h, w_w = decoded.shape
            bxmin, bymin, bxmax, bymax = array_bounds(w_h, w_w, work_transform)
            work_outer_bbox = (bxmin, bymin, bxmax, bymax)
            work_inner_bbox = inner_bbox

        if args.target_res_m is not None:
            src_res_x = abs(float(work_transform.a))
            src_res_y = abs(float(work_transform.e))
            if not (math.isclose(src_res_x, args.target_res_m) and math.isclose(src_res_y, args.target_res_m)):
                decoded, invalid, work_transform = _resample_same_crs(
                    decoded,
                    invalid,
                    work_transform,
                    work_crs,
                    target_res_m=float(args.target_res_m),
                    resampling=_parse_resampling(args.resampling),
                )
                warnings.append(
                    f"DEM was resampled from ~{src_res_x:.3f}x{src_res_y:.3f} m "
                    f"to {args.target_res_m:.3f} m."
                )

    decoded[invalid] = np.nan
    stats = _stats_dict(decoded)
    out_h, out_w = decoded.shape
    out_bounds = array_bounds(out_h, out_w, work_transform)
    out_res_x = abs(float(work_transform.a))
    out_res_y = abs(float(work_transform.e))

    if decode_used == "linear_u16" and (z_min is None or z_max is None):
        raise RuntimeError("Internal error: linear_u16 decode missing z_min/z_max.")

    if decode_used == "linear_u16" and stats["min"] < -500.0:
        warnings.append("Decoded elevations are unexpectedly low (< -500 m). Check decode range.")
    if decode_used == "linear_u16" and stats["max"] > 9000.0:
        warnings.append("Decoded elevations are unexpectedly high (> 9000 m). Check decode range.")

    out_arr = np.where(np.isfinite(decoded), decoded, args.out_nodata).astype(np.float32)
    profile: Dict[str, Any] = {
        "driver": "GTiff",
        "height": out_h,
        "width": out_w,
        "count": 1,
        "dtype": "float32",
        "transform": work_transform,
        "nodata": float(args.out_nodata),
        "compress": "deflate",
        "predictor": 3,
    }
    if work_crs is not None:
        profile["crs"] = work_crs

    with rasterio.open(out_dem, "w", **profile) as dst:
        dst.write(out_arr, 1)

    search_capacity = _estimate_search_capacity_km(
        width=out_w,
        height=out_h,
        transform=work_transform,
        outer_bbox=(out_bounds[0], out_bounds[1], out_bounds[2], out_bounds[3]),
        inner_bbox=work_inner_bbox,
    )

    metadata: Dict[str, Any] = {
        "source_dem": str(input_dem),
        "output_dem": str(out_dem),
        "decode": {
            "mode": decode_used,
            "z_min_m": z_min,
            "z_max_m": z_max,
            "vertical_offset_m": float(args.vertical_offset_m),
        },
        "source_dtype": str(src_dtype),
        "crs": {
            "source_wkt": None if src_crs is None else src_crs.to_wkt(),
            "output_wkt": None if work_crs is None else work_crs.to_wkt(),
            "output_epsg": None if work_crs is None else work_crs.to_epsg(),
        },
        "grid": {
            "width": out_w,
            "height": out_h,
            "resolution_x_m": out_res_x,
            "resolution_y_m": out_res_y,
            "bounds": {
                "xmin": float(out_bounds[0]),
                "ymin": float(out_bounds[1]),
                "xmax": float(out_bounds[2]),
                "ymax": float(out_bounds[3]),
            },
            "transform": [float(v) for v in work_transform[:6]],
        },
        "statistics_m": stats,
        "nodata": {
            "input_nodata": None if src_nodata is None else float(src_nodata),
            "extra_nodata_values": [float(v) for v in args.extra_nodata_values],
            "output_nodata": float(args.out_nodata),
            "invalid_cell_count": int(np.count_nonzero(~np.isfinite(decoded))),
        },
        "horizon_search_capacity": search_capacity,
        "paper_alignment_notes": [
            "Horizon for an inner domain requires a surrounding boundary zone.",
            "Use inner+bounds so boundary width >= chosen horizon search distance.",
        ],
        "warnings": warnings,
    }

    with open(out_meta, "w", encoding="utf-8") as f:
        json.dump(metadata, f, indent=2)

    print("Prepared DEM for HORAYZON.")
    print(f"Input DEM : {input_dem}")
    print(f"Output DEM: {out_dem}")
    print(f"Output meta: {out_meta}")
    print(f"Decode mode: {decode_used}")
    if decode_used == "linear_u16":
        print(f"Decode range: {z_min} .. {z_max} m")
    print(f"Output grid: {out_w}x{out_h} at {out_res_x:.3f} x {out_res_y:.3f} m")
    print(
        "Estimated max search distance (inner can shrink): "
        f"{search_capacity['max_search_km_if_inner_can_shrink']:.3f} km"
    )
    if warnings:
        print("Warnings:")
        for w in warnings:
            print(f" - {w}")


if __name__ == "__main__":
    main()
