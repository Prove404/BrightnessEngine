import argparse
import io
import subprocess
import time
import zipfile
from pathlib import Path

import ee
import matplotlib.pyplot as plt
import numpy as np
import requests
import rasterio

from s2_meltout_base import (
    authenticate_and_initialize,
    load_aoi,
    build_s2_collection,
    build_landsat_collection,
    get_export_params,
)


def _download_raster(url, out_dir, stem, timeout_seconds=180, max_retries=4):
    out_dir.mkdir(parents=True, exist_ok=True)
    last_err = None
    content = None
    request_timeout = (30, None) if timeout_seconds <= 0 else timeout_seconds
    for attempt in range(1, max_retries + 1):
        try:
            resp = requests.get(url, timeout=request_timeout)
            resp.raise_for_status()
            content = resp.content
            break
        except Exception as exc:
            last_err = exc
            if attempt < max_retries:
                time.sleep(2 * attempt)
    if content is None:
        tmp_path = out_dir / f"{stem}.download"
        cmd = [
            "curl.exe",
            "--fail",
            "--location",
            "--silent",
            "--show-error",
            "--output",
            str(tmp_path),
            url,
        ]
        try:
            subprocess.run(cmd, check=True)
            content = tmp_path.read_bytes()
        except Exception as curl_exc:
            raise RuntimeError(
                f"Failed to download QC raster after retries: {last_err}; curl fallback: {curl_exc}"
            ) from curl_exc
        finally:
            tmp_path.unlink(missing_ok=True)

    if content[:2] == b"PK":
        zip_path = out_dir / f"{stem}.zip"
        zip_path.write_bytes(content)
        with zipfile.ZipFile(io.BytesIO(content), "r") as zf:
            names = [n for n in zf.namelist() if n.lower().endswith(".tif")]
            if not names:
                raise RuntimeError("Zip download did not contain a .tif")
            tif_name = names[0]
            zf.extract(tif_name, out_dir)
            return out_dir / tif_name

    tif_path = out_dir / f"{stem}.tif"
    tif_path.write_bytes(content)
    return tif_path


def main():
    parser = argparse.ArgumentParser(
        description="Plot per-pixel valid observation counts for Sentinel-2, Landsat, and merged collections."
    )
    parser.add_argument("--aoi_geojson", help="AOI GeoJSON path (defaults to latest in analysis_results/Terrain).")
    parser.add_argument("--ref_dem_tif", help="Reference DEM for grid-aligned download (defaults to latest in analysis_results/Terrain).")
    parser.add_argument("--start_date", default="2016-10-01")
    parser.add_argument("--end_date", default="2017-08-01")
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
    parser.add_argument("--scale", type=float, default=30.0, help="Export scale in meters.")
    parser.add_argument("--crs", default="EPSG:25832")
    parser.add_argument("--download_timeout", type=float, default=180.0,
                        help="HTTP download timeout in seconds for the Earth Engine export.")
    parser.add_argument("--download_retries", type=int, default=4,
                        help="Number of HTTP download attempts for the Earth Engine export.")
    parser.add_argument(
        "--out_png",
        default="analysis_results/Plots/Meltouts/source_observation_counts_s2_landsat.png",
    )
    parser.add_argument(
        "--out_tif",
        default="analysis_results/Meltout/GEE/source_observation_counts_s2_landsat_demgrid.tif",
    )
    args = parser.parse_args()

    def find_latest_file(directory, pattern):
        base = Path(directory)
        if not base.exists():
            return None
        matches = list(base.glob(pattern))
        if not matches:
            return None
        matches.sort(key=lambda p: p.stat().st_mtime, reverse=True)
        return matches[0]

    if not args.aoi_geojson:
        latest_aoi = find_latest_file("analysis_results/Terrain", "*.geojson")
        if latest_aoi is None:
            raise FileNotFoundError("No AOI GeoJSON found in analysis_results/Terrain.")
        args.aoi_geojson = str(latest_aoi)

    if not args.ref_dem_tif:
        latest_dem = find_latest_file("analysis_results/Terrain", "*.tif")
        if latest_dem is not None:
            args.ref_dem_tif = str(latest_dem)

    print(f"Using AOI: {args.aoi_geojson}")
    if args.ref_dem_tif:
        print(f"Using DEM grid for export alignment: {args.ref_dem_tif}")

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

    count_s2 = s2_col.map(
        lambda img: img.select("snow_mask").mask().rename("obs_count_s2").unmask(0).toInt16()
    ).sum().rename("obs_count_s2")
    count_ls = ls_col.map(
        lambda img: img.select("snow_mask").mask().rename("obs_count_landsat").unmask(0).toInt16()
    ).sum().rename("obs_count_landsat")
    count_total = merged.map(
        lambda img: img.select("snow_mask").mask().rename("obs_count_total").unmask(0).toInt16()
    ).sum().rename("obs_count_total")

    qc = count_s2.addBands(count_ls).addBands(count_total).clip(aoi).toInt16()
    download_params = {
        "format": "GEO_TIFF",
        "filePerBand": False,
    }
    if args.ref_dem_tif:
        ref = get_export_params(args.ref_dem_tif, force_crs=args.crs)
        download_params["crs"] = ref["crs"]
        download_params["crs_transform"] = ref["crsTransform"]
        download_params["dimensions"] = ref["dimensions"]
    else:
        download_params["region"] = aoi
        download_params["scale"] = args.scale
        download_params["crs"] = args.crs

    url = qc.getDownloadURL(download_params)

    out_tif = Path(args.out_tif)
    tif_path = _download_raster(
        url,
        out_tif.parent,
        out_tif.stem,
        timeout_seconds=float(args.download_timeout),
        max_retries=int(args.download_retries),
    )
    if tif_path != out_tif:
        out_tif.parent.mkdir(parents=True, exist_ok=True)
        tif_path.replace(out_tif)
        tif_path = out_tif

    with rasterio.open(tif_path) as ds:
        s2 = ds.read(1).astype(np.float32)
        ls = ds.read(2).astype(np.float32)
        total = ds.read(3).astype(np.float32)

    vmax = float(np.nanmax(total)) if np.isfinite(total).any() else 1.0
    if vmax <= 0:
        vmax = 1.0

    fig, axes = plt.subplots(1, 3, figsize=(16, 5), constrained_layout=True)
    for ax, data, title in [
        (axes[0], s2, "Sentinel-2 valid obs count"),
        (axes[1], ls, "Landsat valid obs count"),
        (axes[2], total, "Merged valid obs count"),
    ]:
        img = ax.imshow(data, cmap="viridis", vmin=0, vmax=vmax)
        ax.set_title(title)
        ax.set_xticks([])
        ax.set_yticks([])
        fig.colorbar(img, ax=ax, fraction=0.046, pad=0.04)

    out_png = Path(args.out_png)
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png, dpi=200)
    plt.close(fig)

    print(f"Saved QC raster: {tif_path}")
    print(f"Saved QC plot: {out_png}")


if __name__ == "__main__":
    main()
