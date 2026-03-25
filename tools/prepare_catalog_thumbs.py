import io
import json
import os
import sys
import urllib.request

from PIL import Image


def choose_urls(entry):
    icon = entry.get("iconUrl") or ""
    thumb = entry.get("thumbnailUrl") or ""
    cover = entry.get("coverUrl") or ""

    if thumb:
        fallback = cover if cover and cover != thumb else icon if icon and icon != thumb else ""
        return thumb, fallback
    if cover:
        return cover, icon if icon and icon != cover else ""
    if icon:
        return icon, ""
    return "", ""


def is_valid_thumb(path, expected_size):
    if not os.path.exists(path):
        return False
    try:
        with Image.open(path) as image:
            return image.size == (expected_size, expected_size)
    except Exception:
        return False


def save_normalized_image(url, destination_path, target_size):
    request = urllib.request.Request(url, headers={"User-Agent": "MILManagerCatalogSync/1.0"})
    with urllib.request.urlopen(request, timeout=20) as response:
        payload = response.read()

    with Image.open(io.BytesIO(payload)) as image:
        image = image.convert("RGBA")
        image.thumbnail((target_size, target_size), Image.Resampling.LANCZOS)

        canvas = Image.new("RGBA", (target_size, target_size), (0, 0, 0, 0))
        offset_x = (target_size - image.width) // 2
        offset_y = (target_size - image.height) // 2
        canvas.alpha_composite(image, (offset_x, offset_y))
        canvas.save(destination_path, format="PNG")


def main():
    if len(sys.argv) != 4:
        print("usage: prepare_catalog_thumbs.py <index_path> <destination_dir> <target_size>", file=sys.stderr)
        return 2

    index_path = sys.argv[1]
    destination_dir = sys.argv[2]
    target_size = int(sys.argv[3])

    if not os.path.exists(index_path):
        print("Catalog thumbs: downloaded=0 skipped=0 failed=0")
        return 0

    with open(index_path, "r", encoding="utf-8") as handle:
        index_data = json.load(handle)

    entries = index_data.get("entries") or []
    if not entries:
        print("Catalog thumbs: downloaded=0 skipped=0 failed=0")
        return 0

    os.makedirs(destination_dir, exist_ok=True)
    manifest_path = os.path.join(destination_dir, "thumbs-manifest.json")
    try:
        with open(manifest_path, "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
    except Exception:
        manifest = {}

    downloaded = 0
    skipped = 0
    failed = 0

    for entry in entries:
        entry_id = str(entry.get("id") or "")
        if not entry_id:
            continue

        primary_url, fallback_url = choose_urls(entry)
        if not primary_url:
            skipped += 1
            continue

        destination_path = os.path.join(destination_dir, f"{entry_id}.img")
        manifest_entry = manifest.get(entry_id) or {}
        is_current = (
            manifest_entry.get("primaryUrl") == primary_url
            and manifest_entry.get("fallbackUrl") == fallback_url
            and int(manifest_entry.get("size") or 0) == target_size
            and is_valid_thumb(destination_path, target_size)
        )
        if is_current:
            skipped += 1
            continue

        success = False
        for url, stored_fallback in ((primary_url, fallback_url), (fallback_url, "")):
            if not url:
                continue
            try:
                save_normalized_image(url, destination_path, target_size)
                if not is_valid_thumb(destination_path, target_size):
                    raise RuntimeError("invalid thumbnail")
                manifest[entry_id] = {
                    "primaryUrl": url,
                    "fallbackUrl": stored_fallback,
                    "size": target_size,
                }
                downloaded += 1
                success = True
                break
            except Exception:
                try:
                    os.remove(destination_path)
                except OSError:
                    pass

        if not success:
            manifest.pop(entry_id, None)
            failed += 1

    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)

    print(f"Catalog thumbs: downloaded={downloaded} skipped={skipped} failed={failed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
