import argparse
import sys
import json
import os
from pathlib import Path
import numpy as np

try:
    import rasterio
    from rasterio.warp import calculate_default_transform, reproject, Resampling
    from rasterio.transform import Affine, array_bounds
except ImportError:
    print("Error: 'rasterio' is required. Please install it.")
    sys.exit(1)

DEFAULT_MELTOUT_DOY_START = 100
DEFAULT_MELTOUT_DOY_END = 213  # Aug 1 for non-leap-year simulations
MELTOUT_NODATA = -9999.0
MELTOUT_UNRESOLVED = 9999.0


def build_doy_range(start_doy, end_doy):
    if end_doy < start_doy:
        raise ValueError(f"Invalid DOY range: start ({start_doy}) must be <= end ({end_doy}).")
    return np.arange(start_doy, end_doy + 1)


DEFAULT_MELTOUT_DOY_RANGE = build_doy_range(DEFAULT_MELTOUT_DOY_START, DEFAULT_MELTOUT_DOY_END)


def discover_meltout_json_files(search_root):
    search_root = Path(search_root)

    direct_files = sorted(search_root.glob("Meltout_*.json"))
    if direct_files:
        return direct_files

    model_folder_files = []
    if search_root.is_dir():
        for child in sorted(search_root.iterdir()):
            if not child.is_dir():
                continue
            if child.name in {"Comparison", "Plots", "RS_Data", "Terrain"}:
                continue
            meltout_dir = child / "Meltout"
            if meltout_dir.is_dir():
                model_folder_files.extend(sorted(meltout_dir.glob("Meltout_*.json")))
    if model_folder_files:
        return model_folder_files

    recursive_files = [
        path for path in search_root.rglob("Meltout_*.json")
        if "Comparison" not in path.parts and "Plots" not in path.parts
    ]
    return sorted(recursive_files)


def parse_groundeye_dem_bounds(dem_path):
    """
    Read a 6-line ESRI ASCII raster header from a GroundEye .dem file and return
    the footprint as (xmin, ymin, xmax, ymax) in EPSG:2056.

    Old Swiss LV03 coordinates (x < 2_000_000 or y < 1_000_000) are automatically
    shifted to LV95/EPSG:2056 by adding the standard offsets.
    """
    header = {}
    with open(dem_path, "r", encoding="utf-8") as f:
        for _ in range(6):
            parts = next(f).split()
            header[parts[0].lower()] = float(parts[1])

    ncols = int(header["ncols"])
    nrows = int(header["nrows"])
    xll   = header["xllcorner"]
    yll   = header["yllcorner"]
    cs    = header["cellsize"]

    if xll < 2_000_000:
        xll += 2_000_000
    if yll < 1_000_000:
        yll += 1_000_000

    xmin, ymin = xll, yll
    xmax, ymax = xll + ncols * cs, yll + nrows * cs
    print(f"GroundEye footprint (EPSG:2056): X=[{xmin:.0f}, {xmax:.0f}]  Y=[{ymin:.0f}, {ymax:.0f}]  ({ncols}×{nrows} @ {cs}m)")
    return xmin, ymin, xmax, ymax


def build_footprint_mask_from_bounds(bounds, profile, nodata=-9999):
    """
    Return a boolean mask (True = outside footprint) aligned to *profile*.
    Pixels whose cell centres fall outside *bounds* (xmin,ymin,xmax,ymax) are True.
    Applies the mask in-place is not the job of this function; the caller does that.
    """
    from rasterio.transform import rowcol
    xmin, ymin, xmax, ymax = bounds
    transform = profile["transform"]
    height    = profile["height"]
    width     = profile["width"]

    # Build arrays of cell-centre coordinates
    cols = np.arange(width,  dtype=np.float64)
    rows = np.arange(height, dtype=np.float64)
    xs = transform.c + (cols + 0.5) * transform.a          # easting  of each column centre
    ys = transform.f + (rows + 0.5) * transform.e          # northing of each row centre (e < 0)

    xs_2d = xs[np.newaxis, :]   # shape (1, width)  → broadcast
    ys_2d = ys[:, np.newaxis]   # shape (height, 1) → broadcast

    outside = (xs_2d < xmin) | (xs_2d > xmax) | (ys_2d < ymin) | (ys_2d > ymax)
    n_outside = int(outside.sum())
    n_total   = height * width
    print(f"GroundEye footprint mask: {n_total - n_outside}/{n_total} pixels inside "
          f"({100.0 * (n_total - n_outside) / n_total:.1f}%)")
    return outside


def apply_border_trim_inplace(arrays, nodata=-9999, trim_pct=0.0):
    """
    Sets a border strip to nodata for each provided array (in-place).
    trim_pct is per side (e.g. 5.0 trims 5% from each side).
    """
    if trim_pct <= 0.0:
        return None, 0, 0
    if trim_pct >= 50.0:
        raise ValueError(f"trim_pct must be < 50, got {trim_pct}")

    shape = None
    for arr in arrays:
        if arr is None:
            continue
        if shape is None:
            shape = arr.shape
        elif arr.shape != shape:
            raise ValueError(f"All arrays must share shape for border trim. Got {shape} and {arr.shape}.")
    if shape is None:
        return None, 0, 0

    h, w = shape
    trim_y = int(np.floor(h * (trim_pct / 100.0)))
    trim_x = int(np.floor(w * (trim_pct / 100.0)))
    if trim_y <= 0 and trim_x <= 0:
        return None, 0, 0
    if trim_y * 2 >= h or trim_x * 2 >= w:
        raise ValueError(
            f"trim_pct={trim_pct} removes entire raster for shape {shape} (trim_y={trim_y}, trim_x={trim_x})."
        )

    border = np.zeros(shape, dtype=bool)
    if trim_y > 0:
        border[:trim_y, :] = True
        border[-trim_y:, :] = True
    if trim_x > 0:
        border[:, :trim_x] = True
        border[:, -trim_x:] = True

    for arr in arrays:
        if arr is not None:
            arr[border] = nodata

    return border, trim_y, trim_x

def _bounds_intersection_and_coverage(bounds_a, bounds_b):
    """
    Returns intersection area and coverage fractions for two bounds tuples:
    (left, bottom, right, top).
    """
    left = max(bounds_a[0], bounds_b[0])
    bottom = max(bounds_a[1], bounds_b[1])
    right = min(bounds_a[2], bounds_b[2])
    top = min(bounds_a[3], bounds_b[3])
    inter_w = max(0.0, right - left)
    inter_h = max(0.0, top - bottom)
    inter_area = inter_w * inter_h

    area_a = max(0.0, bounds_a[2] - bounds_a[0]) * max(0.0, bounds_a[3] - bounds_a[1])
    area_b = max(0.0, bounds_b[2] - bounds_b[0]) * max(0.0, bounds_b[3] - bounds_b[1])

    cov_a = inter_area / area_a if area_a > 0 else 0.0
    cov_b = inter_area / area_b if area_b > 0 else 0.0
    return inter_area, cov_a, cov_b


def _profile_bounds(profile):
    """Returns (left, bottom, right, top) from a raster profile."""
    return array_bounds(profile["height"], profile["width"], profile["transform"])


def reproject_to_profile(src_data, src_profile, dst_profile):
    """
    Reprojects source array/profile to an explicit destination profile.
    """
    destination = np.full((dst_profile["height"], dst_profile["width"]), -9999, dtype=np.float32)
    reproject(
        source=src_data,
        destination=destination,
        src_transform=src_profile["transform"],
        src_crs=src_profile["crs"],
        dst_transform=dst_profile["transform"],
        dst_crs=dst_profile["crs"],
        resampling=Resampling.nearest,
        src_nodata=src_profile.get("nodata", -9999),
        dst_nodata=-9999
    )
    return destination


def read_optional_status_png(png_path):
    status_path = Path(png_path).with_name(f"{Path(png_path).stem}_Status.png")
    if not status_path.exists():
        return None
    with rasterio.open(status_path) as src:
        return src.read(1)


def resolve_ue_cell_size_meters(meta):
    """
    Resolve the UE meltout raster cell size in meters.

    Legacy meltout exports wrote the actor's grid stride (`CellSize`) instead of
    metric cell size. Terrain/SVF exports already write meters, and newer
    meltout exports should provide `CellSizeMeters` explicitly.
    """
    if "CellSizeMeters" in meta:
        return float(meta["CellSizeMeters"])

    cell_size = float(meta.get("CellSize", 1.0))
    scale_x_cm = meta.get("ScaleX")
    scale_y_cm = meta.get("ScaleY")
    if scale_x_cm is not None or scale_y_cm is not None:
        scales_cm = [
            abs(float(value))
            for value in (scale_x_cm, scale_y_cm)
            if value is not None
        ]
        if scales_cm:
            scale_m = sum(scales_cm) / (100.0 * len(scales_cm))
            resolved = cell_size * scale_m
            print(
                "Resolved legacy UE meltout cell size: "
                f"raw CellSize={cell_size}, actor scale={scale_m:.6f} m -> {resolved:.6f} m"
            )
            return resolved

    return cell_size


def resolve_ue_pixel_layout(meta):
    layout = str(meta.get("PixelLayoutTransform", "")).strip().lower()
    if layout:
        if layout in {"identity", "north_up_identity"}:
            return "identity"
        if layout in {"transpose", "swap_xy"}:
            return "transpose"
        print(
            "WARNING: Unknown UE pixel layout "
            f"'{meta.get('PixelLayoutTransform')}'. Falling back to Rotated90 metadata."
        )

    rot90 = bool(meta.get("bRotated90", meta.get("Rotated90", False)))
    return "transpose" if rot90 else "identity"


def read_ue_export(json_path, png_path=None, override_origin=None, invert_y=False):
    """
    Reads the UE export JSON and PNG, returning the data array and profile.
    """
    with open(json_path, 'r') as f:
        meta = json.load(f)
        
    if png_path is None:
        # Assume PNG is same name as JSON
        png_path = os.path.splitext(json_path)[0] + ".png"
        
    if not os.path.exists(png_path):
        raise FileNotFoundError(f"UE PNG file not found: {png_path}")
        
    # Read PNG using rasterio (avoids PIL dependency)
    with rasterio.open(png_path) as src:
        raw_data = src.read(1)
    status_data = read_optional_status_png(png_path)
        
    # Convert to DOY
    doy_scale = meta.get('DOYValueScale', 1.0)
    data = np.full(raw_data.shape, MELTOUT_NODATA, dtype=np.float32)
    curve_data = np.full(raw_data.shape, MELTOUT_UNRESOLVED, dtype=np.float32)
    curve_valid_mask = np.ones(raw_data.shape, dtype=bool)

    if status_data is not None:
        no_snow_code = int(meta.get('MeltoutStatusNoSnowCode', 1))
        resolved_code = int(meta.get('MeltoutStatusResolvedCode', 2))
        unresolved_code = int(meta.get('MeltoutStatusUnresolvedCode', 3))
        curve_valid_mask = status_data > 0
        resolved_mask = (status_data == resolved_code) & (raw_data > 0)
        no_snow_mask = status_data == no_snow_code
        unresolved_mask = status_data == unresolved_code
        curve_data[no_snow_mask] = 0.0
        curve_data[resolved_mask] = raw_data[resolved_mask].astype(np.float32) / doy_scale
        curve_data[~curve_valid_mask] = MELTOUT_NODATA
        data[resolved_mask] = curve_data[resolved_mask]
        data[unresolved_mask] = MELTOUT_NODATA
    else:
        resolved_mask = raw_data > 0
        data[resolved_mask] = raw_data[resolved_mask].astype(np.float32) / doy_scale
        curve_data[resolved_mask] = data[resolved_mask]
    
    # Construct Transform
    cell_size = resolve_ue_cell_size_meters(meta)
    
    # Use Projected Origin if available, otherwise Local Origin (which is likely 0,0)
    origin_x = meta.get('ProjectedOriginX', meta.get('OriginX', 0.0))
    origin_y = meta.get('ProjectedOriginY', meta.get('OriginY', 0.0))
    
    if override_origin:
        origin_x, origin_y = override_origin
        print(f"Overriding UE Origin to: ({origin_x}, {origin_y})")
        
    if origin_x == 0 and origin_y == 0:
        print("WARNING: UE Origin is (0,0). Alignment with S2 raster may fail.")
        
    # Affine(scale_x, shear_x, trans_x, shear_y, scale_y, trans_y)
    scale_y = -cell_size if not invert_y else cell_size
    transform = Affine(cell_size, 0.0, origin_x, 0.0, scale_y, origin_y)
    
    crs = meta.get('Projection', 'EPSG:32632') # Default to UTM32N if unknown, but warn
    if crs == 'UE_Local':
        print("WARNING: UE Projection is 'UE_Local'. Assuming EPSG:32632 (Finse).")
        crs = 'EPSG:32632'
        
    pixel_layout = resolve_ue_pixel_layout(meta)
    if pixel_layout == "transpose":
        print("Detected UE transpose layout. Transposing raster...")
        data = data.T
        curve_data = curve_data.T
        curve_valid_mask = curve_valid_mask.T
    elif meta.get("PixelLayoutTransform"):
        print(f"Detected UE pixel layout: {meta['PixelLayoutTransform']}. Using raster as-is.")
    
    profile = {
        'driver': 'GTiff',
        'dtype': 'float32',
        'nodata': -9999,
        'width': data.shape[1],
        'height': data.shape[0],
        'count': 1,
        'crs': crs,
        'transform': transform
    }
    
    bounds = array_bounds(profile['height'], profile['width'], profile['transform'])
    print(
        "UE Raster Bounds: "
        f"Left={bounds[0]}, Bottom={bounds[1]}, Right={bounds[2]}, Top={bounds[3]}"
    )
    print(f"UE Raster CRS: {crs}")
    
    return data, curve_data, curve_valid_mask, profile

def reproject_to_match(src_data, src_profile, match_path):
    """
    Reprojects the source array/profile to match the match_path raster.
    """
    with rasterio.open(match_path) as match_ds:
        match_crs = match_ds.crs
        match_transform = match_ds.transform
        match_width = match_ds.width
        match_height = match_ds.height
        match_profile = match_ds.profile.copy()

    destination = reproject_to_profile(
        src_data,
        src_profile,
        {
            "height": match_height,
            "width": match_width,
            "transform": match_transform,
            "crs": match_crs,
            "nodata": -9999,
        }
    )
    return destination, match_profile

def calculate_stats(diff_array, nodata=-9999):
    """Calculates basic statistics for the difference array.
    Returns (count, mean_bias, rmse) or (0, None, None) if no valid pixels."""
    valid_mask = (diff_array != nodata)
    valid_data = diff_array[valid_mask]

    if valid_data.size == 0:
        print("No valid pixels for comparison.")
        return 0, None, None

    count = valid_data.size
    mean_bias = np.mean(valid_data)
    rmse = np.sqrt(np.mean(valid_data**2))
    min_val = np.min(valid_data)
    max_val = np.max(valid_data)

    print("-" * 30)
    print(f"Comparison Statistics:")
    print(f"Valid Pixels: {count}")
    print(f"Mean Bias (UE - S2): {mean_bias:.2f}")
    print(f"RMSE: {rmse:.2f}")
    print(f"Min Diff: {min_val:.2f}")
    print(f"Max Diff: {max_val:.2f}")
    print("-" * 30)
    return count, mean_bias, rmse

def create_vis_raster(diff_array, nodata=-9999, min_diff=-20, max_diff=20):
    """
    Creates a visualization raster normalized to [0, 255].
    """
    # Initialize with 0 (nodata)
    vis_array = np.zeros_like(diff_array, dtype=np.uint8)
    
    valid_mask = (diff_array != nodata)
    valid_data = diff_array[valid_mask]
    
    # Clip to range
    clipped = np.clip(valid_data, min_diff, max_diff)
    
    # Normalize to [0, 1]
    normalized = (clipped - min_diff) / (max_diff - min_diff)
    
    # Scale to [1, 255]
    scaled = (normalized * 254.0) + 1.0
    
    vis_array[valid_mask] = scaled.astype(np.uint8)
    
    return vis_array

def save_as_png16(data, path, scale=1.0, offset=0.0, nodata=-9999):
    """
    Saves the data as a 16-bit grayscale PNG.
    Value = (Data + offset) * scale
    """
    # Create mask
    valid_mask = (data != nodata)
    
    # Prepare output array
    out_data = np.zeros_like(data, dtype=np.uint16)
    
    if valid_mask.any():
        # Apply transform
        transformed = (data[valid_mask] + offset) * scale
        
        # Clamp to uint16 range
        transformed = np.clip(transformed, 0, 65535)
        
        out_data[valid_mask] = transformed.astype(np.uint16)
        
    # Write using rasterio with PNG driver
    # We use a dummy profile since PNG doesn't support all GeoTIFF tags, 
    # but rasterio handles the format conversion.
    height, width = data.shape
    profile = {
        'driver': 'PNG',
        'dtype': 'uint16',
        'width': width,
        'height': height,
        'count': 1
    }
    
    with rasterio.open(path, 'w', **profile) as dst:
        dst.write(out_data, 1)
        
    print(f"Saved 16-bit PNG to {path} (Scale={scale}, Offset={offset})")

def compute_meltout_curve(data, valid_mask, doy_range=None):
    """Calculate cumulative melt-out curve (snow-free pixels vs DOY)."""
    if doy_range is None:
        doy_range = DEFAULT_MELTOUT_DOY_RANGE
    valid_data = data[valid_mask]
    total_pixels = len(valid_data)
    counts = []
    for doy in doy_range:
        counts.append(np.sum(valid_data <= doy))
    return np.array(counts, dtype=np.float32), total_pixels


def counts_to_percent(counts, total_pixels):
    if total_pixels <= 0:
        return np.zeros_like(counts, dtype=np.float32)
    return (counts.astype(np.float32) / float(total_pixels)) * 100.0


def plot_combined_meltout_progression(model_curves, output_dir, prefix=None, doy_range=None):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("Error: matplotlib required for combined melt-out plot. Skipping.")
        return

    if doy_range is None:
        doy_range = DEFAULT_MELTOUT_DOY_RANGE

    output_dir = ensure_plot_type_dir(output_dir, "overview")

    plt.figure(figsize=(15.5, 6))
    reference_pct_curves = []
    for label, model_counts, model_total, reference_counts, reference_total, *_ in model_curves:
        model_pct = counts_to_percent(model_counts, model_total)
        reference_pct = counts_to_percent(reference_counts, reference_total)
        reference_pct_curves.append(reference_pct)
        plt.plot(doy_range, model_pct, linewidth=1.8, label=label)

    if reference_pct_curves:
        reference_stack = np.vstack(reference_pct_curves)
        reference_min = np.min(reference_stack, axis=0)
        reference_max = np.max(reference_stack, axis=0)
        reference_mean = np.mean(reference_stack, axis=0)
        plt.fill_between(
            doy_range,
            reference_min,
            reference_max,
            color="0.5",
            alpha=0.18,
            label="S2/Landsat paired-overlap range",
        )
        plt.plot(
            doy_range,
            reference_mean,
            color="k",
            linestyle="--",
            linewidth=2.0,
            label="S2/Landsat reference",
        )

    plt.xlabel('Day of Year (DOY)')
    plt.ylabel('Snow-Free Area (%)')
    plt.title('Melt-out Progression (Models + S2/Landsat Reference)')
    plt.grid(True)
    plt.xlim(doy_range[0], doy_range[-1])
    plt.ylim(0, 100)
    plt.legend(
        loc="upper center",
        bbox_to_anchor=(0.5, -0.12),
        ncol=3,
        fontsize=8,
        frameon=False
    )
    plt.tight_layout()

    filename = f"{prefix}_meltout_progression_all_models.png" if prefix else "meltout_progression_all_models.png"
    output_path = output_dir / filename
    plt.savefig(output_path, bbox_inches="tight")
    print(f"Saved {output_path}")
    plt.close()


def plot_model_stats_summary(model_curves, output_dir, prefix=None):
    """Bar chart of Bias and RMSE for each model run."""
    try:
        import matplotlib.pyplot as plt
        import matplotlib.patches as mpatches
    except ImportError:
        print("Error: matplotlib required for stats summary plot. Skipping.")
        return

    entries = [
        (label, stats_bias, stats_rmse, stats_count)
        for label, _mc, _mt, _rc, _rt, stats_count, stats_bias, stats_rmse in model_curves
        if stats_bias is not None
    ]
    if not entries:
        return

    output_dir = ensure_plot_type_dir(output_dir, "overview")

    labels = [e[0] for e in entries]
    biases = [e[1] for e in entries]
    rmses = [e[2] for e in entries]
    counts = [e[3] for e in entries]

    # Shorten labels: strip common map prefix and trailing timestamp
    def shorten(lbl):
        # Remove leading map tag (e.g. "Totalp_") if present
        for sep in ("_DD_", "_FSM2_"):
            idx = lbl.find(sep)
            if idx >= 0:
                lbl = lbl[idx + 1:]
                break
        # Drop trailing _YYYYMMDD_HHMMSS
        import re
        lbl = re.sub(r'_\d{8}_\d{6}$', '', lbl)
        return lbl

    short_labels = [shorten(l) for l in labels]

    x = np.arange(len(entries))
    bar_width = 0.35

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(max(10, len(entries) * 1.1), 8), sharex=True)

    # Bias plot
    colors = ['tab:green' if abs(b) <= 5 else ('tab:orange' if abs(b) <= 15 else 'tab:red') for b in biases]
    bars1 = ax1.bar(x, biases, bar_width * 2, color=colors, alpha=0.8)
    ax1.axhline(0, color='k', linewidth=0.8)
    ax1.set_ylabel('Mean Bias (UE − S2) [days]')
    ax1.set_title('Meltout Model Comparison — Bias & RMSE (full domain)')
    ax1.grid(axis='y', linestyle='--', alpha=0.5)
    for bar, val in zip(bars1, biases):
        ax1.text(bar.get_x() + bar.get_width() / 2, val + (0.3 if val >= 0 else -0.8),
                 f'{val:+.1f}', ha='center', va='bottom' if val >= 0 else 'top', fontsize=7.5)

    # RMSE plot
    bars2 = ax2.bar(x, rmses, bar_width * 2, color='steelblue', alpha=0.8)
    ax2.set_ylabel('RMSE [days]')
    ax2.grid(axis='y', linestyle='--', alpha=0.5)
    ax2.set_xticks(x)
    ax2.set_xticklabels(short_labels, rotation=35, ha='right', fontsize=8)
    for bar, val, n in zip(bars2, rmses, counts):
        ax2.text(bar.get_x() + bar.get_width() / 2, val + 0.2,
                 f'{val:.1f}\n(n={n:,})', ha='center', va='bottom', fontsize=7)

    legend_patches = [
        mpatches.Patch(color='tab:green', alpha=0.8, label='|Bias| ≤ 5 days'),
        mpatches.Patch(color='tab:orange', alpha=0.8, label='5 < |Bias| ≤ 15 days'),
        mpatches.Patch(color='tab:red', alpha=0.8, label='|Bias| > 15 days'),
    ]
    ax1.legend(handles=legend_patches, fontsize=8, loc='upper right')

    plt.tight_layout()
    filename = f"{prefix}_stats_summary.png" if prefix else "stats_summary.png"
    output_path = output_dir / filename
    plt.savefig(output_path, bbox_inches="tight", dpi=150)
    print(f"Saved {output_path}")
    plt.close()


PLOT_TYPE_DIRS = {
    "diff": "Diff",
    "vis": "Vis",
    "s2": "S2",
    "histogram": "Histogram",
    "scatter": "Scatter",
    "meltout_curve": "MeltoutCurve",
    "bias_vs_elevation": "BiasVsElevation",
    "overview": "Overview",
}


def ensure_plot_type_dir(output_dir, plot_type):
    base = Path(output_dir)
    target = base / PLOT_TYPE_DIRS[plot_type]
    target.mkdir(parents=True, exist_ok=True)
    return target


def resolve_model_label(meta, fallback, stem=None):
    """Build a short human-readable label for a model run.

    Priority order for the base name:
      1. Explicit label fields in JSON metadata
      2. MeltModelTag + RadiationSchemeTag from metadata
      3. Raw stem (filename without extension)

    Geometry tag (DynGeom / StatGeom) is appended when found in either
    the JSON metadata (SnowGeometryTag) or the filename stem.
    """
    import re

    # --- base label ---
    label = None
    for key in ("MeltModelLabel", "MeltModelName", "DegreeDayMeltModel", "MeltModel"):
        value = meta.get(key)
        if value is not None and str(value).strip():
            label = str(value)
            break

    if label is None:
        melt_tag = meta.get("MeltModelTag", "")
        rad_tag = meta.get("RadiationSchemeTag", "")
        if melt_tag:
            label = f"{melt_tag} / {rad_tag}" if rad_tag else melt_tag
        else:
            label = fallback

    # --- geometry tag ---
    geom_tag = meta.get("SnowGeometryTag")  # present in newer exports
    if not geom_tag and stem:
        # Extract from filename: look for _DynGeom or _StatGeom
        m = re.search(r'_(DynGeom|StatGeom)', stem)
        if m:
            geom_tag = m.group(1)

    if geom_tag:
        label = f"{label} [{geom_tag}]"

    return label


def run_comparison(s2_data, s2_nodata, s2_profile, s2_path, ue_json, ue_png, output_dir,
                   override_origin=None, invert_y=False, dem_tif=None, ue_slope_png=None,
                   ue_aspect_png=None, baseline_png=None, return_curve=False, doy_range=None,
                   comparison_grid="ue", fail_on_extent_mismatch=False, trim_border_pct=0.0,
                   groundeye_dem=None):
    ue_json = Path(ue_json)
    ue_png = Path(ue_png) if ue_png else ue_json.with_suffix(".png")
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    stem = ue_json.stem
    out_diff_tif = ensure_plot_type_dir(output_dir, "diff") / f"{stem}_diff.tif"
    out_vis_tif = ensure_plot_type_dir(output_dir, "vis") / f"{stem}_vis.tif"
    out_diff_png = ensure_plot_type_dir(output_dir, "diff") / f"{stem}_diff.png"
    out_vis_png = ensure_plot_type_dir(output_dir, "vis") / f"{stem}_vis.png"
    out_s2_png = ensure_plot_type_dir(output_dir, "s2") / f"{stem}_s2.png"
    out_baseline_diff_tif = ensure_plot_type_dir(output_dir, "diff") / f"{stem}_baseline_diff.tif"
    out_baseline_vis_tif = ensure_plot_type_dir(output_dir, "vis") / f"{stem}_baseline_vis.tif"
    out_baseline_diff_png = ensure_plot_type_dir(output_dir, "diff") / f"{stem}_baseline_diff.png"
    out_baseline_vis_png = ensure_plot_type_dir(output_dir, "vis") / f"{stem}_baseline_vis.png"

    print(f"Reading UE export: {ue_json}")
    ue_data, ue_curve_data, ue_curve_valid_mask, ue_profile = read_ue_export(
        str(ue_json), str(ue_png), override_origin, invert_y
    )

    s2_bounds = _profile_bounds(s2_profile)
    ue_bounds = _profile_bounds(ue_profile)
    if str(s2_profile.get("crs")) == str(ue_profile.get("crs")):
        inter_area, s2_cov, ue_cov = _bounds_intersection_and_coverage(s2_bounds, ue_bounds)
        print(f"Grid overlap diagnostics: intersection={inter_area:.2f}, ref_coverage={s2_cov:.3f}, ue_coverage={ue_cov:.3f}")
        mismatch = (s2_cov < 0.99) or (ue_cov < 0.99)
        if mismatch:
            msg = (
                "Extent mismatch detected between reference and UE rasters. "
                f"Reference coverage by overlap={s2_cov:.3f}, UE coverage by overlap={ue_cov:.3f}."
            )
            if fail_on_extent_mismatch:
                raise ValueError(msg)
            print(f"WARNING: {msg}")
    else:
        print(
            f"WARNING: CRS mismatch (Reference={s2_profile.get('crs')}, UE={ue_profile.get('crs')}). "
            "Extent diagnostics are skipped."
        )

    if comparison_grid == "ue":
        print("Reprojecting reference raster to UE grid...")
        s2_compare = reproject_to_profile(s2_data, s2_profile, ue_profile)
        ue_compare = ue_data
        ue_curve_compare = ue_curve_data
        ue_curve_valid_compare = ue_curve_valid_mask.copy()
        compare_profile = ue_profile.copy()
        compare_s2_nodata = -9999
    else:
        print("Reprojecting UE raster to reference grid...")
        ue_compare, _ = reproject_to_match(ue_data, ue_profile, s2_path)
        ue_curve_compare = reproject_to_profile(ue_curve_data, ue_profile, s2_profile)
        ue_curve_valid_compare = reproject_to_profile(
            ue_curve_valid_mask.astype(np.float32), ue_profile, s2_profile
        ) > 0.5
        s2_compare = s2_data.copy()
        compare_profile = s2_profile.copy()
        compare_s2_nodata = s2_nodata

    ue_baseline_compare = None
    ue_baseline_curve_compare = None
    if baseline_png:
        print(f"Reading Baseline UE export: {baseline_png}")
        base_png = Path(baseline_png)
        base_json = base_png.with_suffix(".json")
        found_base_json = base_json if base_json.exists() else ue_json
        ue_base_data, ue_base_curve_data, _, ue_base_profile = read_ue_export(
            str(found_base_json), str(base_png), override_origin, invert_y
        )
        if comparison_grid == "ue":
            if (
                ue_base_data.shape != ue_data.shape
                or ue_base_profile.get("transform") != ue_profile.get("transform")
                or str(ue_base_profile.get("crs")) != str(ue_profile.get("crs"))
            ):
                print("Reprojecting Baseline UE raster to UE comparison grid...")
                ue_baseline_compare = reproject_to_profile(ue_base_data, ue_base_profile, ue_profile)
                ue_baseline_curve_compare = reproject_to_profile(ue_base_curve_data, ue_base_profile, ue_profile)
            else:
                ue_baseline_compare = ue_base_data
                ue_baseline_curve_compare = ue_base_curve_data
        else:
            print("Reprojecting Baseline UE raster to reference grid...")
            ue_baseline_compare, _ = reproject_to_match(ue_base_data, ue_base_profile, s2_path)
            ue_baseline_curve_compare = reproject_to_profile(ue_base_curve_data, ue_base_profile, s2_profile)

    target_nodata = -9999
    border_mask, trim_y, trim_x = apply_border_trim_inplace(
        [s2_compare, ue_compare, ue_baseline_compare],
        nodata=target_nodata,
        trim_pct=trim_border_pct
    )
    if border_mask is not None:
        ue_curve_compare[border_mask] = MELTOUT_NODATA
        ue_curve_valid_compare[border_mask] = False
        if ue_baseline_curve_compare is not None:
            ue_baseline_curve_compare[border_mask] = MELTOUT_NODATA
    if border_mask is not None:
        trimmed = int(border_mask.sum())
        total = int(border_mask.size)
        print(
            f"Applied border trim: {trim_border_pct:.2f}% per side "
            f"(trim_y={trim_y}px, trim_x={trim_x}px, trimmed_pixels={trimmed}/{total})."
        )

    if groundeye_dem is not None:
        ge_bounds = parse_groundeye_dem_bounds(groundeye_dem)
        ge_outside = build_footprint_mask_from_bounds(ge_bounds, compare_profile, nodata=target_nodata)
        for arr in [s2_compare, ue_compare, ue_baseline_compare]:
            if arr is not None:
                arr[ge_outside] = target_nodata
        ue_curve_compare[ge_outside] = MELTOUT_NODATA
        ue_curve_valid_compare[ge_outside] = False
        if ue_baseline_curve_compare is not None:
            ue_baseline_curve_compare[ge_outside] = MELTOUT_NODATA


    s2_mask = (s2_compare == target_nodata)
    if compare_s2_nodata is not None and compare_s2_nodata != target_nodata:
        s2_mask |= (s2_compare == compare_s2_nodata)

    ue_mask = (ue_compare == target_nodata)
    combined_mask = s2_mask | ue_mask
    curve_mask = (~s2_mask) & ue_curve_valid_compare

    diff = np.full_like(s2_compare, target_nodata, dtype=np.float32)
    valid_indices = ~combined_mask
    diff[valid_indices] = ue_compare[valid_indices] - s2_compare[valid_indices]

    print("--- Main Model Stats ---")
    _stats_count, _stats_bias, _stats_rmse = calculate_stats(diff, target_nodata)

    out_profile = compare_profile.copy()
    out_profile.update({
        'dtype': 'float32', 'count': 1, 'nodata': target_nodata, 'compress': 'lzw'
    })
    with rasterio.open(out_diff_tif, 'w', **out_profile) as dst:
        dst.write(diff, 1)

    print("Creating visualization raster ([-20, 20] days -> [1, 255])...")
    vis_data = create_vis_raster(diff, target_nodata, min_diff=-20, max_diff=20)

    vis_profile = compare_profile.copy()
    vis_profile.update({ 'dtype': 'uint8', 'count': 1, 'nodata': 0, 'compress': 'lzw' })
    with rasterio.open(out_vis_tif, 'w', **vis_profile) as dst:
        dst.write(vis_data, 1)

    save_as_png16(diff, out_diff_png, scale=100.0, offset=327.68, nodata=target_nodata)
    vis_float = vis_data.astype(np.float32)
    save_as_png16(vis_float, out_vis_png, scale=257.0, offset=0.0, nodata=0)

    if ue_baseline_compare is not None:
        ue_base_mask = (ue_baseline_compare == target_nodata)
        combined_base_mask = s2_mask | ue_base_mask

        diff_base = np.full_like(s2_compare, target_nodata, dtype=np.float32)
        valid_base_indices = ~combined_base_mask
        diff_base[valid_base_indices] = ue_baseline_compare[valid_base_indices] - s2_compare[valid_base_indices]

        print("--- Baseline Model Stats ---")
        calculate_stats(diff_base, target_nodata)

        with rasterio.open(out_baseline_diff_tif, 'w', **out_profile) as dst:
            dst.write(diff_base, 1)

        vis_base_data = create_vis_raster(diff_base, target_nodata, min_diff=-20, max_diff=20)
        with rasterio.open(out_baseline_vis_tif, 'w', **vis_profile) as dst:
            dst.write(vis_base_data, 1)

        save_as_png16(diff_base, out_baseline_diff_png, scale=100.0, offset=327.68, nodata=target_nodata)
        vis_base_float = vis_base_data.astype(np.float32)
        save_as_png16(vis_base_float, out_baseline_vis_png, scale=257.0, offset=0.0, nodata=0)

    with ue_json.open('r') as f:
        meta = json.load(f)
    doy_scale = meta.get('DOYValueScale', 179.057)
    save_as_png16(s2_compare, out_s2_png, scale=doy_scale, offset=0.0, nodata=target_nodata)

    generate_plots(
        s2_compare,
        ue_compare,
        dem_tif,
        target_nodata,
        ue_baseline_compare,
        curve_valid_mask=curve_mask,
        ue_curve_data=ue_curve_compare,
        ue_baseline_curve_data=ue_baseline_curve_compare,
        output_dir=output_dir,
        prefix=stem,
        doy_range=doy_range
    )

    if ue_slope_png and ue_aspect_png:
        if comparison_grid != "reference":
            print("WARNING: Stratified slope/aspect analysis currently requires --comparison_grid reference. Skipping.")
        else:
            perform_stratified_analysis(
                s2_compare,
                ue_compare,
                ue_slope_png,
                ue_aspect_png,
                ue_profile,
                s2_path,
                target_nodata,
                ue_baseline_compare,
                output_dir=output_dir,
                prefix=stem
            )

    if return_curve:
        label = resolve_model_label(meta, stem, stem=stem)
        valid_mask = curve_mask
        model_counts, model_total = compute_meltout_curve(ue_curve_compare, valid_mask, doy_range)
        reference_counts, reference_total = compute_meltout_curve(s2_compare, valid_mask, doy_range)
        return label, model_counts, model_total, reference_counts, reference_total, _stats_count, _stats_bias, _stats_rmse


def main():
    parser = argparse.ArgumentParser(description="Compare UE and reference melt-out rasters")
    parser.add_argument('--s2_meltout_tif', help="Reference melt-out GeoTIFF (defaults to latest in analysis_results/Meltout/GEE)")
    parser.add_argument('--ue_meltout_json', help="UE Export JSON Metadata")
    parser.add_argument('--ue_meltout_png', help="UE Export PNG (optional, inferred from JSON if omitted)")
    parser.add_argument('--ue_meltout_dir', help="Directory of UE meltout JSON files to compare")
    parser.add_argument('--out_dir', default='analysis_results/Plots/Meltouts', help="Output directory for plots/rasters")
    parser.add_argument('--out_diff_tif', help="Output Difference GeoTIFF")
    parser.add_argument('--out_vis_tif', help="Output Visualization GeoTIFF (8-bit)")
    parser.add_argument('--out_diff_png', help="Output Difference 16-bit PNG")
    parser.add_argument('--out_vis_png', help="Output Visualization 16-bit PNG")
    parser.add_argument('--out_s2_png', help="Output reference 16-bit PNG")
    parser.add_argument('--dem_tif', help="Elevation GeoTIFF for bias analysis (must match reference grid)")
    parser.add_argument('--ue_slope_png', help="UE Slope PNG (16-bit, scaled *100)")
    parser.add_argument('--ue_aspect_png', help="UE Aspect PNG (16-bit, scaled *100)")
    parser.add_argument('--ue_origin_x', type=float, help="Override UE Projected Origin X (Easting)")
    parser.add_argument('--ue_origin_y', type=float, help="Override UE Projected Origin Y (Northing)")
    parser.add_argument('--invert_y', action='store_true', help="Invert Y axis (treat as North-Up with negative scale)")
    parser.add_argument('--ue_meltout_baseline_png', help="Baseline UE Meltout PNG for comparative analysis")
    parser.add_argument('--out_baseline_diff_tif', help="Output Baseline Difference GeoTIFF")
    parser.add_argument('--out_baseline_vis_tif', help="Output Baseline Visualization GeoTIFF (8-bit)")
    parser.add_argument('--out_baseline_diff_png', help="Output Baseline Difference 16-bit PNG")
    parser.add_argument('--out_baseline_vis_png', help="Output Baseline Visualization 16-bit PNG")
    parser.add_argument('--doy_start', type=int, default=DEFAULT_MELTOUT_DOY_START, help="First DOY shown in melt-out curves.")
    parser.add_argument('--doy_end', type=int, default=DEFAULT_MELTOUT_DOY_END, help="Last DOY shown in melt-out curves.")
    parser.add_argument('--trim_border_pct', type=float, default=0.0, help="Trim this percent from each raster edge before comparison.")
    parser.add_argument(
        '--comparison_grid',
        choices=['ue', 'reference'],
        default='ue',
        help="Grid used for comparison outputs. 'ue' avoids partial-coverage artifacts when domains differ."
    )
    parser.add_argument(
        '--fail_on_extent_mismatch',
        action='store_true',
        help="Abort comparison when reference and UE extents do not substantially overlap."
    )
    parser.add_argument(
        '--groundeye_dem',
        default=None,
        help=(
            "Path to a GroundEye .dem file (ESRI ASCII, 6-line header). "
            "When provided, the comparison is restricted to the dem footprint, "
            "masking all pixels outside that rectangle to nodata. "
            "Default: full domain (no masking)."
        )
    )

    args = parser.parse_args()
    doy_range = build_doy_range(args.doy_start, args.doy_end)

    groundeye_dem = args.groundeye_dem

    def find_latest_file(directory, pattern):
        base = Path(directory)
        if not base.exists():
            return None
        matches = list(base.glob(pattern))
        if not matches:
            return None
        matches.sort(key=lambda p: p.stat().st_mtime, reverse=True)
        return matches[0]

    if not args.s2_meltout_tif:
        latest_s2 = find_latest_file("analysis_results/Meltout/GEE", "*.tif")
        if not latest_s2:
            print("Error: --s2_meltout_tif is required (no reference GeoTIFF found in analysis_results/Meltout/GEE).")
            sys.exit(1)
        args.s2_meltout_tif = str(latest_s2)
        print(f"Using latest reference GeoTIFF: {args.s2_meltout_tif}")

    print(f"Reading reference raster: {args.s2_meltout_tif}")
    with rasterio.open(args.s2_meltout_tif) as s2_ds:
        s2_data = s2_ds.read(1)
        s2_nodata = s2_ds.nodata
        s2_profile = s2_ds.profile
        print(f"S2 Raster Bounds: {s2_ds.bounds}")
        print(f"S2 Raster CRS: {s2_ds.crs}")
    if (s2_nodata is None or s2_nodata != -9999) and np.any(s2_data == -9999):
        print("Detected -9999 fill values in reference raster. Treating -9999 as nodata.")
        s2_nodata = -9999

    override_origin = (args.ue_origin_x, args.ue_origin_y) if args.ue_origin_x is not None else None

    if not args.ue_meltout_dir and not args.ue_meltout_json:
        default_dir = Path("analysis_results/Meltout")
        if default_dir.exists():
            args.ue_meltout_dir = str(default_dir)
            print(f"Using default UE meltout directory: {args.ue_meltout_dir}")

    if args.ue_meltout_dir:
        meltout_dir = Path(args.ue_meltout_dir)
        json_files = discover_meltout_json_files(meltout_dir)
        if not json_files:
            print(f"No meltout JSON files found in {meltout_dir}")
            sys.exit(1)

        model_curves = []
        for ue_json in json_files:
            result = run_comparison(
                s2_data,
                s2_nodata,
                s2_profile,
                args.s2_meltout_tif,
                ue_json,
                None,
                args.out_dir,
                override_origin=override_origin,
                invert_y=args.invert_y,
                dem_tif=args.dem_tif,
                ue_slope_png=args.ue_slope_png,
                ue_aspect_png=args.ue_aspect_png,
                baseline_png=args.ue_meltout_baseline_png,
                return_curve=True,
                doy_range=doy_range,
                comparison_grid=args.comparison_grid,
                fail_on_extent_mismatch=args.fail_on_extent_mismatch,
                trim_border_pct=args.trim_border_pct,
                groundeye_dem=groundeye_dem,
            )
            if result:
                model_curves.append(result)

        if model_curves:
            plot_combined_meltout_progression(
                model_curves,
                output_dir=args.out_dir,
                prefix=meltout_dir.name,
                doy_range=doy_range
            )
            plot_model_stats_summary(
                model_curves,
                output_dir=args.out_dir,
                prefix=meltout_dir.name,
            )

        print("Done.")
        return

    if not args.ue_meltout_json:
        latest_ue = find_latest_file("analysis_results/Meltout", "Meltout_*.json")
        if not latest_ue:
            print("Error: --ue_meltout_json is required unless --ue_meltout_dir is provided.")
            sys.exit(1)
        args.ue_meltout_json = str(latest_ue)
        print(f"Using latest UE meltout JSON: {args.ue_meltout_json}")

    run_comparison(
        s2_data,
        s2_nodata,
        s2_profile,
        args.s2_meltout_tif,
        args.ue_meltout_json,
        args.ue_meltout_png,
        args.out_dir,
        override_origin=override_origin,
        invert_y=args.invert_y,
        dem_tif=args.dem_tif,
        ue_slope_png=args.ue_slope_png,
        ue_aspect_png=args.ue_aspect_png,
        baseline_png=args.ue_meltout_baseline_png,
        return_curve=False,
        doy_range=doy_range,
        comparison_grid=args.comparison_grid,
        fail_on_extent_mismatch=args.fail_on_extent_mismatch,
        trim_border_pct=args.trim_border_pct,
        groundeye_dem=groundeye_dem,
    )

    print("Done.")

def generate_plots(
    s2_data,
    ue_data,
    dem_path=None,
    nodata=-9999,
    ue_baseline_data=None,
    curve_valid_mask=None,
    ue_curve_data=None,
    ue_baseline_curve_data=None,
    output_dir='analysis_results/Plots/Meltouts',
    prefix=None,
    doy_range=None,
):
    try:
        import matplotlib.pyplot as plt
        from scipy.stats import gaussian_kde
    except ImportError:
        print("Error: matplotlib/scipy required for plotting. Skipping.")
        return

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    def plot_path(name, plot_type):
        target_dir = ensure_plot_type_dir(output_dir, plot_type)
        if prefix:
            return target_dir / f"{prefix}_{name}"
        return target_dir / name
    
    mask = (s2_data != nodata) & (ue_data != nodata)
    if not mask.any():
        print("No common valid pixels for plotting.")
        return
        
    s2_valid = s2_data[mask]
    ue_valid = ue_data[mask]
    diff_valid = ue_valid - s2_valid
    
    # 1. Histogram Comparison
    plt.figure(figsize=(10, 6))
    plt.hist(s2_valid, bins=50, alpha=0.5, label='Reference (Satellite)', density=True)
    plt.hist(ue_valid, bins=50, alpha=0.5, label='UE Simulation', density=True)
    plt.xlabel('Melt-out Day of Year')
    plt.ylabel('Density')
    plt.title('Melt-out Date Distribution')
    plt.legend()
    histogram_path = plot_path('histogram_comparison.png', 'histogram')
    plt.savefig(histogram_path)
    print(f"Saved {histogram_path}")
    plt.close()
    
    # 2. Scatter Density Plot
    # Downsample for scatter if too many points (plotting millions is slow)
    if len(s2_valid) > 10000:
        indices = np.random.choice(len(s2_valid), 10000, replace=False)
        x = s2_valid[indices]
        y = ue_valid[indices]
    else:
        x = s2_valid
        y = ue_valid
        
    xy = np.vstack([x, y])
    z = gaussian_kde(xy)(xy)
    
    plt.figure(figsize=(8, 8))
    plt.scatter(x, y, c=z, s=5, cmap='viridis')
    plt.plot([0, 365], [0, 365], 'r--') # 1:1 line
    plt.xlabel('Reference DOY')
    plt.ylabel('UE Simulation DOY')
    plt.title('Melt-out Date Scatter Plot')
    plt.colorbar(label='Density')
    plt.xlim(100, 250) # Zoom to typical melt season
    plt.ylim(100, 250)
    plt.grid(True)
    scatter_path = plot_path('scatter_plot.png', 'scatter')
    plt.savefig(scatter_path)
    print(f"Saved {scatter_path}")
    plt.close()
    
    # 3. Melt-out Curve (Snow-Free Area % vs Day on fixed comparison mask)
    if doy_range is None:
        doy_range = DEFAULT_MELTOUT_DOY_RANGE

    curve_mask = curve_valid_mask if curve_valid_mask is not None else mask
    curve_source = ue_curve_data if ue_curve_data is not None else ue_data

    s2_counts, s2_total = compute_meltout_curve(s2_data, curve_mask, doy_range)
    ue_counts, ue_total = compute_meltout_curve(curve_source, curve_mask, doy_range)
    s2_pct = counts_to_percent(s2_counts, s2_total)
    ue_pct = counts_to_percent(ue_counts, ue_total)
    
    base_pct = None
    if ue_baseline_data is not None:
        baseline_curve_source = ue_baseline_curve_data if ue_baseline_curve_data is not None else ue_baseline_data
        base_counts, base_total = compute_meltout_curve(baseline_curve_source, curve_mask, doy_range)
        base_pct = counts_to_percent(base_counts, base_total)
        
    plt.figure(figsize=(10, 6))
    plt.plot(doy_range, s2_pct, 'k-', linewidth=2, label='Reference (Satellite)')
    plt.plot(doy_range, ue_pct, 'b-', linewidth=2, label='Enhanced (UE RI)')
    
    if base_pct is not None:
        plt.plot(doy_range, base_pct, 'r--', linewidth=2, label='Baseline (DD Only)')
        
    plt.xlabel('Day of Year (DOY)')
    plt.ylabel('Snow-Free Area (%)')
    plt.title(f'Melt-out Progression (Valid Overlap Pixels: {s2_total:,})')
    plt.legend()
    plt.grid(True)
    plt.xlim(doy_range[0], doy_range[-1])
    plt.ylim(0, 100)
    meltout_curve_path = plot_path('meltout_curve.png', 'meltout_curve')
    plt.savefig(meltout_curve_path)
    print(f"Saved {meltout_curve_path}")
    plt.close()
    
    # 3. Bias vs Elevation
    if dem_path:
        with rasterio.open(dem_path) as src:
            # We assume DEM is already aligned or we interpret it casually.
            # Ideally we should reproject DEM to match S2 grid.
            # For header check:
            if src.width != s2_data.shape[1] or src.height != s2_data.shape[0]:
                print("DEM dimensions do not match S2. Skipping Elevation Analysis.")
                return
            dem_data = src.read(1)
            
        dem_valid = dem_data[mask]
        
        # Bin by elevation (every 50m)
        elev_bins = np.arange(1000, 1900, 50) # Finse is around 1222m, higher slopes up to 1800m
        mean_bias = []
        bin_centers = []
        
        for i in range(len(elev_bins)-1):
            bin_mask = (dem_valid >= elev_bins[i]) & (dem_valid < elev_bins[i+1])
            if bin_mask.any():
                mean_bias.append(np.mean(diff_valid[bin_mask]))
                bin_centers.append((elev_bins[i] + elev_bins[i+1])/2)
                
        plt.figure(figsize=(10, 6))
        plt.plot(bin_centers, mean_bias, 'o-')
        plt.xlabel('Elevation (m)')
        plt.ylabel('Mean Bias (UE - S2) [Days]')
        plt.title('Melt-out Bias vs Elevation')
        plt.grid(True)
        plt.axhline(0, color='r', linestyle='--')
        bias_elev_path = plot_path('bias_vs_elevation.png', 'bias_vs_elevation')
        plt.savefig(bias_elev_path)
        print(f"Saved {bias_elev_path}")
        plt.close()


    

def perform_stratified_analysis(s2_data, ue_data, slope_path, aspect_path, ue_profile, ref_tif, nodata=-9999, ue_baseline_data=None, output_dir='analysis_results/Plots/Meltouts', prefix=None):
    """
    V2: Uses the already derived UE profile to reproject slope/aspect.
    Supports comparative analysis if ue_baseline_data is provided.
    """
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("Error: matplotlib required for stratified analysis. Skipping.")
        return
    
    print("Reading and Reprojecting Terrain Derivatives...")
    
    # Read raw PNGs
    def read_raw_png(path, rot90):
        with rasterio.open(path) as ds:
            arr = ds.read(1)
        if rot90:
            arr = arr.T
        return arr.astype(np.float32) / 100.0
    
    # Assume terrain export shared same settings (Rotated90) as UE
    # Ideally we'd read terrain metadata, but user workflow implies they match.
    rot90 = True 
    
    slope_raw = read_raw_png(slope_path, rot90)
    aspect_raw = read_raw_png(aspect_path, rot90) 
    
    # Reproject
    slope_reproj, _ = reproject_to_match(slope_raw, ue_profile, ref_tif)
    aspect_reproj, _ = reproject_to_match(aspect_raw, ue_profile, ref_tif)
    
    # Calculate Bias/RMSE
    # Mask: S2 valid AND UE valid AND Baseline valid (if exists)
    mask = (s2_data != nodata) & (ue_data != nodata)
    if ue_baseline_data is not None:
        mask &= (ue_baseline_data != nodata)

    if not mask.any(): return
    
    diff_main = ue_data[mask] - s2_data[mask]
    diff_base = None
    if ue_baseline_data is not None:
        diff_base = ue_baseline_data[mask] - s2_data[mask]

    flat_slope = slope_reproj[mask]
    flat_aspect = aspect_reproj[mask]
    
    # 1. Bias vs Slope / RMSE vs Slope
    slope_bins = np.arange(0, 90, 5)
    slope_centers = []
    
    # Metrics Lists
    main_bias = []
    main_rmse = []
    base_bias = []
    base_rmse = []
    
    for i in range(len(slope_bins)-1):
        bidx = (flat_slope >= slope_bins[i]) & (flat_slope < slope_bins[i+1])
        if bidx.any():
            slope_centers.append((slope_bins[i] + slope_bins[i+1])/2)
            
            # Main Model
            chunk_main = diff_main[bidx]
            main_bias.append(np.mean(chunk_main))
            main_rmse.append(np.sqrt(np.mean(chunk_main**2)))
            
            # Baseline Model
            if diff_base is not None:
                chunk_base = diff_base[bidx]
                base_bias.append(np.mean(chunk_base))
                base_rmse.append(np.sqrt(np.mean(chunk_base**2)))
            
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    def plot_path(name):
        if prefix:
            return output_dir / f"{prefix}_{name}"
        return output_dir / name

    # Plot Bias vs Slope (Comparison if baseline exists)
    plt.figure(figsize=(10, 6))
    plt.plot(slope_centers, main_bias, 'o-', color='tab:blue', label='Enhanced (UE Rad)' if diff_base is not None else 'Mean Bias')
    if diff_base is not None:
        plt.plot(slope_centers, base_bias, 's--', color='tab:gray', label='Baseline (DD Only)')
    
    plt.axhline(0, color='r', linestyle='--')
    plt.xlabel('Slope (degrees)')
    plt.ylabel('Mean Bias (UE - S2) [Days]')
    plt.title('Melt-out Bias vs Slope')
    plt.legend()
    plt.grid(True)
    plt.savefig(plot_path('bias_vs_slope.png'))
    plt.close()
    
    if diff_base is not None:
         # Plot Delta RMSE vs Slope
         # Delta RMSE = RMSE_baseline - RMSE_main (Positive = Improvement)
         delta_rmse = np.array(base_rmse) - np.array(main_rmse)
         
         plt.figure(figsize=(10, 6))
         plt.bar(slope_centers, delta_rmse, width=4, color='tab:green', alpha=0.7)
         plt.xlabel('Slope (degrees)')
         plt.ylabel('Delta RMSE (Days) [Positive = Improvement]')
         plt.title('RMSE Improvement by Slope (Baseline - Enhanced)')
         plt.axhline(0, color='k', linewidth=0.5)
         plt.grid(True)
         plt.savefig(plot_path('delta_rmse_vs_slope.png'))
         plt.close()

    # 2. Bias vs Aspect
    aspect_bins = np.arange(0, 361, 45)
    aspect_labels = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW']
    aspect_centers = []
    
    # Metrics
    asp_main_bias = []
    asp_main_rmse = []
    asp_base_bias = []
    asp_base_rmse = []
    
    for i in range(len(aspect_bins)-1):
        bidx = (flat_aspect >= aspect_bins[i]) & (flat_aspect < aspect_bins[i+1])
        if bidx.any():
            aspect_centers.append((aspect_bins[i] + aspect_bins[i+1])/2)
            
            chunk_main = diff_main[bidx]
            asp_main_bias.append(np.mean(chunk_main))
            asp_main_rmse.append(np.sqrt(np.mean(chunk_main**2)))
            
            if diff_base is not None:
                chunk_base = diff_base[bidx]
                asp_base_bias.append(np.mean(chunk_base))
                asp_base_rmse.append(np.sqrt(np.mean(chunk_base**2)))
            
    # Filter labels to match valid centers
    valid_aspect_labels = []
    for i in range(len(aspect_bins)-1):
        center = (aspect_bins[i] + aspect_bins[i+1])/2
        if center in aspect_centers:
            valid_aspect_labels.append(aspect_labels[i])

    plt.figure(figsize=(10, 6))
    if diff_base is None:
        plt.bar(aspect_centers, asp_main_bias, width=40, color='tab:orange', alpha=0.7)
    else:
        # Side-by-side bars for comparison
        width = 15
        x = np.array(aspect_centers)
        plt.bar(x - width/2, asp_base_bias, width, label='Baseline', color='tab:gray')
        plt.bar(x + width/2, asp_main_bias, width, label='Enhanced', color='tab:blue')
        plt.legend()
        
    plt.axhline(0, color='r', linestyle='--')
    plt.xlabel('Aspect (degrees)')
    plt.ylabel('Mean Bias (UE - S2) [Days]')
    plt.title('Melt-out Bias vs Aspect')
    plt.xticks(aspect_centers, valid_aspect_labels)
    plt.grid(True, axis='y')
    plt.savefig(plot_path('bias_vs_aspect.png'))
    plt.close()
    
    if diff_base is not None:
         # Delta RMSE vs Aspect
         delta_rmse_asp = np.array(asp_base_rmse) - np.array(asp_main_rmse)
         
         plt.figure(figsize=(10, 6))
         plt.bar(aspect_centers, delta_rmse_asp, width=40, color='tab:green', alpha=0.7)
         plt.xlabel('Aspect (degrees)')
         plt.ylabel('Delta RMSE (Days) [Positive = Improvement]')
         plt.title('RMSE Improvement by Aspect (Baseline - Enhanced)')
         plt.xticks(aspect_centers, valid_aspect_labels)
         plt.axhline(0, color='k', linewidth=0.5)
         plt.grid(True, axis='y')
         plt.savefig(plot_path('delta_rmse_vs_aspect.png'))
         plt.close()
    


    print(f"Stratified analysis complete. plots saved to {output_dir}")

if __name__ == "__main__":
    main()
