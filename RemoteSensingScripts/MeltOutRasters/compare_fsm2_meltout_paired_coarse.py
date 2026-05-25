import argparse
import json
import math
from pathlib import Path

import numpy as np
import pandas as pd

try:
    import matplotlib.pyplot as plt
    import rasterio
    from rasterio.transform import array_bounds, Affine
except ImportError as exc:  # pragma: no cover
    raise SystemExit(f"Required dependency missing: {exc}")

from compare_meltout_rasters import (
    MELTOUT_NODATA,
    apply_border_trim_inplace,
    discover_meltout_json_files,
    read_ue_export,
    reproject_to_profile,
)


def find_latest_file(directory, pattern):
    base = Path(directory)
    if not base.exists():
        return None
    matches = list(base.glob(pattern))
    if not matches:
        return None
    matches.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return matches[0]


def pick_latest_matching_json(search_root, required_tokens):
    required_tokens = [token.lower() for token in required_tokens]
    candidates = []
    for path in discover_meltout_json_files(search_root):
        lowered = str(path).lower()
        if all(token in lowered for token in required_tokens):
            candidates.append(path)
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def load_reference_raster(path):
    with rasterio.open(path) as ds:
        data = ds.read(1).astype(np.float32)
        nodata = ds.nodata
        profile = ds.profile.copy()
        bounds = ds.bounds
        crs = ds.crs

    if (nodata is None or nodata != -9999) and np.any(data == -9999):
        print("Detected -9999 fill values in reference raster. Treating -9999 as nodata.")
        nodata = -9999
        profile["nodata"] = -9999

    print(f"Reference raster: {path}")
    print(f"  CRS: {crs}")
    print(f"  Bounds: {bounds}")
    print(f"  Shape: {data.shape}")
    return data, nodata, profile


def load_obs_count_raster(path):
    if path is None:
        return None
    with rasterio.open(path) as ds:
        band = 3 if ds.count >= 3 else 1
        data = ds.read(band).astype(np.float32)
        print(f"Observation-count raster: {path} (band {band})")
        print(f"  Shape: {data.shape}")
        return data


def valid_melt_mask(arr, nodata=MELTOUT_NODATA):
    return np.isfinite(arr) & (arr != nodata) & (arr > 0)


def aggregate_blocks_median(data, valid_mask, factor, min_valid_frac, nodata=MELTOUT_NODATA):
    coarse_h = data.shape[0] // factor
    coarse_w = data.shape[1] // factor
    out = np.full((coarse_h, coarse_w), nodata, dtype=np.float32)
    coverage = np.zeros((coarse_h, coarse_w), dtype=np.float32)
    total_pixels = factor * factor
    min_valid = int(math.ceil(min_valid_frac * total_pixels))

    for row in range(coarse_h):
        y0 = row * factor
        y1 = y0 + factor
        for col in range(coarse_w):
            x0 = col * factor
            x1 = x0 + factor
            block_valid = valid_mask[y0:y1, x0:x1]
            valid_count = int(block_valid.sum())
            coverage[row, col] = valid_count / float(total_pixels)
            if valid_count < min_valid:
                continue
            values = data[y0:y1, x0:x1][block_valid]
            if values.size == 0:
                continue
            out[row, col] = float(np.median(values))
    return out, coverage


def aggregate_blocks_mean(data, valid_mask, factor, min_valid_frac, nodata=MELTOUT_NODATA):
    coarse_h = data.shape[0] // factor
    coarse_w = data.shape[1] // factor
    out = np.full((coarse_h, coarse_w), nodata, dtype=np.float32)
    total_pixels = factor * factor
    min_valid = int(math.ceil(min_valid_frac * total_pixels))

    for row in range(coarse_h):
        y0 = row * factor
        y1 = y0 + factor
        for col in range(coarse_w):
            x0 = col * factor
            x1 = x0 + factor
            block_valid = valid_mask[y0:y1, x0:x1]
            valid_count = int(block_valid.sum())
            if valid_count < min_valid:
                continue
            values = data[y0:y1, x0:x1][block_valid]
            if values.size == 0:
                continue
            out[row, col] = float(np.mean(values))
    return out


def build_coarse_profile(fine_profile, factor):
    fine_transform = fine_profile["transform"]
    width = fine_profile["width"] // factor
    height = fine_profile["height"] // factor
    transform = Affine(
        fine_transform.a * factor,
        fine_transform.b,
        fine_transform.c,
        fine_transform.d,
        fine_transform.e * factor,
        fine_transform.f,
    )
    profile = fine_profile.copy()
    profile.update(
        driver="GTiff",
        dtype="float32",
        nodata=MELTOUT_NODATA,
        width=width,
        height=height,
        count=1,
        transform=transform,
    )
    return profile


def write_float_tif(path, data, profile):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with rasterio.open(path, "w", **profile) as dst:
        dst.write(data.astype(np.float32), 1)


def calculate_metrics(model, ref, mask):
    if not mask.any():
        return {
            "n_cells": 0,
            "mean_bias_days": None,
            "median_bias_days": None,
            "mae_days": None,
            "rmse_days": None,
            "within_3_days_pct": None,
            "within_7_days_pct": None,
            "within_14_days_pct": None,
            "pearson_r": None,
            "spearman_r": None,
        }

    diff = model[mask] - ref[mask]
    abs_diff = np.abs(diff)
    result = {
        "n_cells": int(diff.size),
        "mean_bias_days": float(np.mean(diff)),
        "median_bias_days": float(np.median(diff)),
        "mae_days": float(np.mean(abs_diff)),
        "rmse_days": float(np.sqrt(np.mean(diff ** 2))),
        "within_3_days_pct": float(np.mean(abs_diff <= 3.0) * 100.0),
        "within_7_days_pct": float(np.mean(abs_diff <= 7.0) * 100.0),
        "within_14_days_pct": float(np.mean(abs_diff <= 14.0) * 100.0),
        "pearson_r": None,
        "spearman_r": None,
    }

    if diff.size >= 2:
        model_vals = model[mask]
        ref_vals = ref[mask]
        corr = np.corrcoef(model_vals, ref_vals)[0, 1]
        if np.isfinite(corr):
            result["pearson_r"] = float(corr)
        model_ranks = pd.Series(model_vals).rank(method="average").to_numpy()
        ref_ranks = pd.Series(ref_vals).rank(method="average").to_numpy()
        spearman = np.corrcoef(model_ranks, ref_ranks)[0, 1]
        if np.isfinite(spearman):
            result["spearman_r"] = float(spearman)
    return result


def format_metrics_text(default_metrics, ue_metrics):
    lines = [
        "Coarse 30 m paired metrics",
        f"Default RMSE: {default_metrics['rmse_days']:.2f} d" if default_metrics["rmse_days"] is not None else "Default RMSE: n/a",
        f"UE RMSE: {ue_metrics['rmse_days']:.2f} d" if ue_metrics["rmse_days"] is not None else "UE RMSE: n/a",
        f"Default MAE: {default_metrics['mae_days']:.2f} d" if default_metrics["mae_days"] is not None else "Default MAE: n/a",
        f"UE MAE: {ue_metrics['mae_days']:.2f} d" if ue_metrics["mae_days"] is not None else "UE MAE: n/a",
        f"Cells: {ue_metrics['n_cells']}",
        "Improvement map: |UE-ref| - |Default-ref|",
        "Negative values mean UE is better",
    ]
    return "\n".join(lines)


def plot_panel(
    output_path,
    ref_coarse,
    default_diff,
    ue_diff,
    ue_minus_default,
    improvement,
    obs_count,
    paired_mask,
    default_metrics,
    ue_metrics,
):
    fig, axes = plt.subplots(2, 3, figsize=(16, 10), constrained_layout=True)

    ref_show = np.where(ref_coarse != MELTOUT_NODATA, ref_coarse, np.nan)
    diff_lim = 30.0
    imp_lim = 15.0
    obs_show = np.where(obs_count != MELTOUT_NODATA, obs_count, np.nan)
    paired_show = np.where(paired_mask, 1.0, np.nan)

    plots = [
        (axes[0, 0], ref_show, "Reference Meltout DOY (30 m)", "viridis", (100, 180)),
        (axes[0, 1], np.where(default_diff != MELTOUT_NODATA, default_diff, np.nan), "Default - Reference (days)", "RdBu_r", (-diff_lim, diff_lim)),
        (axes[0, 2], np.where(ue_diff != MELTOUT_NODATA, ue_diff, np.nan), "UE - Reference (days)", "RdBu_r", (-diff_lim, diff_lim)),
        (axes[1, 0], np.where(ue_minus_default != MELTOUT_NODATA, ue_minus_default, np.nan), "UE - Default (days)", "RdBu_r", (-diff_lim, diff_lim)),
        (axes[1, 1], np.where(improvement != MELTOUT_NODATA, improvement, np.nan), "|UE-ref| - |Default-ref|", "RdBu", (-imp_lim, imp_lim)),
        (axes[1, 2], obs_show, "Merged Observation Count", "cividis", None),
    ]

    for ax, arr, title, cmap, clim in plots:
        img = ax.imshow(arr, cmap=cmap)
        if clim is not None:
            img.set_clim(*clim)
        ax.set_title(title)
        ax.set_xticks([])
        ax.set_yticks([])
        fig.colorbar(img, ax=ax, fraction=0.046, pad=0.04)

    axes[0, 0].contour(np.where(np.isfinite(paired_show), paired_show, 0.0), levels=[0.5], colors="white", linewidths=0.4)

    fig.suptitle("FSM2 Meltout Comparison on Common 30 m Support", fontsize=15)
    fig.text(
        0.73,
        0.07,
        format_metrics_text(default_metrics, ue_metrics),
        fontsize=10,
        family="monospace",
        va="bottom",
        ha="left",
        bbox=dict(facecolor="white", alpha=0.85, edgecolor="0.7"),
    )

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=200)
    plt.close(fig)


def write_metrics_files(output_dir, default_metrics, ue_metrics, meta):
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "fsm2_meltout_paired_coarse_metrics.csv"
    json_path = output_dir / "fsm2_meltout_paired_coarse_metrics.json"

    rows = [
        {"model": "FSM2_DefaultRadiation", **default_metrics},
        {"model": "FSM2_UEFluxCalibrated", **ue_metrics},
    ]

    header = list(rows[0].keys())
    with csv_path.open("w", encoding="utf-8") as f:
        f.write(",".join(header) + "\n")
        for row in rows:
            f.write(",".join("" if row[key] is None else str(row[key]) for key in header) + "\n")

    payload = {
        "meta": meta,
        "models": rows,
    }
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Saved metrics CSV: {csv_path}")
    print(f"Saved metrics JSON: {json_path}")


def main():
    parser = argparse.ArgumentParser(description="Paired coarse-grid meltout comparison: FSM2 default vs UE vs S2/Landsat.")
    parser.add_argument("--map-dir", help="Map directory under analysis_results/Maps/<MapName>.")
    parser.add_argument("--reference-tif", required=True, help="Merged S2/Landsat meltout raster.")
    parser.add_argument("--obs-count-tif", default=None, help="Observation-count QC raster (preferably merged-count in band 3).")
    parser.add_argument("--default-json", default=None, help="FSM2 default-radiation meltout JSON.")
    parser.add_argument("--ue-json", default=None, help="FSM2 UE-flux-calibrated meltout JSON.")
    parser.add_argument("--out-dir", required=True, help="Output directory.")
    parser.add_argument("--coarse-resolution", type=float, default=30.0, help="Target comparison support in meters.")
    parser.add_argument("--trim-border-pct", type=float, default=5.0, help="Border trim percent per side on the fine grid.")
    parser.add_argument("--min-valid-frac", type=float, default=0.7, help="Minimum valid fraction required inside each coarse block.")
    parser.add_argument("--min-obs-count", type=float, default=20.0, help="Minimum merged observation count required.")
    args = parser.parse_args()

    map_dir = Path(args.map_dir) if args.map_dir else None
    output_dir = Path(args.out_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if args.default_json:
        default_json = Path(args.default_json)
    else:
        if map_dir is None:
            raise FileNotFoundError("--default-json is required when --map-dir is omitted.")
        default_json = pick_latest_matching_json(map_dir, ["fsm2", "defaultradiation"])
        if default_json is None:
            default_json = pick_latest_matching_json(map_dir, ["fsm2_defaultradiation"])
    if args.ue_json:
        ue_json = Path(args.ue_json)
    else:
        if map_dir is None:
            raise FileNotFoundError("--ue-json is required when --map-dir is omitted.")
        ue_json = pick_latest_matching_json(map_dir, ["fsm2", "uefluxcalibrated"])

    if default_json is None or not default_json.exists():
        raise FileNotFoundError("Could not locate FSM2 default-radiation meltout JSON.")
    if ue_json is None or not ue_json.exists():
        raise FileNotFoundError("Could not locate FSM2 UE-flux-calibrated meltout JSON.")

    if args.obs_count_tif:
        obs_count_tif = Path(args.obs_count_tif)
    else:
        search_dir = Path(args.reference_tif).parent
        obs_count_tif = find_latest_file(search_dir, "SourceObservationCounts_*.tif")
    if obs_count_tif is not None and not obs_count_tif.exists():
        obs_count_tif = None

    ref_data, ref_nodata, ref_profile = load_reference_raster(args.reference_tif)
    obs_count_data = load_obs_count_raster(obs_count_tif)

    print(f"Reading default model meltout: {default_json}")
    default_data, _, _, default_profile = read_ue_export(str(default_json))
    print(f"Reading UE model meltout: {ue_json}")
    ue_data, _, _, ue_profile = read_ue_export(str(ue_json))

    print("Reprojecting model rasters to reference grid...")
    default_on_ref = reproject_to_profile(default_data, default_profile, ref_profile)
    ue_on_ref = reproject_to_profile(ue_data, ue_profile, ref_profile)

    trim_arrays = [ref_data, default_on_ref, ue_on_ref]
    if obs_count_data is not None:
        trim_arrays.append(obs_count_data)
    _, trim_y, trim_x = apply_border_trim_inplace(trim_arrays, nodata=MELTOUT_NODATA, trim_pct=args.trim_border_pct)
    if trim_y > 0 or trim_x > 0:
        print(
            f"Applied fine-grid trim before coarse aggregation: {args.trim_border_pct:.2f}% "
            f"(trim_y={trim_y}px, trim_x={trim_x}px)"
        )

    fine_res = abs(float(ref_profile["transform"].a))
    factor = int(round(args.coarse_resolution / fine_res))
    if factor <= 0 or not np.isclose(factor * fine_res, args.coarse_resolution, atol=1e-6):
        raise ValueError(
            f"Coarse resolution {args.coarse_resolution} m is not an integer multiple of "
            f"reference resolution {fine_res} m."
        )

    coarse_profile = build_coarse_profile(ref_profile, factor)
    coarse_bounds = array_bounds(coarse_profile["height"], coarse_profile["width"], coarse_profile["transform"])
    print(
        f"Aggregating to {args.coarse_resolution:.1f} m support "
        f"(factor={factor}, coarse_shape={coarse_profile['height']}x{coarse_profile['width']})"
    )
    print(f"Coarse bounds: {coarse_bounds}")

    ref_valid = valid_melt_mask(ref_data, nodata=ref_nodata)
    default_valid = valid_melt_mask(default_on_ref)
    ue_valid = valid_melt_mask(ue_on_ref)

    ref_coarse, ref_coverage = aggregate_blocks_median(ref_data, ref_valid, factor, args.min_valid_frac)
    default_coarse, default_coverage = aggregate_blocks_median(default_on_ref, default_valid, factor, args.min_valid_frac)
    ue_coarse, ue_coverage = aggregate_blocks_median(ue_on_ref, ue_valid, factor, args.min_valid_frac)

    if obs_count_data is not None:
        obs_valid = np.isfinite(obs_count_data) & (obs_count_data != MELTOUT_NODATA)
        obs_coarse = aggregate_blocks_mean(obs_count_data, obs_valid, factor, 0.0)
    else:
        obs_coarse = np.full_like(ref_coarse, MELTOUT_NODATA, dtype=np.float32)

    paired_mask = (
        (ref_coarse != MELTOUT_NODATA)
        & (default_coarse != MELTOUT_NODATA)
        & (ue_coarse != MELTOUT_NODATA)
        & (ref_coverage >= args.min_valid_frac)
        & (default_coverage >= args.min_valid_frac)
        & (ue_coverage >= args.min_valid_frac)
    )
    if obs_count_tif is not None:
        paired_mask &= np.isfinite(obs_coarse) & (obs_coarse >= args.min_obs_count)

    default_diff = np.full_like(ref_coarse, MELTOUT_NODATA, dtype=np.float32)
    ue_diff = np.full_like(ref_coarse, MELTOUT_NODATA, dtype=np.float32)
    ue_minus_default = np.full_like(ref_coarse, MELTOUT_NODATA, dtype=np.float32)
    improvement = np.full_like(ref_coarse, MELTOUT_NODATA, dtype=np.float32)

    default_diff[paired_mask] = default_coarse[paired_mask] - ref_coarse[paired_mask]
    ue_diff[paired_mask] = ue_coarse[paired_mask] - ref_coarse[paired_mask]
    ue_minus_default[paired_mask] = ue_coarse[paired_mask] - default_coarse[paired_mask]
    improvement[paired_mask] = np.abs(ue_diff[paired_mask]) - np.abs(default_diff[paired_mask])

    default_metrics = calculate_metrics(default_coarse, ref_coarse, paired_mask)
    ue_metrics = calculate_metrics(ue_coarse, ref_coarse, paired_mask)

    print("Paired coarse-grid metrics:")
    print(f"  Common coarse cells: {int(paired_mask.sum())}")
    print(f"  Default RMSE: {default_metrics['rmse_days']:.2f} d" if default_metrics["rmse_days"] is not None else "  Default RMSE: n/a")
    print(f"  UE RMSE: {ue_metrics['rmse_days']:.2f} d" if ue_metrics["rmse_days"] is not None else "  UE RMSE: n/a")
    print(f"  Default MAE: {default_metrics['mae_days']:.2f} d" if default_metrics["mae_days"] is not None else "  Default MAE: n/a")
    print(f"  UE MAE: {ue_metrics['mae_days']:.2f} d" if ue_metrics["mae_days"] is not None else "  UE MAE: n/a")

    write_float_tif(output_dir / "fsm2_default_minus_reference_30m.tif", default_diff, coarse_profile)
    write_float_tif(output_dir / "fsm2_ue_minus_reference_30m.tif", ue_diff, coarse_profile)
    write_float_tif(output_dir / "fsm2_ue_minus_default_30m.tif", ue_minus_default, coarse_profile)
    write_float_tif(output_dir / "fsm2_ue_vs_default_improvement_30m.tif", improvement, coarse_profile)
    write_float_tif(output_dir / "reference_meltout_30m.tif", ref_coarse, coarse_profile)
    write_float_tif(output_dir / "fsm2_default_meltout_30m.tif", default_coarse, coarse_profile)
    write_float_tif(output_dir / "fsm2_ue_meltout_30m.tif", ue_coarse, coarse_profile)
    if obs_count_tif is not None:
        write_float_tif(output_dir / "reference_obs_count_30m.tif", obs_coarse, coarse_profile)

    plot_panel(
        output_dir / "fsm2_meltout_paired_coarse_30m.png",
        ref_coarse,
        default_diff,
        ue_diff,
        ue_minus_default,
        improvement,
        obs_coarse,
        paired_mask,
        default_metrics,
        ue_metrics,
    )

    meta = {
        "reference_tif": str(Path(args.reference_tif).resolve()),
        "obs_count_tif": str(obs_count_tif.resolve()) if obs_count_tif is not None else None,
        "default_json": str(default_json.resolve()),
        "ue_json": str(ue_json.resolve()),
        "coarse_resolution_m": args.coarse_resolution,
        "fine_reference_resolution_m": fine_res,
        "aggregation_factor": factor,
        "min_valid_frac": args.min_valid_frac,
        "min_obs_count": args.min_obs_count if obs_count_tif is not None else None,
        "trim_border_pct": args.trim_border_pct,
        "paired_cells": int(paired_mask.sum()),
    }
    write_metrics_files(output_dir, default_metrics, ue_metrics, meta)
    print(f"Saved coarse paired analysis to {output_dir}")


if __name__ == "__main__":
    main()
