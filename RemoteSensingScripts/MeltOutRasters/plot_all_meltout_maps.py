"""
Plot spatial meltout maps for every model run found under a map directory.

For each meltout JSON discovered, produces a figure with:
  [Reference DOY] | [Model DOY] | [Model - Reference diff]
  + metrics text box and optional observation-count inset.

Usage:
  python plot_all_meltout_maps.py \
    --map-dir analysis_results/Maps/Totalp \
    --reference-tif analysis_results/Maps/Totalp/RS_Data/Meltout/Meltout_Reference_*.tif \
    --out-dir analysis_results/Maps/Totalp/Comparison/Meltout/Maps \
    [--obs-count-tif ...] \
    [--coarse-resolution 30.0] \
    [--trim-border-pct 0.0] \
    [--min-valid-frac 0.5] \
    [--min-obs-count 5]
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np
import pandas as pd

PLOT_RESULTS_SCRIPTS = Path(__file__).resolve().parents[3] / "plot-results" / "scripts"
if str(PLOT_RESULTS_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(PLOT_RESULTS_SCRIPTS))

try:
    from plot_map_context import apply_projected_map_context, extent_from_bounds
except ImportError:
    def extent_from_bounds(bounds, src_crs=None):
        left, bottom, right, top = bounds
        return (left, right, bottom, top)

    def apply_projected_map_context(ax, bounds, src_crs=None):
        ax.set_xlabel("Easting [m]")
        ax.set_ylabel("Northing [m]")

try:
    import matplotlib.pyplot as plt
    import matplotlib.gridspec as gridspec
    import rasterio
except ImportError as exc:
    raise SystemExit(f"Required dependency missing: {exc}")

from compare_fsm2_meltout_paired_coarse import (
    aggregate_blocks_median,
    aggregate_blocks_mean,
    build_coarse_profile,
    load_obs_count_raster,
    load_reference_raster,
    valid_melt_mask,
    calculate_metrics,
    find_latest_file,
)
from compare_meltout_rasters import (
    MELTOUT_NODATA,
    apply_border_trim_inplace,
    discover_meltout_json_files,
    read_ue_export,
    reproject_to_profile,
)

FONT_SIZE = 12
TITLE_FONT_SIZE = 14
SUPTITLE_FONT_SIZE = 16


# ---------------------------------------------------------------------------
# Label helpers
# ---------------------------------------------------------------------------

def _geom_tag_from_stem(stem: str) -> str:
    m = re.search(r'_(DynGeom|StatGeom)', stem)
    return m.group(1) if m else ""


def _model_label_from_json(json_path: Path) -> str:
    """Build a short, human-readable title from the JSON filename."""
    import json as _json
    stem = json_path.stem
    try:
        with json_path.open() as f:
            meta = _json.load(f)
        melt = meta.get("MeltModelTag", "")
        rad = meta.get("RadiationSchemeTag", "")
        base = f"{melt} / {rad}" if rad else melt
    except Exception:
        base = stem

    geom = _geom_tag_from_stem(stem)
    if not geom:
        geom = ""
    return f"{base} [{geom}]" if geom else base


def _safe_filename(label: str) -> str:
    return re.sub(r'[^A-Za-z0-9_\-]', '_', label).strip('_')


def _path_str(path: Path) -> str:
    return str(path).replace("\\", "/")


# ---------------------------------------------------------------------------
# Plotting
# ---------------------------------------------------------------------------

def format_metrics_text(metrics: dict, label: str) -> str:
    lines = [label]
    if metrics["rmse_days"] is not None:
        lines += [
            f"Bias:  {metrics['mean_bias_days']:+.2f} d",
            f"RMSE:  {metrics['rmse_days']:.2f} d",
            f"Pearson:{metrics['pearson_r']:.3f}" if metrics["pearson_r"] is not None else "Pearson:n/a",
            f"Spearman:{metrics['spearman_r']:.3f}" if metrics.get("spearman_r") is not None else "Spearman:n/a",
            f"n:     {metrics['n_cells']:,}",
            f"≤7d:   {metrics['within_7_days_pct']:.1f}%",
        ]
    else:
        lines.append("(no paired cells)")
    return "\n".join(lines)


def _compact_summary_label(label: str) -> str:
    lower = label.lower()
    model = _summary_model_key(label)

    if "dyngeom" in lower:
        geom = "Dyn"
    elif "statgeom" in lower:
        geom = "Stat"
    else:
        geom = "NA"

    return f"{model}\n{geom}"


def _summary_model_key(label: str) -> str:
    lower = label.lower()
    if "dd_hock2" in lower:
        return "H2"
    if "dd_hock3exact" in lower:
        return "H3"
    if "dd_pellicciottid" in lower:
        return "PelD"
    if "dd_pellicciottiue" in lower:
        return "PelUE"
    if "fsm2_uefluxcalibrated" in lower:
        return "F-UE"
    if "fsm2_defaultradiation" in lower:
        return "F-Def"
    return "Other"


def _summary_geom_key(label: str) -> str:
    lower = label.lower()
    if "dyngeom" in lower:
        return "Dyn"
    if "statgeom" in lower:
        return "Stat"
    return "NA"


def plot_meltout_map(
    output_path: Path,
    ref_coarse: np.ndarray,
    model_coarse: np.ndarray,
    diff: np.ndarray,
    obs_coarse: np.ndarray | None,
    paired_mask: np.ndarray,
    metrics: dict,
    title: str,
    map_bounds: tuple[float, float, float, float],
    doy_clim=(100, 213),
    diff_lim=30.0,
    src_crs=None,
):
    has_obs = obs_coarse is not None and np.any(obs_coarse != MELTOUT_NODATA)
    fig, axes = plt.subplots(2, 2, figsize=(13.5, 12.5), constrained_layout=False)
    fig.subplots_adjust(left=0.05, right=0.96, top=0.91, bottom=0.14, wspace=0.28, hspace=0.30)
    axes = [axes[0, 0], axes[0, 1], axes[1, 0], axes[1, 1]]

    ref_show = np.where(ref_coarse != MELTOUT_NODATA, ref_coarse, np.nan)
    model_show = np.where(model_coarse != MELTOUT_NODATA, model_coarse, np.nan)
    diff_show = np.where(diff != MELTOUT_NODATA, diff, np.nan)

    panels = [
        (axes[0], ref_show, "Reference Meltout DOY", "viridis", doy_clim),
        (axes[1], model_show, "Model Meltout DOY", "viridis", doy_clim),
        (axes[2], diff_show, "Model − Reference [days]", "RdBu_r", (-diff_lim, diff_lim)),
    ]
    if has_obs:
        obs_show = np.where(obs_coarse != MELTOUT_NODATA, obs_coarse, np.nan)
        panels.append((axes[3], obs_show, "Observation Count", "cividis", None))
    else:
        axes[3].axis("off")

    panel_extent = extent_from_bounds(map_bounds, src_crs=src_crs)
    for ax, arr, panel_title, cmap, clim in panels:
        img = ax.imshow(
            arr,
            cmap=cmap,
            interpolation="nearest",
            origin="upper",
            extent=panel_extent,
        )
        if clim is not None:
            img.set_clim(*clim)
        ax.set_title(panel_title, fontsize=TITLE_FONT_SIZE)
        apply_projected_map_context(ax, map_bounds, src_crs=src_crs)
        cbar = fig.colorbar(img, ax=ax, fraction=0.046, pad=0.04, shrink=0.85)
        cbar.ax.tick_params(labelsize=FONT_SIZE)
        ax.tick_params(axis="both", labelsize=FONT_SIZE)

    # Paired-mask contour on diff panel
    axes[2].contour(
        np.where(paired_mask, 1.0, 0.0),
        extent=panel_extent,
        levels=[0.5], colors="white", linewidths=0.4, alpha=0.6,
    )

    fig.suptitle(title, fontsize=SUPTITLE_FONT_SIZE, y=0.97)

    # Metrics text below panels
    metrics_txt = format_metrics_text(metrics, "Paired coarse metrics")
    fig.text(
        0.5, 0.02, metrics_txt,
        ha="center", va="bottom", fontsize=FONT_SIZE,
        family="monospace",
        bbox=dict(facecolor="white", alpha=0.85, edgecolor="0.7"),
    )

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=180, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved {output_path}")


# ---------------------------------------------------------------------------
# Summary stats plot
# ---------------------------------------------------------------------------

def plot_all_stats_summary(all_metrics: list[tuple[str, dict]], output_path: Path):
    """Multi-panel bar chart: Bias, RMSE, Pearson r, Spearman r, within-7d%, n."""
    import matplotlib.patches as mpatches
    import matplotlib.colors as mcolors

    entries = [(lbl, m) for lbl, m in all_metrics if m["n_cells"] > 0 and m["rmse_days"] is not None]
    if not entries:
        print("No valid metrics — skipping summary plot.")
        return

    labels = [lbl for lbl, _ in entries]
    summary_labels = [_compact_summary_label(lbl) for lbl in labels]
    model_keys = [_summary_model_key(lbl) for lbl in labels]
    geom_keys = [_summary_geom_key(lbl) for lbl in labels]
    biases  = [m["mean_bias_days"]      for _, m in entries]
    rmses   = [m["rmse_days"]           for _, m in entries]
    rs      = [m["pearson_r"] if m["pearson_r"] is not None else float("nan") for _, m in entries]
    spears  = [m["spearman_r"] if m.get("spearman_r") is not None else float("nan") for _, m in entries]
    w7      = [m["within_7_days_pct"]   for _, m in entries]
    ns      = [m["n_cells"]             for _, m in entries]

    x = np.arange(len(entries))
    bw = 0.65  # bar width

    fig, axes = plt.subplots(3, 2, figsize=(max(14, len(entries) * 1.45), 13.3), sharex=True)
    fig.subplots_adjust(hspace=0.16, wspace=0.34, left=0.07, right=0.97, top=0.91, bottom=0.10)

    model_palette = {
        "H2": "#8c564b",
        "H3": "#7f7f7f",
        "PelD": "#1b9e77",
        "PelUE": "#d95f02",
        "F-Def": "#7570b3",
        "F-UE": "#e7298a",
        "Other": "#666666",
    }

    def _mix_color(color, target, weight):
        rgb = np.array(mcolors.to_rgb(color))
        target_rgb = np.array(mcolors.to_rgb(target))
        return tuple((1.0 - weight) * rgb + weight * target_rgb)

    def _bar_color(model_key: str, geom_key: str):
        base = model_palette.get(model_key, model_palette["Other"])
        if geom_key == "Dyn":
            return _mix_color(base, "white", 0.43)
        if geom_key == "Stat":
            return _mix_color(base, "black", 0.08)
        return base

    bar_colors = [_bar_color(model, geom) for model, geom in zip(model_keys, geom_keys)]

    def _bar(ax, vals, ylabel, title, hline=None, fmt="{:.1f}", ylim=None, show_values=True):
        bars = ax.bar(x, vals, bw, color=bar_colors, alpha=0.92)
        if hline is not None:
            ax.axhline(hline, color="k", linewidth=0.8, linestyle="--")
        ax.set_ylabel(ylabel, fontsize=FONT_SIZE)
        ax.set_title(title, fontsize=TITLE_FONT_SIZE, pad=3)
        ax.tick_params(axis="both", labelsize=FONT_SIZE)
        ax.grid(axis="y", linestyle="--", alpha=0.4)
        if ylim:
            ax.set_ylim(*ylim)
        else:
            finite_vals = np.asarray([v for v in vals if np.isfinite(v)], dtype=float)
            if finite_vals.size:
                lo = min(float(finite_vals.min()), hline if hline is not None else float(finite_vals.min()))
                hi = max(float(finite_vals.max()), hline if hline is not None else float(finite_vals.max()))
                span_raw = hi - lo
                if span_raw <= 0:
                    span_raw = max(abs(hi) * 0.12, 1.0)
                pad = span_raw * 0.16
                lower = lo - pad
                upper = hi + pad
                if lo >= 0:
                    lower = 0
                ax.set_ylim(lower, upper)
        if show_values:
            for bar, val in zip(bars, vals):
                if not np.isfinite(val):
                    continue
                ymin, ymax = ax.get_ylim()
                span = ymax - ymin
                y_offset = max(span * 0.02, 0.03)
                y_pos = val + (y_offset if val >= 0 else -y_offset)
                va = "bottom" if val >= 0 else "top"
                ax.text(bar.get_x() + bar.get_width() / 2, y_pos,
                        fmt.format(val), ha="center", va=va, fontsize=FONT_SIZE - 1)
        return bars

    _bar(axes[0, 0], biases, "Bias [days]", "Mean Bias (model - ref)",
         hline=0, fmt="{:+.1f}")
    # 2. RMSE
    _bar(axes[0, 1], rmses, "RMSE [days]", "RMSE", fmt="{:.1f}")

    _bar(axes[1, 0], rs, "Pearson r", "Spatial Correlation (Pearson r)",
         hline=None, fmt="{:.3f}", ylim=(0, 1.05))
    # 4. Spearman r
    _bar(axes[1, 1], spears, "Spearman r", "Spatial Correlation (Spearman r)",
         hline=None, fmt="{:.3f}", ylim=(0, 1.05))

    # 5. Within 7 days %
    _bar(axes[2, 0], w7, "% cells", "Within 7 days (%)", fmt="{:.1f}",
         ylim=(0, 105))

    # 6. Cells used in each paired comparison
    _bar(axes[2, 1], ns, "cell count", "Cells used in comparison", fmt="{:,.0f}", show_values=False)

    model_handles = []
    seen_models = []
    for model in model_keys:
        if model in seen_models:
            continue
        seen_models.append(model)
        model_handles.append(mpatches.Patch(color=model_palette.get(model, model_palette["Other"]), label=model))
    shade_handles = [
        mpatches.Patch(color=_mix_color("0.5", "white", 0.43), label="DynGeom: lighter"),
        mpatches.Patch(color=_mix_color("0.5", "black", 0.08), label="StatGeom: darker"),
    ]
    fig.legend(
        handles=model_handles + shade_handles,
        loc="upper center",
        bbox_to_anchor=(0.5, 0.985),
        fontsize=FONT_SIZE - 2,
        frameon=True,
        framealpha=0.9,
        ncol=min(len(model_handles) + len(shade_handles), 7),
    )

    # Shared x-axis labels on bottom row
    for ax in axes[2, :]:
        ax.set_xticks(x)
        ax.set_xticklabels(summary_labels, rotation=0, ha="center", fontsize=FONT_SIZE - 1, linespacing=0.9)

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved {output_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Produce spatial meltout maps for every model run in a map directory."
    )
    parser.add_argument("--map-dir", required=True,
                        help="Map directory, e.g. analysis_results/Maps/Totalp")
    parser.add_argument("--reference-tif", required=True,
                        help="S2/Landsat merged meltout reference GeoTIFF.")
    parser.add_argument("--obs-count-tif", default=None,
                        help="Observation-count QC raster (optional).")
    parser.add_argument("--no-obs-count", action="store_true",
                        help="Disable observation-count loading, including automatic discovery.")
    parser.add_argument("--out-dir", required=True,
                        help="Output directory for PNG maps.")
    parser.add_argument("--coarse-resolution", type=float, default=30.0,
                        help="Coarse aggregation resolution in metres (default 30).")
    parser.add_argument("--trim-border-pct", type=float, default=0.0,
                        help="Border trim pct applied before aggregation.")
    parser.add_argument("--min-valid-frac", type=float, default=0.5,
                        help="Min fine-cell valid fraction per coarse block.")
    parser.add_argument("--min-obs-count", type=float, default=5.0,
                        help="Min observation count per coarse block (if obs raster provided).")
    parser.add_argument("--diff-lim", type=float, default=30.0,
                        help="Colour scale limit for diff maps (±days).")
    parser.add_argument("--groundeye-dem", default=None,
                        help="Path to GroundEye .dem file (ESRI ASCII 6-line header). "
                             "When given, pixels outside the DEM footprint are masked to nodata "
                             "before aggregation.")
    parser.add_argument("--metrics-csv", default=None,
                        help="Optional CSV path for per-run map-comparison metrics.")
    parser.add_argument("--json-files", nargs="*", default=None,
                        help="Optional explicit meltout JSON files. When provided, only these runs are plotted.")
    args = parser.parse_args()

    map_dir = Path(args.map_dir)
    output_dir = Path(args.out_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Reference raster
    ref_data, ref_nodata, ref_profile = load_reference_raster(args.reference_tif)

    # GroundEye AOI mask (applied to ref_data and obs in-place before aggregation)
    if args.groundeye_dem:
        from compare_meltout_rasters import parse_groundeye_dem_bounds, build_footprint_mask_from_bounds
        ge_bounds = parse_groundeye_dem_bounds(args.groundeye_dem)
        ge_outside = build_footprint_mask_from_bounds(ge_bounds, ref_profile, nodata=ref_nodata)
        ref_data = ref_data.copy()
        ref_data[ge_outside] = ref_nodata if ref_nodata is not None else MELTOUT_NODATA
        print(f"GroundEye mask applied: {(~ge_outside).sum()} / {ge_outside.size} ref pixels inside AOI")
    else:
        ge_outside = None

    # Obs count
    if args.no_obs_count:
        obs_count_data = None
    elif args.obs_count_tif:
        obs_count_data = load_obs_count_raster(args.obs_count_tif)
    else:
        search_dir = Path(args.reference_tif).parent
        obs_tif = find_latest_file(search_dir, "SourceObservationCounts_*.tif")
        obs_count_data = load_obs_count_raster(obs_tif) if obs_tif else None

    if obs_count_data is not None and ge_outside is not None:
        obs_count_data = obs_count_data.copy()
        obs_count_data[ge_outside] = MELTOUT_NODATA

    # Fine resolution & aggregation factor
    fine_res = abs(float(ref_profile["transform"].a))
    factor = int(round(args.coarse_resolution / fine_res))
    if factor <= 0:
        raise ValueError(
            f"coarse_resolution {args.coarse_resolution} m < fine resolution {fine_res} m"
        )
    print(f"Aggregation factor: {factor}  ({fine_res} m -> {factor * fine_res:.1f} m)")

    coarse_profile = build_coarse_profile(ref_profile, factor)
    from rasterio.transform import array_bounds
    coarse_bounds_raw = array_bounds(
        int(coarse_profile["height"]),
        int(coarse_profile["width"]),
        coarse_profile["transform"],
    )
    coarse_bounds = (
        float(coarse_bounds_raw[0]),
        float(coarse_bounds_raw[1]),
        float(coarse_bounds_raw[2]),
        float(coarse_bounds_raw[3]),
    )
    ref_crs = ref_profile.get("crs")

    # Pre-aggregate reference once
    ref_valid = valid_melt_mask(ref_data, nodata=ref_nodata)
    ref_coarse, ref_coverage = aggregate_blocks_median(
        ref_data, ref_valid, factor, args.min_valid_frac
    )

    # Obs count coarse
    if obs_count_data is not None:
        obs_valid = np.isfinite(obs_count_data) & (obs_count_data != MELTOUT_NODATA)
        obs_coarse = aggregate_blocks_mean(obs_count_data, obs_valid, factor, 0.0)
    else:
        obs_coarse = None

    # Discover all model JSONs, or use an explicit per-run subset.
    if args.json_files:
        json_files = [Path(p) for p in args.json_files]
    else:
        json_files = discover_meltout_json_files(map_dir)
    if not json_files:
        raise FileNotFoundError(f"No Meltout_*.json files found under {map_dir}")
    print(f"Found {len(json_files)} meltout JSON files.")

    all_metrics: list[tuple[str, dict]] = []
    metric_rows: list[dict] = []

    for ue_json in sorted(json_files):
        stem = ue_json.stem
        label = _model_label_from_json(ue_json)
        print(f"\n--- {label} ---")

        try:
            model_data, _, _, model_profile = read_ue_export(str(ue_json))
        except Exception as e:
            print(f"  ERROR reading {ue_json.name}: {e}")
            continue

        # Reproject model to reference grid
        model_on_ref = reproject_to_profile(model_data, model_profile, ref_profile)

        # Apply GroundEye AOI mask to model
        if ge_outside is not None:
            model_on_ref[ge_outside] = MELTOUT_NODATA

        # Border trim (copies, so ref_data stays intact each iteration)
        ref_trim = ref_data.copy()
        model_trim = model_on_ref.copy()
        trim_arrays = [ref_trim, model_trim]
        obs_trim = obs_count_data.copy() if obs_count_data is not None else None
        if obs_trim is not None:
            trim_arrays.append(obs_trim)

        if args.trim_border_pct > 0:
            apply_border_trim_inplace(trim_arrays, nodata=MELTOUT_NODATA, trim_pct=args.trim_border_pct)

        # Aggregate model
        model_valid = valid_melt_mask(model_trim)
        model_coarse, model_coverage = aggregate_blocks_median(
            model_trim, model_valid, factor, args.min_valid_frac
        )

        # Paired mask
        paired_mask = (
            (ref_coarse != MELTOUT_NODATA)
            & (model_coarse != MELTOUT_NODATA)
            & (ref_coverage >= args.min_valid_frac)
            & (model_coverage >= args.min_valid_frac)
        )
        if obs_coarse is not None:
            valid_obs = np.isfinite(obs_coarse) & (obs_coarse >= args.min_obs_count)
            paired_mask &= valid_obs

        # Diff
        diff = np.full_like(ref_coarse, MELTOUT_NODATA, dtype=np.float32)
        diff[paired_mask] = model_coarse[paired_mask] - ref_coarse[paired_mask]

        metrics = calculate_metrics(model_coarse, ref_coarse, paired_mask)
        if metrics["rmse_days"] is not None:
            print(f"  n={metrics['n_cells']}, bias={metrics['mean_bias_days']:+.2f}d, "
                  f"RMSE={metrics['rmse_days']:.2f}d, r={metrics['pearson_r']:.3f}"
                  if metrics["pearson_r"] is not None
                  else f"  n={metrics['n_cells']}, bias={metrics['mean_bias_days']:+.2f}d, RMSE={metrics['rmse_days']:.2f}d")
        else:
            print(f"  n={metrics['n_cells']} (no valid pairs)")

        all_metrics.append((label, metrics))

        metric_rows.append({
            "label": label,
            "meltout_json": _path_str(ue_json),
            "meltout_png": _path_str(ue_json.with_suffix(".png")),
            "n_cells": metrics.get("n_cells"),
            "mean_bias_days": metrics.get("mean_bias_days"),
            "rmse_days": metrics.get("rmse_days"),
            "mae_days": metrics.get("mae_days"),
            "pearson_r": metrics.get("pearson_r"),
            "spearman_r": metrics.get("spearman_r"),
            "within_7_days_pct": metrics.get("within_7_days_pct"),
            "within_14_days_pct": metrics.get("within_14_days_pct"),
        })

        safe_name = _safe_filename(stem)
        out_png = output_dir / f"{safe_name}_meltout_map.png"

        plot_meltout_map(
            output_path=out_png,
            ref_coarse=ref_coarse,
            model_coarse=model_coarse,
            diff=diff,
            obs_coarse=obs_coarse,
            paired_mask=paired_mask,
            metrics=metrics,
            title=f"Meltout Comparison — {label}",
            map_bounds=coarse_bounds,
            doy_clim=(100, 213),
            diff_lim=args.diff_lim,
            src_crs=ref_crs,
        )

    if all_metrics:
        plot_all_stats_summary(
            all_metrics,
            output_path=output_dir / "meltout_stats_summary.png",
        )

    metrics_csv = Path(args.metrics_csv) if args.metrics_csv else output_dir / "meltout_map_comparison_metrics.csv"
    metrics_csv.parent.mkdir(parents=True, exist_ok=True)
    pd.DataFrame(metric_rows).to_csv(metrics_csv, index=False)
    print(f"Saved metrics CSV {metrics_csv}")

    print("\nDone.")


if __name__ == "__main__":
    main()
