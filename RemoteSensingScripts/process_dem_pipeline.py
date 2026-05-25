import os
import glob
import subprocess
import json
import sys
import math
import argparse

# Configuration
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_INPUT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "../../../../DEM"))

OUTPUT_FILENAME_BASE = "DEM_Merged_UE"
TARGET_RESOLUTION = 1.0
TARGET_UE_SIZE = 2017 # Closest valid UE landscape size for 2km (2000m / 1.0 = 2000px -> pad onto 2017)
TARGET_CRS = "EPSG:2056" # Swiss LV95 based on input filenames. Change to EPSG:25832 if needed.
NODATA_VALUE = -9999

def check_gdal_installed():
    try:
        subprocess.check_output(["gdalinfo", "--version"])
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False

def key_to_float(val):
    if isinstance(val, (int, float)): 
        return float(val)
    return float(val.replace(',',''))

def get_lat_lon(easting, northing, src_crs):
    try:
        # echoed coordinates | gdaltransform -s_srs SRC -t_srs EPSG:4326
        # Output is usually "Lon Lat Z"
        cmd = ["gdaltransform", "-s_srs", src_crs, "-t_srs", "EPSG:4326"]
        input_str = f"{easting} {northing}"
        process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        stdout, stderr = process.communicate(input=input_str)
        
        if process.returncode != 0:
            print(f"Error getting Lat/Lon: {stderr}")
            return None, None
            
        parts = stdout.strip().split()
        if len(parts) >= 2:
            return float(parts[1]), float(parts[0]) # Lat, Lon (Output is usually Longitude Latitude Altitude)
    except Exception as e:
        print(f"Exception getting Lat/Lon: {e}")
    return None, None

def main():
    parser = argparse.ArgumentParser(description="Process DEM tiles into UE compatible formats.")
    parser.add_argument("--input_dir", default=DEFAULT_INPUT_DIR, help="Directory containing raw swissalti3d TIF files.")
    parser.add_argument("--output_dir", default=SCRIPT_DIR, help="Directory to save the processed outputs.")
    args = parser.parse_args()

    input_dir = os.path.abspath(args.input_dir)
    output_dir = os.path.abspath(args.output_dir)

    if not check_gdal_installed():
        print("ERROR: GDAL tools (gdalinfo, gdalwarp, gdal_translate) not found in PATH.")
        print("Please run this script within an OSGeo4W shell or an environment with GDAL installed.")
        return

    # Ensure we are working in the output directory
    os.chdir(output_dir)
    print(f"Working Directory: {os.getcwd()}")
    print(f"Input Directory: {input_dir}")

    # 1. Find Input Files
    tif_files = glob.glob(os.path.join(input_dir, "swissalti3d*.tif"))
    if not tif_files:
        print(f"No tif files found in {input_dir}")
        return
    
    print(f"Found {len(tif_files)} input tiles.")

    # 2. Determine Extent
    min_e = float('inf')
    max_e = float('-inf')
    min_n = float('inf')
    max_n = float('-inf')

    for f in tif_files:
        basename = os.path.basename(f)
        parts = basename.split('_')
        # parts[2] is '2783-1173'
        coords = parts[2].split('-')
        e_km = int(coords[0])
        n_km = int(coords[1])
        
        curr_min_e = e_km * 1000
        curr_max_e = (e_km + 1) * 1000
        curr_min_n = n_km * 1000
        curr_max_n = (n_km + 1) * 1000
        
        if curr_min_e < min_e: min_e = curr_min_e
        if curr_max_e > max_e: max_e = curr_max_e
        if curr_min_n < min_n: min_n = curr_min_n
        if curr_max_n > max_n: max_n = curr_max_n

    print(f"Detected Data Bounds: E[{min_e} : {max_e}], N[{min_n} : {max_n}]")
    data_width = max_e - min_e
    data_height = max_n - min_n
    print(f"Dimensions: {data_width}x{data_height} meters")

    # 3. Calculate Target Extent for UE
    target_ue_pixels = TARGET_UE_SIZE
    target_width_m = (target_ue_pixels - 1) * TARGET_RESOLUTION
    full_span_m = target_ue_pixels * TARGET_RESOLUTION 
    
    # Center the data
    center_e = (min_e + max_e) / 2
    center_n = (min_n + max_n) / 2
    
    target_min_e = center_e - (full_span_m / 2)
    target_max_e = center_e + (full_span_m / 2)
    target_min_n = center_n - (full_span_m / 2)
    target_max_n = center_n + (full_span_m / 2)

    print(f"Target Extent (padded): {target_min_e} {target_min_n} {target_max_e} {target_max_n}")

    # 4. Warp/Merge Command
    merged_tif_temp = "temp_merged.tif"
    
    print("Running gdalwarp...")
    cmd_warp = [
        "gdalwarp",
        "-t_srs", TARGET_CRS,
        "-te", str(target_min_e), str(target_min_n), str(target_max_e), str(target_max_n),
        "-tr", str(TARGET_RESOLUTION), str(TARGET_RESOLUTION),
        "-r", "bilinear",
        "-srcnodata", "-9999", # Assuming source nodata
        "-dstnodata", "-9999",
        "-overwrite"
    ] + tif_files + [merged_tif_temp]
    
    subprocess.check_call(cmd_warp)

    # 5. Get Statistics
    print("Getting statistics...")
    stats_output = subprocess.check_output(["gdalinfo", "-stats", merged_tif_temp], text=True)
    
    min_z = 0
    max_z = 1000
    for line in stats_output.split('\n'):
        if "Minimum=" in line:
            parts = line.split(',')
            for p in parts:
                if "Minimum=" in p: min_z = float(p.split('=')[1])
                if "Maximum=" in p: max_z = float(p.split('=')[1])
    
    print(f"Elevation Range: {min_z} m to {max_z} m")

    # 6. Scale and Convert to UInt16 (save as TIF first)
    print("Normalizing to UInt16 (Intermediate TIF)...")
    intermediate_u16_tif = f"{OUTPUT_FILENAME_BASE}_{TARGET_UE_SIZE}_u16.tif"
    
    cmd_translate_tif = [
        "gdal_translate",
        "-ot", "UInt16",
        "-scale", str(min_z), str(max_z), "0", "65535",
        merged_tif_temp, 
        intermediate_u16_tif
    ]
    subprocess.check_call(cmd_translate_tif)

    # 7. Convert to PNG using ImageMagick
    print("Converting to PNG using ImageMagick...")
    output_png = f"{OUTPUT_FILENAME_BASE}_{TARGET_UE_SIZE}.png"
    
    cmd_magick = [
        "magick", "convert",
        intermediate_u16_tif,
        "-define", "png:color-type=0",
        "-depth", "16",
        "-strip",
        output_png
    ]
    subprocess.check_call(cmd_magick)

    # 8. Write Metadata JSON
    z_scale_cm = (max_z - min_z) * 100 * 128 / 65535
    # UE Z Origin depends on actor placement. "Up=0" strategy
    actor_z_cm = min_z * 100 + 256 * z_scale_cm
    
    # Calculate Lat/Lon for Origin (NW Corner)
    origin_lat, origin_lon = get_lat_lon(target_min_e, target_max_n, TARGET_CRS)
    if origin_lat is None:
        print("WARNING: Could not calculate Lat/Lon. Check gdaltransform.")

    georeferencing = {
        "crs_string": TARGET_CRS,
        "origin_projected_easting": target_min_e,
        "origin_projected_northing": target_max_n, # NW Corner: Min Easting, Max Northing
        "origin_projected_up": 0,
        "origin_wgs84_latitude": origin_lat,
        "origin_wgs84_longitude": origin_lon,
        "note": "Lat/Lon is for the Origin (NW Corner). Use for Cesium/SunSky."
    }

    # VHM Material Parameters
    snow_inv_size_per_meter = 1.0 / target_ue_pixels
    snow_origin_meters = (target_min_e, target_min_n) # Material samples from bottom-left / SW corner
    snow_displacement_scale = 1.0 / z_scale_cm  # Match VHM internal height scale

    meta = {
        "asset_name": output_png,
        "crs": TARGET_CRS,
        "extent_m": { "xmin": target_min_e, "ymin": target_min_n, "xmax": target_max_e, "ymax": target_max_n },
        "resolution": TARGET_RESOLUTION,
        "size_px": target_ue_pixels,
        "stats": { "minZ": min_z, "maxZ": max_z, "range": max_z - min_z },
        "ue_import_settings": {
            "scale_x": 100 * TARGET_RESOLUTION, 
            "scale_y": 100 * TARGET_RESOLUTION,
            "scale_z": z_scale_cm,
            "actor_location_z": actor_z_cm
        },
        "vhm_material_params": {
            "SnowInvSizePerMeter": snow_inv_size_per_meter,
            "SnowOriginMeters_X": snow_origin_meters[0],
            "SnowOriginMeters_Y": snow_origin_meters[1],
            "SnowDepthDecodeScale": 65.535,  # 65535/1000 = convert 16-bit mm to meters
            "SnowDisplacementScale": snow_displacement_scale,  # = 1 / ZScale_cm
            "note": "SnowDisplacementScale = 1/ZScale_cm. Matches VHM internal height representation."
        },
        "georeferencing_actor": georeferencing
    }
    
    with open(f"{OUTPUT_FILENAME_BASE}_Metadata.json", "w") as f:
        json.dump(meta, f, indent=2)

    print("Success! Output generated:")
    print(f" - {output_png}")
    print(f" - {OUTPUT_FILENAME_BASE}_Metadata.json")
    
    # Cleanup
    if os.path.exists(merged_tif_temp):
        os.remove(merged_tif_temp)

if __name__ == "__main__":
    main()
