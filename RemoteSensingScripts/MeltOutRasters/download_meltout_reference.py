import argparse
import io
import subprocess
import zipfile
import time
from pathlib import Path

import ee
import requests
import rasterio

from s2_meltout_base import (
    authenticate_and_initialize,
    load_aoi,
    build_s2_collection,
    build_landsat_collection,
    compute_meltout_doy,
    compute_valid_obs_count,
    get_export_params,
)


def download_raster(url, out_path, timeout_seconds=300, max_retries=4):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    last_err = None
    data = None
    request_timeout = (30, None) if timeout_seconds <= 0 else timeout_seconds
    for attempt in range(1, max_retries + 1):
        try:
            resp = requests.get(url, timeout=request_timeout)
            resp.raise_for_status()
            data = resp.content
            break
        except Exception as exc:
            last_err = exc
            if attempt < max_retries:
                time.sleep(2 * attempt)
    if data is None:
        tmp_path = out_path.with_suffix(out_path.suffix + ".download")
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
            data = tmp_path.read_bytes()
        except Exception as curl_exc:
            raise RuntimeError(f"Failed to download raster after retries: {last_err}; curl fallback: {curl_exc}") from curl_exc
        finally:
            tmp_path.unlink(missing_ok=True)

    if data[:2] == b"PK":
        with zipfile.ZipFile(io.BytesIO(data), "r") as zf:
            tif_names = [n for n in zf.namelist() if n.lower().endswith(".tif")]
            if not tif_names:
                raise RuntimeError("Downloaded zip does not contain a TIFF file.")
            tif_name = tif_names[0]
            zf.extract(tif_name, out_path.parent)
            extracted = out_path.parent / tif_name
            if extracted != out_path:
                extracted.replace(out_path)
            return out_path

    out_path.write_bytes(data)
    return out_path


def normalize_nodata_tag(tif_path, nodata_value=-9999):
    with rasterio.open(tif_path, "r+") as ds:
        if ds.nodata != nodata_value:
            ds.nodata = nodata_value
            print(f"Updated nodata tag to {nodata_value} for {tif_path}")


def main():
    parser = argparse.ArgumentParser(description="Download merged S2+Landsat melt-out reference from Earth Engine.")
    parser.add_argument("--aoi_geojson", help="AOI GeoJSON path (defaults to latest in analysis_results/Terrain).")
    parser.add_argument("--ref_dem_tif", help="Reference DEM for grid-aligned download (defaults to latest in analysis_results/Terrain).")
    parser.add_argument("--start_date", default="2016-10-01")
    parser.add_argument("--end_date", default="2017-08-01")
    parser.add_argument("--project_id", default="industrial-silo-470310-i8")
    parser.add_argument("--strategy", default="midpoint", choices=["midpoint", "first_ground", "last_snow"])
    parser.add_argument("--ndsi_threshold", type=float, default=0.4)
    parser.add_argument("--red_min_reflectance", type=float, default=0.12)
    parser.add_argument("--swir1_max_reflectance", type=float, default=0.16)
    parser.add_argument(
        "--disable_reflectance_guards",
        action="store_true",
        help="Use only the NDSI threshold, without red/SWIR1 guard thresholds.",
    )
    parser.add_argument("--s2_max_cloud_pct", type=float, default=90.0)
    parser.add_argument("--landsat_max_cloud_pct", type=float, default=90.0)
    parser.add_argument("--scale", type=float, default=30.0)
    parser.add_argument("--crs", default="EPSG:25832")
    parser.add_argument("--download_timeout", type=float, default=300.0,
                        help="HTTP download timeout in seconds for the Earth Engine export.")
    parser.add_argument("--download_retries", type=int, default=4,
                        help="Number of HTTP download attempts for the Earth Engine export.")
    parser.add_argument(
        "--include_qc",
        action="store_true",
        help="Include QC count bands (may exceed getDownloadURL size on fine grids).",
    )
    parser.add_argument(
        "--out_tif",
        default="analysis_results/Meltout/GEE/meltout_s2_landsat_20170801_midpoint_demgrid_local.tif",
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
    merged = ee.ImageCollection(s2_col.merge(ls_col)).sort("system:time_start")

    result = compute_meltout_doy(merged, start_date_ee, strategy=args.strategy)
    export_bands = ["meltout_doy"]
    if args.include_qc:
        result = result.addBands(compute_valid_obs_count(merged, "obs_count_total"))
        result = result.addBands(compute_valid_obs_count(s2_col, "obs_count_s2"))
        result = result.addBands(compute_valid_obs_count(ls_col, "obs_count_landsat"))
        export_bands.extend(["gap_days", "obs_count_total", "obs_count_s2", "obs_count_landsat"])

    export_img = result.select(export_bands).clip(aoi).unmask(-9999).int16()

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

    url = export_img.getDownloadURL(download_params)

    out_tif = Path(args.out_tif)
    saved = download_raster(
        url,
        out_tif,
        timeout_seconds=float(args.download_timeout),
        max_retries=int(args.download_retries),
    )
    normalize_nodata_tag(saved)
    print(f"Saved melt-out reference: {saved}")


if __name__ == "__main__":
    main()
