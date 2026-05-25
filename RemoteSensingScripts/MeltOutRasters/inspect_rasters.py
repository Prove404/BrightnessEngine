import rasterio
import json
import sys
import os
from pathlib import Path
from rasterio.transform import Affine


def resolve_ue_cell_size_meters(meta):
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
            return cell_size * scale_m
    return cell_size

def inspect_s2(path):
    print(f"--- Sentinel-2 Raster: {path} ---")
    try:
        with rasterio.open(path) as ds:
            print(f"CRS: {ds.crs}")
            print(f"Bounds: {ds.bounds}")
            print(f"Transform: {ds.transform}")
            print(f"Size: {ds.width}x{ds.height}")
            print(f"Nodata: {ds.nodata}")
            
            data = ds.read(1)
            print(f"Data Min: {data.min()}")
            print(f"Data Max: {data.max()}")
            
            if ds.nodata is not None:
                valid_data = data[data != ds.nodata]
                if valid_data.size > 0:
                    print(f"Valid Data Min: {valid_data.min()}")
                    print(f"Valid Data Max: {valid_data.max()}")
    except Exception as e:
        print(f"Error reading S2: {e}")

def inspect_ue(json_path):
    print(f"--- UE Export: {json_path} ---")
    try:
        with open(json_path, 'r') as f:
            meta = json.load(f)
            
        origin_x = meta.get('ProjectedOriginX', 0)
        origin_y = meta.get('ProjectedOriginY', 0)
        raw_cell_size = meta.get('CellSize', 1.0)
        cell_size = resolve_ue_cell_size_meters(meta)
        width = meta.get('GridWidth', 1)
        height = meta.get('GridHeight', 1)
        crs = meta.get('Projection', 'Unknown')
        pixel_layout = meta.get('PixelLayoutTransform')
        legacy_rot90 = bool(meta.get('bRotated90', meta.get('Rotated90', False)))
        
        print(f"CRS: {crs}")
        print(f"Origin: ({origin_x}, {origin_y})")
        print(f"CellSize (resolved meters): {cell_size}")
        if 'CellSizeMeters' in meta or meta.get('ScaleX') is not None or meta.get('ScaleY') is not None:
            print(f"CellSize (raw metadata): {raw_cell_size}")
        print(f"Size: {width}x{height}")
        if pixel_layout:
            print(f"PixelLayoutTransform: {pixel_layout}")
        print(f"Legacy Rotated90 flag: {legacy_rot90}")
        if 'LegacyRotate90Requested' in meta:
            print(f"LegacyRotate90Requested: {bool(meta['LegacyRotate90Requested'])}")
        
        # Calculate bounds assuming Positive Y Scale (Bottom-Up)
        # MinX = OriginX
        # MaxX = OriginX + Width * CellSize
        # MinY = OriginY
        # MaxY = OriginY + Height * CellSize
        
        min_x = origin_x
        max_x = origin_x + width * cell_size
        min_y = origin_y
        max_y = origin_y + height * cell_size
        
        print(f"Bounds (assuming Bottom-Up/South-Up): Left={min_x}, Bottom={min_y}, Right={max_x}, Top={max_y}")
        
        # Calculate bounds assuming Negative Y Scale (Top-Down/North-Up)
        # MinX = OriginX
        # MaxX = OriginX + Width * CellSize
        # MaxY = OriginY
        # MinY = OriginY - Height * CellSize
        
        min_y_topdown = origin_y - height * cell_size
        max_y_topdown = origin_y
        
        print(f"Bounds (assuming Top-Down/North-Up): Left={min_x}, Bottom={min_y_topdown}, Right={max_x}, Top={max_y_topdown}")

    except Exception as e:
        print(f"Error reading UE JSON: {e}")

if __name__ == "__main__":
    def find_latest_file(directory, pattern):
        base = Path(directory)
        if not base.exists():
            return None
        matches = list(base.glob(pattern))
        if not matches:
            return None
        matches.sort(key=lambda p: p.stat().st_mtime, reverse=True)
        return matches[0]

    s2_path = None
    ue_path = None

    if len(sys.argv) >= 2:
        s2_path = Path(sys.argv[1])
    if len(sys.argv) >= 3:
        ue_path = Path(sys.argv[2])

    if s2_path is None:
        s2_path = find_latest_file("analysis_results/Meltout/GEE", "*.tif")
    if ue_path is None:
        ue_path = find_latest_file("analysis_results/Meltout", "Meltout_*.json")

    if s2_path is None or ue_path is None:
        print("Usage: python inspect_rasters.py <s2_tif> <ue_json>")
        print("Or run with no args to auto-pick latest files.")
        sys.exit(1)

    print(f"Using S2: {s2_path}")
    print(f"Using UE: {ue_path}")

    inspect_s2(str(s2_path))
    inspect_ue(str(ue_path))
