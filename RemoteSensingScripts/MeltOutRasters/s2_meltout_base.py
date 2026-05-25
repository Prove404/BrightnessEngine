import ee
import argparse
import json
import os
import sys
import datetime
from pathlib import Path

LANDSAT_C2_OPTICAL_SCALE = 0.0000275
LANDSAT_C2_OPTICAL_OFFSET = -0.2
SENTINEL2_SR_SCALE = 0.0001

def authenticate_and_initialize(project_id='industrial-silo-470310-i8'):
    """Authenticates and initializes the Earth Engine API."""
    try:
        ee.Initialize(project=project_id)
    except Exception as e:
        print(f"Initialization failed: {e}")
        print("Attempting to authenticate...")
        try:
            ee.Authenticate()
            ee.Initialize(project=project_id)
        except Exception as e2:
            print(f"Authentication failed: {e2}")
            sys.exit(1)

def load_aoi(geojson_path):
    """Loads a GeoJSON file and returns an ee.Geometry."""
    if not os.path.exists(geojson_path):
        print(f"Error: GeoJSON file not found at {geojson_path}")
        sys.exit(1)
    
    try:
        with open(geojson_path, 'r') as f:
            data = json.load(f)
        
        # Handle FeatureCollection, Feature, or Geometry
        if data['type'] == 'FeatureCollection':
            # Assuming the AOI is the union of all features or just the first one.
            # For simplicity, let's take the geometry of the first feature if it exists,
            # or try to convert the whole thing.
            # A safer bet for a general AOI is often just one polygon.
            # Let's try to convert the first feature's geometry.
            if len(data['features']) > 0:
                geom_dict = data['features'][0]['geometry']
                return ee.Geometry(geom_dict)
            else:
                print("Error: FeatureCollection is empty.")
                sys.exit(1)
        elif data['type'] == 'Feature':
            return ee.Geometry(data['geometry'])
        elif data['type'] in ['Polygon', 'MultiPolygon', 'Point', 'MultiPoint', 'LineString', 'MultiLineString', 'GeometryCollection']:
            return ee.Geometry(data)
        else:
            print(f"Unsupported GeoJSON type: {data['type']}")
            sys.exit(1)
            
    except Exception as e:
        print(f"Error loading GeoJSON: {e}")
        sys.exit(1)

def mask_s2_clouds(image):
    """Masks clouds in a Sentinel-2 image using the QA60 band.

    Args:
        image (ee.Image): A Sentinel-2 image.

    Returns:
        ee.Image: A cloud-masked Sentinel-2 image.
    """
    qa = image.select('QA60')

    # Bits 10 and 11 are clouds and cirrus, respectively.
    cloud_bit_mask = 1 << 10
    cirrus_bit_mask = 1 << 11

    # Both flags should be set to zero, indicating clear conditions.
    mask = qa.bitwiseAnd(cloud_bit_mask).eq(0) \
        .And(qa.bitwiseAnd(cirrus_bit_mask).eq(0))

    return image.updateMask(mask)

def select_s2_bands(image):
    """Selects/renames Sentinel-2 bands to common reflectance names."""
    optical = image.select(['B3', 'B4', 'B11'], ['green', 'red', 'swir1']).multiply(SENTINEL2_SR_SCALE)
    return optical.copyProperties(image, image.propertyNames())

def scale_landsat_sr(image):
    """Applies USGS Collection 2 Level-2 optical scaling factors."""
    optical = image.select('SR_B.*').multiply(LANDSAT_C2_OPTICAL_SCALE).add(LANDSAT_C2_OPTICAL_OFFSET)
    return image.addBands(optical, None, True)

def mask_landsat_clouds(image):
    """Masks clouds/shadows in Landsat C2 L2 imagery using QA_PIXEL."""
    qa = image.select('QA_PIXEL')
    fill_bit = 1 << 0
    dilated_cloud_bit = 1 << 1
    cirrus_bit = 1 << 2
    cloud_bit = 1 << 3
    cloud_shadow_bit = 1 << 4

    mask = qa.bitwiseAnd(fill_bit).eq(0) \
        .And(qa.bitwiseAnd(dilated_cloud_bit).eq(0)) \
        .And(qa.bitwiseAnd(cirrus_bit).eq(0)) \
        .And(qa.bitwiseAnd(cloud_bit).eq(0)) \
        .And(qa.bitwiseAnd(cloud_shadow_bit).eq(0))

    return image.updateMask(mask)

def select_landsat_bands(image):
    """Selects/renames Landsat 8/9 scaled SR bands to common reflectance names."""
    return image.select(['SR_B3', 'SR_B4', 'SR_B6'], ['green', 'red', 'swir1'])

def add_ndsi(image):
    """Computes NDSI from harmonized Green/SWIR1 bands and adds it as 'NDSI'."""
    ndsi = image.normalizedDifference(['green', 'swir1']).rename('NDSI')
    return image.addBands(ndsi)

def add_snow_mask(
    image,
    ndsi_threshold=0.4,
    red_min_reflectance=0.12,
    swir1_max_reflectance=0.16,
    use_reflectance_guards=True,
):
    """Adds a binary snow_mask band based on NDSI and optional reflectance guards.
    
    Assumes image is already cloud-masked (masked pixels are excluded).
    However, for a binary mask 0/1, we might want to keep the footprint.
    If the image is masked, the snow_mask will also be masked.
    
    Snow default: NDSI > threshold, red > threshold, SWIR1 < threshold.
    """
    ndsi = image.select('NDSI')
    snow = ndsi.gt(ndsi_threshold)
    if use_reflectance_guards:
        snow = snow.And(image.select('red').gt(red_min_reflectance)) \
                   .And(image.select('swir1').lt(swir1_max_reflectance))
    snow = snow.rename('snow_mask')
    return image.addBands(snow)

def build_s2_collection(
    aoi,
    start_date,
    end_date_inclusive,
    start_date_ee,
    ndsi_threshold,
    max_cloud_pct,
    red_min_reflectance=0.12,
    swir1_max_reflectance=0.16,
    use_reflectance_guards=True,
):
    """Builds processed Sentinel-2 collection with harmonized bands."""
    s2_sr = ee.ImageCollection("COPERNICUS/S2_SR_HARMONIZED")
    filtered = s2_sr.filterBounds(aoi) \
                    .filterDate(start_date, end_date_inclusive) \
                    .filter(ee.Filter.lt('CLOUDY_PIXEL_PERCENTAGE', max_cloud_pct))

    return filtered.map(mask_s2_clouds) \
                   .map(select_s2_bands) \
                   .map(add_ndsi) \
                   .map(lambda img: add_snow_mask(
                       img,
                       ndsi_threshold,
                       red_min_reflectance,
                       swir1_max_reflectance,
                       use_reflectance_guards,
                   )) \
                   .map(lambda img: add_doy(img, start_date_ee)) \
                   .map(lambda img: img.set('source_sensor', 'Sentinel-2'))

def build_landsat_collection(
    aoi,
    start_date,
    end_date_inclusive,
    start_date_ee,
    ndsi_threshold,
    max_cloud_pct,
    red_min_reflectance=0.12,
    swir1_max_reflectance=0.16,
    use_reflectance_guards=True,
):
    """Builds processed Landsat 8/9 C2 L2 collection with harmonized bands."""
    l8 = ee.ImageCollection("LANDSAT/LC08/C02/T1_L2")
    l9 = ee.ImageCollection("LANDSAT/LC09/C02/T1_L2")
    landsat = l8.merge(l9)

    filtered = landsat.filterBounds(aoi) \
                      .filterDate(start_date, end_date_inclusive) \
                      .filter(ee.Filter.lt('CLOUD_COVER', max_cloud_pct))

    return filtered.map(scale_landsat_sr) \
                   .map(mask_landsat_clouds) \
                   .map(select_landsat_bands) \
                   .map(add_ndsi) \
                   .map(lambda img: add_snow_mask(
                       img,
                       ndsi_threshold,
                       red_min_reflectance,
                       swir1_max_reflectance,
                       use_reflectance_guards,
                   )) \
                   .map(lambda img: add_doy(img, start_date_ee)) \
                   .map(lambda img: img.set('source_sensor', 'Landsat-8/9'))

def compute_valid_obs_count(collection, out_name):
    """Counts valid (unmasked) snow_mask observations per pixel."""
    valid = collection.map(
        lambda img: img.select('snow_mask').mask().rename(out_name).unmask(0).toInt16()
    )
    return valid.sum().rename(out_name).toInt16()

def add_doy(image, start_date):
    """Adds both DOY and relative-day bands, masked to valid pixels."""
    mask = image.mask().select(0)
    doy_val = image.date().getRelative('day', 'year').add(1).int()
    rel_day = image.date().difference(start_date, 'day').int()
    doy_band = ee.Image(doy_val).rename('doy').toInt().updateMask(mask)
    rel_band = ee.Image(rel_day).rename('rel_day').toInt().updateMask(mask)
    return image.addBands([doy_band, rel_band])

def compute_meltout_doy(collection, start_date, strategy='midpoint'):
    """Computes the melt-out DOY for each pixel.
    
    Melt-out DOY is defined as the DOY of the last image where snow_mask == 1,
    provided that all subsequent valid observations are snow-free.
    
    Strategy:
    1. Calculate 'last_snow_doy': Max DOY where snow_mask == 1.
    2. Calculate 'last_valid_doy': Max DOY where mask is valid (not cloudy).
    3. If last_valid_doy > last_snow_doy, it implies we saw the ground bare after the last snow.
       In this case, meltout_doy = last_snow_doy.
       Otherwise, we don't know if it melted or if we just stopped having data/clouds.
    """
    
    # Use relative day (days since start_date) to avoid Jan 1 wrap.
    snow_rel = collection.map(lambda img: img.select('rel_day').updateMask(img.select('snow_mask')))
    last_snow_rel = snow_rel.max().rename('last_snow_rel')
    
    def get_ground_after_snow(img):
        rel = img.select('rel_day')
        is_ground = img.select('snow_mask').eq(0)
        is_after = rel.gt(last_snow_rel)
        return rel.updateMask(is_ground.And(is_after))

    ground_after_snow = collection.map(get_ground_after_snow)
    first_ground_rel = ground_after_snow.min().rename('first_ground_rel')
    
    # Midpoint between last snow and first ground (in season-day space)
    # Strategy Selection
    if strategy == 'first_ground':
        meltout_rel = first_ground_rel.unmask(last_snow_rel).rename('meltout_rel')
    elif strategy == 'last_snow':
        meltout_rel = last_snow_rel.rename('meltout_rel')
    else: # midpoint
        meltout_rel = last_snow_rel.add(first_ground_rel).divide(2).rename('meltout_rel')
        meltout_rel = meltout_rel.unmask(last_snow_rel)

    # Cast to Int
    meltout_rel = meltout_rel.toInt()
    
    # Convert season-day to calendar DOY (supports one year crossing)
    start_doy = ee.Number(start_date.getRelative('day', 'year')).add(1)
    year0_days = ee.Date.fromYMD(ee.Number(start_date.get('year')).add(1), 1, 1) \
        .difference(ee.Date.fromYMD(start_date.get('year'), 1, 1), 'day')
    offset_to_next = year0_days.subtract(start_doy).add(1)  # days from start to Jan 1 of next year
    same_year = meltout_rel.lt(offset_to_next)
    
    doy_same = meltout_rel.add(start_doy)
    doy_next = meltout_rel.subtract(offset_to_next).add(1)
    meltout_doy = doy_same.where(same_year.Not(), doy_next).rename('meltout_doy').toInt()
    
    gap_size = first_ground_rel.subtract(last_snow_rel).rename('gap_days')

    return meltout_doy.addBands(gap_size).addBands(last_snow_rel).addBands(first_ground_rel)

def get_export_params(ref_tif_path, force_crs=None):
    """Reads CRS, transform, and dimensions from a reference GeoTIFF."""
    try:
        import rasterio
    except ImportError:
        print("Error: 'rasterio' is required for reading reference DEM.")
        print("Please install it (on Windows, use conda or Gohlke wheels).")
        sys.exit(1)

    with rasterio.open(ref_tif_path) as src:
        if force_crs:
            crs = force_crs
            print(f"Using forced CRS: {crs}")
        else:
            # Try to get EPSG code first
            epsg = src.crs.to_epsg()
            if epsg:
                crs = f"EPSG:{epsg}"
            else:
                # Fallback to WKT, but warn
                print("Warning: Could not determine simple EPSG code from reference DEM.")
                print("GEE might fail to parse the raw WKT CRS.")
                print(f"Raw CRS: {src.crs}")
                print("Recommendation: Use --force_crs EPSG:XXXX to specify it manually.")
                crs = src.crs.to_wkt()
            
        transform = src.transform
        width = src.width
        height = src.height
        
        # GEE expects crs_transform as [xScale, xShearing, xLevel, yShearing, yScale, yLevel]
        # rasterio transform is Affine(a, b, c, d, e, f) -> [a, b, c, d, e, f]
        # Cast to standard python floats to avoid numpy types issues
        gee_transform = [float(transform.a), float(transform.b), float(transform.c), 
                         float(transform.d), float(transform.e), float(transform.f)]
        
        print(f"Read Reference DEM Params:")
        print(f"  CRS: {crs}")
        print(f"  Transform: {gee_transform}")
        print(f"  Dimensions: {width}x{height}")
        
        return {
            'crs': crs,
            'crsTransform': gee_transform,
            'dimensions': f"{width}x{height}"
        }

def export_image(image, description, bucket=None, folder=None, **kwargs):
    """Exports an image to Drive or Cloud Storage."""
    # Set nodata
    # We cast to Int16 as requested.
    # We should unmask and set nodata value.
    # User requested nodata = -9999
    
    # Select bands to export.
    # If image has multiple bands, we might want to export them all or just meltout_doy.
    # User said "Export a single-band meltout_doy GeoTIFF (Int16) and optionally a second QC band."
    # Let's export meltout_doy and qc if present.
    
    bands = image.bandNames().getInfo()
    to_export = image.select(bands)
    
    # Handle nodata: unmask(-9999)
    to_export = to_export.unmask(-9999).int16()
    
    task_config = {
        'image': to_export,
        'description': description,
        'scale': kwargs.get('scale'), # Optional if dimensions/crsTransform provided
        'region': kwargs.get('region'), # Optional if dimensions provided
        'crs': kwargs.get('crs'),
        'crsTransform': kwargs.get('crsTransform'),
        'dimensions': kwargs.get('dimensions'),
        'maxPixels': 1e13,
        'fileFormat': 'GeoTIFF',
        'formatOptions': {
            'cloudOptimized': True
        }
    }
    
    # Remove None values
    task_config = {k: v for k, v in task_config.items() if v is not None}
    
    if bucket:
        task = ee.batch.Export.image.toCloudStorage(bucket=bucket, **task_config)
        print(f"Starting Cloud Storage export task: {description} -> gs://{bucket}/{description}.tif")
    else:
        # Default to Drive
        task = ee.batch.Export.image.toDrive(folder=folder, **task_config)
        print(f"Starting Drive export task: {description} -> Drive/{folder}/{description}.tif")
        
    task.start()
    print(f"Task ID: {task.id}")
    print("Monitor at: https://code.earthengine.google.com/tasks")

def main():
    parser = argparse.ArgumentParser(description="Melt-out Analysis Script (Sentinel-2 and/or Landsat 8/9)")
    parser.add_argument('--aoi_geojson', help="Path to AOI GeoJSON file (defaults to latest in analysis_results/Terrain)")
    parser.add_argument('--start_date', help="Start date (YYYY-MM-DD). Defaults to latest melt model config if omitted.")
    parser.add_argument('--end_date', help="End date (YYYY-MM-DD). Defaults to latest melt model config if omitted.")
    parser.add_argument('--out_tif', help="Output filename prefix (e.g. 'meltout_2017')")
    parser.add_argument('--project_id', default='industrial-silo-470310-i8', help="Google Cloud Project ID")
    parser.add_argument('--ref_dem_tif', help="Path to reference DEM GeoTIFF for grid alignment")
    parser.add_argument('--force_crs', help="Force a specific CRS (e.g. 'EPSG:25832') if auto-detection fails")
    parser.add_argument('--strategy', default='midpoint', choices=['midpoint', 'first_ground', 'last_snow'],
                        help="Melt-out strategy: 'midpoint' (default), 'first_ground' (conservative snow duration), 'last_snow' (conservative snow-free).")
    parser.add_argument('--sensor_mode', default='s2_landsat', choices=['s2', 'landsat', 's2_landsat'],
                        help="Sensors used for melt-out extraction: Sentinel-2, Landsat 8/9, or merged.")
    parser.add_argument('--ndsi_threshold', type=float, default=0.4,
                        help="NDSI threshold for binary snow mask (default: 0.4).")
    parser.add_argument('--red_min_reflectance', type=float, default=0.12,
                        help="Minimum red reflectance required for snow when reflectance guards are enabled (default: 0.12).")
    parser.add_argument('--swir1_max_reflectance', type=float, default=0.16,
                        help="Maximum SWIR1 reflectance allowed for snow when reflectance guards are enabled (default: 0.16).")
    parser.add_argument('--disable_reflectance_guards', action='store_true',
                        help="Use only the NDSI threshold, without red/SWIR1 guard thresholds.")
    parser.add_argument('--s2_max_cloud_pct', type=float, default=100.0,
                        help="Prefilter threshold for S2 CLOUDY_PIXEL_PERCENTAGE (default: 100).")
    parser.add_argument('--landsat_max_cloud_pct', type=float, default=100.0,
                        help="Prefilter threshold for Landsat CLOUD_COVER (default: 100).")
    
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

    def load_projection_from_terrain():
        terrain_path = Path("analysis_results/Terrain/Terrain.json")
        if not terrain_path.exists():
            return None
        try:
            data = json.loads(terrain_path.read_text(encoding="utf-8"))
            projection = data.get("Projection")
            if projection:
                return str(projection)
        except Exception as e:
            print(f"Warning: Failed to read Terrain.json: {e}")
        return None

    def load_dates_from_melt_model():
        melt_dir = Path("analysis_results/DegreeDay_OutputsUE")
        latest = find_latest_file(melt_dir, "DegreeDay_MeltModel_*.txt")
        if not latest:
            return None, None
        start = None
        end = None
        try:
            for line in latest.read_text(encoding="utf-8").splitlines():
                if line.startswith("StartTime ="):
                    start = line.split("=", 1)[1].strip().split(" ")[0]
                elif line.startswith("EndTime ="):
                    end = line.split("=", 1)[1].strip().split(" ")[0]
        except Exception as e:
            print(f"Warning: Failed to read melt model config file: {e}")
        return start, end

    if not args.start_date or not args.end_date:
        auto_start, auto_end = load_dates_from_melt_model()
        if not args.start_date and auto_start:
            args.start_date = auto_start
            print(f"Using Start Date from melt model config: {args.start_date}")
        if not args.end_date and auto_end:
            args.end_date = auto_end
            print(f"Using End Date from melt model config: {args.end_date}")

    if not args.start_date or not args.end_date:
        print("Error: --start_date and --end_date are required (or provide a melt model config with StartTime/EndTime).")
        sys.exit(1)

    if not args.aoi_geojson:
        latest_aoi = find_latest_file("analysis_results/Terrain", "*.geojson")
        if not latest_aoi:
            print("Error: No AOI GeoJSON found in analysis_results/Terrain. Provide --aoi_geojson.")
            sys.exit(1)
        args.aoi_geojson = str(latest_aoi)
        print(f"Using latest AOI: {args.aoi_geojson}")

    if not args.ref_dem_tif:
        latest_dem = find_latest_file("analysis_results/Terrain", "*.tif")
        if latest_dem:
            args.ref_dem_tif = str(latest_dem)
            print(f"Using latest reference DEM: {args.ref_dem_tif}")

    if not args.out_tif:
        out_dir = Path("analysis_results/Meltout/GEE")
        out_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        end_tag = args.end_date.replace("-", "")
        args.out_tif = str(out_dir / f"meltout_{args.sensor_mode}_{end_tag}_{args.strategy}_{ts}.tif")
        print(f"Using default output name: {args.out_tif}")

    if not args.force_crs:
        auto_crs = load_projection_from_terrain()
        if auto_crs:
            args.force_crs = auto_crs
            print(f"Using CRS from Terrain.json: {args.force_crs}")

    print(f"Authenticating to GEE (Project: {args.project_id})...")
    authenticate_and_initialize(project_id=args.project_id)
    
    # Load AOI
    # If ref_dem_tif is provided, we might not strictly need aoi_geojson if we use the DEM bounds,
    # but the script structure uses aoi_geojson for filtering collections.
    # So we keep using aoi_geojson for the query, and ref_dem_tif for the export grid.
    
    print(f"Loading AOI from {args.aoi_geojson}...")
    aoi = load_aoi(args.aoi_geojson)
    
    # Note: cross-year seasons are handled using relative day to avoid Jan 1 wrap.
    start_y = int(args.start_date.split('-')[0])
    end_y = int(args.end_date.split('-')[0])
    if start_y != end_y:
        print("INFO: Analysis spans multiple calendar years. Using relative-day ordering to avoid DOY wrap.")
    
    print(f"Querying optical collections from {args.start_date} to {args.end_date} (mode: {args.sensor_mode})...")

    # Advance end date by 1 day to make it inclusive
    end_date_inclusive = ee.Date(args.end_date).advance(1, 'day')
    start_date_ee = ee.Date(args.start_date)

    processed_col = None
    per_sensor_cols = []

    if args.sensor_mode in ['s2', 's2_landsat']:
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
        per_sensor_cols.append(("Sentinel-2", s2_col))

    if args.sensor_mode in ['landsat', 's2_landsat']:
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
        per_sensor_cols.append(("Landsat-8/9", ls_col))

    for _, col in per_sensor_cols:
        processed_col = col if processed_col is None else processed_col.merge(col)

    processed_col = ee.ImageCollection(processed_col).sort('system:time_start')

    # Get basic info
    for sensor_name, col in per_sensor_cols:
        print(f"{sensor_name} images after masks: {col.size().getInfo()}")
    count = processed_col.size().getInfo()
    
    print(f"Found {count} total images after merge.")
    
    if count > 0:
        # Get date range
        dates = processed_col.aggregate_array('system:time_start').map(
            lambda t: ee.Date(t).format('YYYY-MM-dd')
        ).getInfo()
        
        if dates:
            print(f"Date span: {dates[0]} to {dates[-1]}")
        try:
            source_sensors = processed_col.aggregate_array('source_sensor').distinct().getInfo()
            print(f"Sources in merged collection: {source_sensors}")
        except Exception:
            pass
            
        # Compute melt-out
        print(f"Computing melt-out DOY (Strategy: {args.strategy})...")
        result = compute_meltout_doy(processed_col, start_date_ee, strategy=args.strategy)
        obs_count_total = compute_valid_obs_count(processed_col, 'obs_count_total')
        result = result.addBands(obs_count_total)
        if args.sensor_mode in ['s2', 's2_landsat']:
            obs_count_s2 = compute_valid_obs_count(s2_col, 'obs_count_s2')
            result = result.addBands(obs_count_s2)
        if args.sensor_mode in ['landsat', 's2_landsat']:
            obs_count_landsat = compute_valid_obs_count(ls_col, 'obs_count_landsat')
            result = result.addBands(obs_count_landsat)
        
        # Print band names to verify
        print(f"Result bands: {result.bandNames().getInfo()}")
        
        # Export
        if args.out_tif:
            description = os.path.splitext(os.path.basename(args.out_tif))[0]
            
            export_params = {}
            if args.ref_dem_tif:
                print(f"Reading export parameters from {args.ref_dem_tif}...")
                export_params = get_export_params(args.ref_dem_tif, force_crs=args.force_crs)
            else:
                # Default export params if no reference DEM
                # Just use the AOI bounds and a default scale (e.g. 10m or 20m)
                print("No reference DEM provided. Using AOI bounds and 20m scale.")
                export_params = {
                    'region': aoi,
                    'scale': 20
                }
            
            # Export melt-out and QC bands.
            export_bands = ['meltout_doy', 'gap_days', 'obs_count_total']
            if args.sensor_mode in ['s2', 's2_landsat']:
                export_bands.append('obs_count_s2')
            if args.sensor_mode in ['landsat', 's2_landsat']:
                export_bands.append('obs_count_landsat')
            to_export = result.select(export_bands)
            
            print("Export Configuration:")
            print(f"  Description: {description}")
            print(f"  CRS: {export_params.get('crs')}")
            print(f"  Transform: {export_params.get('crsTransform')}")
            print(f"  Dimensions: {export_params.get('dimensions')}")
            
            export_image(
                to_export, 
                description=description, 
                folder="GEE_Exports", # Default folder
                **export_params
            )


        
        if dates:
            print(f"Date span: {dates[0]} to {dates[-1]}")
            
        # Get bounds of the collection (union of footprints) or just the AOI bounds
        # Printing AOI bounds as requested
        aoi_bounds = aoi.bounds().getInfo()
        print(f"AOI Bounds (GeoJSON): {json.dumps(aoi_bounds['coordinates'])}")
        
    else:
        print("No images found matching criteria.")

if __name__ == "__main__":
    main()
