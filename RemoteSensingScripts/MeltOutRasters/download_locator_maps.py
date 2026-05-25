import argparse
import datetime
import json
import math
import time
from pathlib import Path

import ee
import requests
from PIL import Image, ImageColor, ImageDraw, ImageFont

from s2_meltout_base import authenticate_and_initialize, load_aoi, mask_s2_clouds


def find_latest_file(directory, pattern):
    base = Path(directory)
    if not base.exists():
        return None
    matches = list(base.glob(pattern))
    if not matches:
        return None
    matches.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return matches[0]


def infer_map_tag(geojson_path):
    data = json.loads(Path(geojson_path).read_text(encoding="utf-8"))
    if data.get("type") == "FeatureCollection" and data.get("features"):
        props = data["features"][0].get("properties", {})
        if props.get("MapTag"):
            return str(props["MapTag"])
    if data.get("type") == "Feature" and data.get("properties", {}).get("MapTag"):
        return str(data["properties"]["MapTag"])
    return Path(geojson_path).stem


def build_satellite_base(region, start_date, end_date, max_cloud_pct):
    collection = (
        ee.ImageCollection("COPERNICUS/S2_SR_HARMONIZED")
        .filterBounds(region)
        .filterDate(start_date, end_date)
        .filter(ee.Filter.lt("CLOUDY_PIXEL_PERCENTAGE", max_cloud_pct))
        .map(mask_s2_clouds)
    )
    rgb = collection.median().select(["B4", "B3", "B2"])
    return rgb.visualize(min=150, max=3200, gamma=1.15)


def build_locator_images(
    aoi,
    local_radius_m,
    regional_radius_m,
    global_box_km,
    style,
    imagery_start_date,
    imagery_end_date,
    imagery_max_cloud_pct,
    weather_station_point=None,
):
    aoi_centroid = aoi.centroid(1)
    regional_box = aoi_centroid.buffer(global_box_km * 1000.0, 1).bounds(1)

    local_region = aoi_centroid.buffer(local_radius_m, 1).bounds(1)
    regional_region = aoi_centroid.buffer(regional_radius_m, 1).bounds(1)
    global_region = ee.Geometry.Rectangle(
        coords=[-25.0, 32.0, 45.0, 72.0],
        proj="EPSG:4326",
        geodesic=False,
    )

    countries = ee.FeatureCollection("USDOS/LSIB_SIMPLE/2017")

    ocean = ee.Image.constant(1).visualize(
        min=0,
        max=1,
        palette=["d9e7f5"],
    )
    land_mask = ee.Image().byte().paint(countries, 1)
    land = land_mask.selfMask().visualize(palette=["f6f1e8"])
    borders = ee.Image().byte().paint(countries, 1, 1).selfMask().visualize(
        palette=["73808c"]
    )

    aoi_fill = ee.Image().byte().paint(aoi, 1).selfMask().visualize(
        palette=["c53d2f"],
        opacity=0.18,
    )
    aoi_outline = ee.Image().byte().paint(aoi, 1, 3).selfMask().visualize(
        palette=["9b2318"]
    )
    aoi_dot = ee.Image().byte().paint(aoi_centroid, 1, 10).selfMask().visualize(
        palette=["9b2318"]
    )
    station_dot = None
    if weather_station_point is not None:
        station_halo = ee.Image().byte().paint(weather_station_point, 1, 26).selfMask().visualize(
            palette=["ffffff"],
            opacity=0.95,
        )
        station_core = ee.Image().byte().paint(weather_station_point, 1, 14).selfMask().visualize(
            palette=["2b6cff"],
            opacity=1.0,
        )
        station_dot = station_halo.blend(station_core)
    regional_box_outline = ee.Image().byte().paint(regional_box, 1, 3).selfMask().visualize(
        palette=["e28e35"]
    )

    dem = ee.ImageCollection("COPERNICUS/DEM/GLO30").select("DEM").mosaic()
    topo_vis = dem.visualize(
        min=1400,
        max=3300,
        palette=["f8f7f1", "d8d2bd", "b39c72", "7e6a4f", "f4f4f4"],
    )
    hillshade = ee.Terrain.hillshade(dem).visualize(
        min=80,
        max=255,
        palette=["2f3438", "ffffff"],
        opacity=0.35,
    )
    terrain_base = topo_vis.blend(hillshade)

    if style == "terrain_satellite":
        local_satellite = build_satellite_base(
            local_region, imagery_start_date, imagery_end_date, imagery_max_cloud_pct
        )
        global_image = ocean.blend(land).blend(borders).blend(regional_box_outline).blend(aoi_dot)
        regional_image = terrain_base.blend(borders).blend(aoi_fill).blend(aoi_outline).blend(aoi_dot)
        local_image = local_satellite.blend(aoi_fill).blend(aoi_outline)
    else:
        global_image = ocean.blend(land).blend(borders).blend(regional_box_outline).blend(aoi_dot)
        regional_image = ocean.blend(land).blend(borders).blend(aoi_fill).blend(aoi_outline).blend(aoi_dot)
        local_image = terrain_base.blend(aoi_fill).blend(aoi_outline)

    if station_dot is not None:
        regional_image = regional_image.blend(station_dot)
        local_image = local_image.blend(station_dot)

    return {
        "global": {"image": global_image, "region": global_region, "crs": "EPSG:4326"},
        "regional": {"image": regional_image, "region": regional_region, "crs": "EPSG:3857"},
        "local": {"image": local_image, "region": local_region, "crs": "EPSG:3857"},
    }


def get_thumb_url(image, region, dimensions, crs="EPSG:3857"):
    return image.getThumbURL(
        {
            "region": region,
            "dimensions": dimensions,
            "format": "png",
            "crs": crs,
        }
    )


def download_binary(url, out_path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    last_err = None
    for attempt in range(1, 5):
        try:
            response = requests.get(url, timeout=300)
            response.raise_for_status()
            out_path.write_bytes(response.content)
            return
        except Exception as exc:
            last_err = exc
            if attempt < 4:
                time.sleep(2 * attempt)
    raise RuntimeError(f"Failed to download {out_path.name}: {last_err}")


def fit_image(image, max_width, max_height):
    scale = min(max_width / image.width, max_height / image.height)
    new_size = (
        max(1, int(round(image.width * scale))),
        max(1, int(round(image.height * scale))),
    )
    return image.resize(new_size, Image.Resampling.LANCZOS)


def lonlat_to_webmercator(lon_deg, lat_deg):
    radius = 6378137.0
    lon_rad = math.radians(lon_deg)
    lat_rad = math.radians(max(min(lat_deg, 85.05112878), -85.05112878))
    x = radius * lon_rad
    y = radius * math.log(math.tan(math.pi / 4.0 + lat_rad / 2.0))
    return x, y


def project_point_into_panel(bounds_lonlat, lon_deg, lat_deg, panel_width, panel_height):
    coords = bounds_lonlat["coordinates"][0]
    lons = [pt[0] for pt in coords]
    lats = [pt[1] for pt in coords]
    min_lon, max_lon = min(lons), max(lons)
    min_lat, max_lat = min(lats), max(lats)

    min_x, min_y = lonlat_to_webmercator(min_lon, min_lat)
    max_x, max_y = lonlat_to_webmercator(max_lon, max_lat)
    pt_x, pt_y = lonlat_to_webmercator(lon_deg, lat_deg)

    x_frac = 0.5 if max_x == min_x else (pt_x - min_x) / (max_x - min_x)
    y_frac = 0.5 if max_y == min_y else 1.0 - ((pt_y - min_y) / (max_y - min_y))

    return (
        max(0, min(panel_width - 1, int(round(x_frac * panel_width)))),
        max(0, min(panel_height - 1, int(round(y_frac * panel_height)))),
    )


def draw_station_marker(draw, panel_origin_x, panel_origin_y, panel_width, panel_height, bounds_lonlat, station_lon, station_lat, label):
    px, py = project_point_into_panel(bounds_lonlat, station_lon, station_lat, panel_width, panel_height)
    cx = panel_origin_x + px
    cy = panel_origin_y + py
    draw.ellipse([cx - 15, cy - 15, cx + 15, cy + 15], fill=(255, 255, 255, 235), outline=(35, 35, 35, 160), width=1)
    draw.ellipse([cx - 8, cy - 8, cx + 8, cy + 8], fill=ImageColor.getrgb("#2b6cff"), outline=(255, 255, 255), width=2)
    font = ImageFont.load_default()
    text_bbox = draw.textbbox((0, 0), label, font=font)
    label_w = text_bbox[2] - text_bbox[0]
    label_h = text_bbox[3] - text_bbox[1]
    lx = min(panel_origin_x + panel_width - label_w - 10, cx + 18)
    ly = max(panel_origin_y + 10, cy - label_h - 6)
    draw.rounded_rectangle([lx - 5, ly - 3, lx + label_w + 5, ly + label_h + 3], radius=8, fill=(255, 255, 255, 225))
    draw.text((lx, ly - 1), label, fill=ImageColor.getrgb("#17366f"), font=font)


def draw_local_scale_bar(draw, panel_origin_x, panel_origin_y, panel_width, panel_height, bounds_lonlat, scale_km=5):
    coords = bounds_lonlat["coordinates"][0]
    lons = [pt[0] for pt in coords]
    lats = [pt[1] for pt in coords]
    min_x, _ = lonlat_to_webmercator(min(lons), min(lats))
    max_x, _ = lonlat_to_webmercator(max(lons), min(lats))
    width_m = abs(max_x - min_x)
    if width_m <= 0:
        return
    bar_px = int(round(panel_width * ((scale_km * 1000.0) / width_m)))
    bar_px = max(80, min(bar_px, panel_width // 3))
    x0 = panel_origin_x + panel_width - bar_px - 40
    y0 = panel_origin_y + panel_height - 46
    mid_x = x0 + bar_px // 2
    draw.rectangle([x0, y0, mid_x, y0 + 10], fill=(255, 255, 255, 235), outline=(30, 30, 30))
    draw.rectangle([mid_x, y0, x0 + bar_px, y0 + 10], fill=(35, 35, 35, 235), outline=(30, 30, 30))
    for tick_x in [x0, mid_x, x0 + bar_px]:
        draw.line([tick_x, y0, tick_x, y0 + 16], fill=(30, 30, 30), width=2)
    font = ImageFont.load_default()
    draw.text((x0 - 2, y0 - 18), "0", fill=ImageColor.getrgb("#222222"), font=font)
    draw.text((mid_x - 10, y0 - 18), f"{scale_km // 2}", fill=ImageColor.getrgb("#222222"), font=font)
    draw.text((x0 + bar_px - 16, y0 - 18), f"{scale_km} km", fill=ImageColor.getrgb("#222222"), font=font)


def draw_local_north_arrow(draw, panel_origin_x, panel_origin_y, panel_width, panel_height):
    base_x = panel_origin_x + panel_width - 52
    base_y = panel_origin_y + panel_height - 108
    arrow_h = 58
    draw.line([base_x, base_y, base_x, base_y - arrow_h], fill=(255, 255, 255, 240), width=6)
    draw.polygon(
        [(base_x, base_y - arrow_h - 16), (base_x - 10, base_y - arrow_h + 8), (base_x + 10, base_y - arrow_h + 8)],
        fill=(255, 255, 255, 240),
        outline=(40, 40, 40),
    )
    font = ImageFont.load_default()
    draw.text((base_x - 4, base_y - arrow_h - 34), "N", fill=ImageColor.getrgb("#222222"), font=font)


def draw_labeled_panel(canvas, image, x, y, label):
    border = 8
    panel_box = [x, y, x + image.width, y + image.height]

    draw = ImageDraw.Draw(canvas, "RGBA")
    draw.rounded_rectangle(panel_box, radius=18, fill=(255, 255, 255, 255))
    draw.rounded_rectangle(panel_box, radius=18, outline=(180, 176, 168, 255), width=border)
    canvas.paste(image, (x, y))

    font = ImageFont.load_default()
    label_pad_x = 14
    label_pad_y = 10
    text_bbox = draw.textbbox((0, 0), label, font=font)
    label_w = (text_bbox[2] - text_bbox[0]) + 2 * label_pad_x
    label_h = (text_bbox[3] - text_bbox[1]) + 2 * label_pad_y
    label_box = [x + 16, y + 16, x + 16 + label_w, y + 16 + label_h]
    draw.rounded_rectangle(label_box, radius=12, fill=(255, 255, 255, 235), outline=(160, 160, 160, 255), width=1)
    draw.text((x + 16 + label_pad_x, y + 16 + label_pad_y - 1), label, fill=ImageColor.getrgb("#222222"), font=font)


def compose_locator_panel(outputs, out_path, style, station_name=None, station_lon=None, station_lat=None):
    canvas = Image.new("RGB", (2400, 1650), ImageColor.getrgb("#ece7de"))

    local_img = fit_image(Image.open(outputs["local"]["file"]).convert("RGB"), 1500, 1500)
    regional_img = fit_image(Image.open(outputs["regional"]["file"]).convert("RGB"), 760, 760)
    global_img = fit_image(Image.open(outputs["global"]["file"]).convert("RGB"), 760, 520)

    draw_labeled_panel(canvas, local_img, 70, 70, "Local AOI")
    draw_labeled_panel(canvas, regional_img, 1580, 70, "Regional Context")
    draw_labeled_panel(canvas, global_img, 1580, 840, "Europe Context")

    draw = ImageDraw.Draw(canvas)
    font = ImageFont.load_default()
    style_text = "Plain basemap" if style == "plain" else "Terrain / satellite basemap"
    draw.text((70, 30), f"Totalp locator figure ({style_text})", fill=ImageColor.getrgb("#222222"), font=font)
    if station_name and station_lon is not None and station_lat is not None:
        draw_station_marker(
            draw,
            70,
            70,
            local_img.width,
            local_img.height,
            outputs["local"]["bounds_lonlat"],
            station_lon,
            station_lat,
            station_name,
        )
    draw_local_north_arrow(draw, 70, 70, local_img.width, local_img.height)
    draw_local_scale_bar(draw, 70, 70, local_img.width, local_img.height, outputs["local"]["bounds_lonlat"])

    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)


def main():
    parser = argparse.ArgumentParser(
        description="Download AOI locator-map PNGs from Earth Engine using the existing GEE pipeline."
    )
    parser.add_argument(
        "--aoi_geojson",
        help="AOI GeoJSON path. Defaults to the Totalp AOI export if present, else latest analysis_results GeoJSON.",
    )
    parser.add_argument("--project_id", default="industrial-silo-470310-i8")
    parser.add_argument(
        "--out_dir",
        help="Output folder for PNGs and metadata JSON.",
    )
    parser.add_argument(
        "--prefix",
        help="Filename prefix. Defaults to '<MapTag>_locator'.",
    )
    parser.add_argument("--dimensions", type=int, default=2400)
    parser.add_argument("--local_radius_m", type=float, default=12000.0)
    parser.add_argument("--regional_radius_m", type=float, default=220000.0)
    parser.add_argument(
        "--style",
        default="plain",
        choices=["plain", "terrain_satellite"],
        help="Map styling preset for the three exported panels.",
    )
    parser.add_argument(
        "--imagery_start_date",
        default="2023-06-01",
        help="Start date for Sentinel-2 imagery used by the terrain/satellite style.",
    )
    parser.add_argument(
        "--imagery_end_date",
        default="2023-09-30",
        help="End date for Sentinel-2 imagery used by the terrain/satellite style.",
    )
    parser.add_argument(
        "--imagery_max_cloud_pct",
        type=float,
        default=20.0,
        help="Sentinel-2 scene-level cloud filter for the terrain/satellite style.",
    )
    parser.add_argument(
        "--global_box_km",
        type=float,
        default=120.0,
        help="Half-size of the highlighted regional box used on the global panel.",
    )
    parser.add_argument("--station_lat", type=float, default=46.833325)
    parser.add_argument("--station_lon", type=float, default=9.806394)
    parser.add_argument("--station_name", default="WFJ")
    args = parser.parse_args()

    default_totalp_aoi = Path(
        "analysis_results/Maps/Totalp/RS_Data/Meltout/AOI_Totalp_from_Terrain_2017_2m.geojson"
    )
    if not args.aoi_geojson:
        if default_totalp_aoi.exists():
            args.aoi_geojson = str(default_totalp_aoi)
        else:
            latest_aoi = find_latest_file("analysis_results", "*.geojson")
            if latest_aoi is None:
                raise FileNotFoundError("No AOI GeoJSON found under analysis_results.")
            args.aoi_geojson = str(latest_aoi)

    map_tag = infer_map_tag(args.aoi_geojson)
    if not args.prefix:
        args.prefix = f"{map_tag}_locator"
        if args.style != "plain":
            args.prefix = f"{args.prefix}_{args.style}"

    if not args.out_dir:
        args.out_dir = str(Path("analysis_results/Maps") / map_tag / "RS_Data" / "Locator")

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Using AOI: {args.aoi_geojson}")
    print(f"Map tag: {map_tag}")
    print(f"Output folder: {out_dir}")

    authenticate_and_initialize(project_id=args.project_id)
    aoi = load_aoi(args.aoi_geojson)
    weather_station_point = ee.Geometry.Point([args.station_lon, args.station_lat])
    locator_images = build_locator_images(
        aoi=aoi,
        local_radius_m=args.local_radius_m,
        regional_radius_m=args.regional_radius_m,
        global_box_km=args.global_box_km,
        style=args.style,
        imagery_start_date=args.imagery_start_date,
        imagery_end_date=args.imagery_end_date,
        imagery_max_cloud_pct=args.imagery_max_cloud_pct,
        weather_station_point=weather_station_point,
    )

    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    outputs = {}
    for panel_name, panel in locator_images.items():
        out_path = out_dir / f"{args.prefix}_{panel_name}_{timestamp}.png"
        url = get_thumb_url(panel["image"], panel["region"], args.dimensions, panel["crs"])
        print(f"Downloading {panel_name} panel -> {out_path.name}")
        download_binary(url, out_path)
        outputs[panel_name] = {
            "file": str(out_path),
            "url": url,
            "crs": panel["crs"],
            "bounds_lonlat": panel["region"].bounds(1).getInfo(),
        }

    composite_path = out_dir / f"{args.prefix}_composite_{timestamp}.png"
    compose_locator_panel(
        outputs,
        composite_path,
        args.style,
        station_name=args.station_name,
        station_lon=args.station_lon,
        station_lat=args.station_lat,
    )
    print(f"Saved composite: {composite_path}")

    metadata = {
        "map_tag": map_tag,
        "aoi_geojson": args.aoi_geojson,
        "project_id": args.project_id,
        "style": args.style,
        "dimensions": args.dimensions,
        "local_radius_m": args.local_radius_m,
        "regional_radius_m": args.regional_radius_m,
        "global_box_km": args.global_box_km,
        "station_lat": args.station_lat,
        "station_lon": args.station_lon,
        "station_name": args.station_name,
        "imagery_start_date": args.imagery_start_date,
        "imagery_end_date": args.imagery_end_date,
        "imagery_max_cloud_pct": args.imagery_max_cloud_pct,
        "generated_utc": datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "outputs": outputs,
        "composite_file": str(composite_path),
    }
    metadata_path = out_dir / f"{args.prefix}_{timestamp}_metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(f"Saved metadata: {metadata_path}")


if __name__ == "__main__":
    main()
