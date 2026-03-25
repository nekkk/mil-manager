#!/usr/bin/env python3
import io
import hashlib
import json
import os
import time
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "catalog-source"
METADATA_PATH = SOURCE_DIR / "catalog-metadata.json"
ENTRIES_DIR = SOURCE_DIR / "entries"
LEGACY_SOURCE_PATH = SOURCE_DIR / "catalog-source.json"
CACHE_DIR = ROOT / ".cache" / "titledb"
TITLEDB_SOURCES = {
    "pt": "https://raw.githubusercontent.com/blawar/titledb/master/BR.pt.json",
    "en": "https://raw.githubusercontent.com/blawar/titledb/master/US.en.json",
}
TITLEDB_CACHE_TTL_SECONDS = 24 * 60 * 60
DIST_PATHS = [
    ROOT / "dist" / "index.json",
]
DIST_THUMBS_DIR = ROOT / "dist" / "thumbs"
DIST_THUMBS_PACK_PATH = ROOT / "dist" / "thumbs-pack.zip"
DIST_THUMBS_MANIFEST_PATH = ROOT / "dist" / "thumbs-manifest.json"
ARTWORK_MANIFEST_PATH = CACHE_DIR / "artwork-manifest.json"
DEFAULT_PUBLIC_BASE_URL = "https://nekkk.github.io/mil-manager-catalog/"
THUMB_SIZE = 110

REQUIRED_FIELDS = ("id", "section", "titleId")
_TITLEDB_BY_LOCALE = {}


def load_source() -> dict:
    if METADATA_PATH.exists() and ENTRIES_DIR.exists():
        metadata = json.loads(METADATA_PATH.read_text(encoding="utf-8"))
        entries = []
        for path in sorted(ENTRIES_DIR.glob("*.json")):
            entry = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(entry, dict):
                raise ValueError(f"Entrada invalida em {path}")
            entries.append(entry)
        metadata["entries"] = entries
        return metadata

    if LEGACY_SOURCE_PATH.exists():
        return json.loads(LEGACY_SOURCE_PATH.read_text(encoding="utf-8"))

    raise FileNotFoundError(
        f"Fonte do catalogo nao encontrada. Esperado: {METADATA_PATH} + {ENTRIES_DIR} ou {LEGACY_SOURCE_PATH}"
    )


def download_to_cache(url: str, cache_path: Path) -> None:
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=90) as response:
        payload = response.read()
    cache_path.write_bytes(payload)


def download_bytes(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "MILManagerCatalogGenerator/1.0"})
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read()


def load_titledb(locale: str) -> dict:
    cached = _TITLEDB_BY_LOCALE.get(locale)
    if cached is not None:
        return cached

    url = TITLEDB_SOURCES[locale]
    cache_path = CACHE_DIR / f"{locale}.json"
    should_refresh = True
    if cache_path.exists():
        age_seconds = time.time() - cache_path.stat().st_mtime
        should_refresh = age_seconds > TITLEDB_CACHE_TTL_SECONDS

    if should_refresh:
        try:
            download_to_cache(url, cache_path)
        except Exception:
            if not cache_path.exists():
                raise

    raw = json.loads(cache_path.read_text(encoding="utf-8"))
    by_title_id = {}
    for item in raw.values():
        if not isinstance(item, dict):
            continue
        title_id = str(item.get("id") or "").upper()
        if title_id:
            by_title_id[title_id] = item

    _TITLEDB_BY_LOCALE[locale] = by_title_id
    return by_title_id


def enrich_entry_with_titledb(merged: dict) -> dict:
    title_id = str(merged.get("titleId") or "").upper()
    if not title_id:
        return merged

    try:
        pt = load_titledb("pt").get(title_id, {})
        en = load_titledb("en").get(title_id, {})
    except Exception:
        return merged

    intro_pt = (pt.get("intro") or "").strip()
    intro_en = (en.get("intro") or "").strip()
    official_name = (pt.get("name") or "").strip() or (en.get("name") or "").strip()
    icon_url = (pt.get("iconUrl") or "").strip() or (en.get("iconUrl") or "").strip()
    banner_url = (pt.get("bannerUrl") or "").strip() or (en.get("bannerUrl") or "").strip()

    merged["_titledbIntroPtBr"] = intro_pt
    merged["_titledbIntroEnUs"] = intro_en
    merged["_titledbName"] = official_name
    if icon_url and not merged.get("thumbnailUrl"):
        merged["thumbnailUrl"] = icon_url
    if icon_url and not merged.get("iconUrl"):
        merged["iconUrl"] = icon_url
    if banner_url and not merged.get("coverUrl"):
        merged["coverUrl"] = banner_url

    return merged


def default_content_types_for_section(section: str) -> list[str]:
    normalized = str(section or "").strip().lower()
    if normalized == "translations":
        return ["translation"]
    if normalized == "mods":
        return ["mod"]
    if normalized == "cheats":
        return ["cheat"]
    return []


def normalize_content_type(value: str) -> str:
    normalized = str(value or "").strip().lower()
    aliases = {
        "traducao": "translation",
        "tradução": "translation",
        "translation": "translation",
        "dub": "dub",
        "dublagem": "dub",
        "mod": "mod",
        "mods": "mod",
        "cheat": "cheat",
        "cheats": "cheat",
    }
    return aliases.get(normalized, normalized)


def normalize_public_base_url(source: dict) -> str:
    base_url = (
        str(source.get("publicBaseUrl") or "").strip()
        or os.environ.get("MIL_CATALOG_PUBLIC_BASE_URL", "").strip()
        or DEFAULT_PUBLIC_BASE_URL
    )
    if not base_url.endswith("/"):
        base_url += "/"
    return base_url


def choose_artwork_urls(entry: dict) -> tuple[str, str]:
    thumbnail = str(entry.get("thumbnailUrl") or "").strip()
    icon = str(entry.get("iconUrl") or "").strip()
    cover = str(entry.get("coverUrl") or "").strip()

    primary = thumbnail or icon or cover
    fallback = ""
    for candidate in (icon, cover):
        if candidate and candidate != primary:
            fallback = candidate
            break

    return primary, fallback


def normalize_compatibility(raw_compatibility, entry_id: str) -> dict:
    compatibility = raw_compatibility or {}
    if not isinstance(compatibility, dict):
        raise ValueError(f"Entrada '{entry_id}' com compatibility invalido")
    normalized = {}
    min_version = str(compatibility.get("minGameVersion") or "").strip()
    max_version = str(compatibility.get("maxGameVersion") or "").strip()
    exact_versions = compatibility.get("exactGameVersions") or []
    if not isinstance(exact_versions, list):
        raise ValueError(f"Entrada '{entry_id}' com exactGameVersions invalido")
    exact_versions = [str(item).strip() for item in exact_versions if str(item).strip()]
    if min_version:
        normalized["minGameVersion"] = min_version
    if max_version:
        normalized["maxGameVersion"] = max_version
    if exact_versions:
        normalized["exactGameVersions"] = exact_versions
    return normalized


def normalize_variant(variant: dict, entry_id: str) -> dict:
    if not isinstance(variant, dict):
        raise ValueError(f"Entrada '{entry_id}' com variant invalida")
    normalized = {
        "id": str(variant.get("id") or "").strip(),
        "label": str(variant.get("label") or "").strip(),
        "downloadUrl": str(variant.get("downloadUrl") or "").strip(),
        "packageVersion": str(variant.get("packageVersion") or variant.get("version") or "").strip(),
        "contentRevision": str(variant.get("contentRevision") or "").strip(),
        "compatibility": normalize_compatibility(variant.get("compatibility"), entry_id),
    }
    if not normalized["id"] or not normalized["downloadUrl"]:
        raise ValueError(f"Entrada '{entry_id}' com variant sem id ou downloadUrl")
    normalized = {key: value for key, value in normalized.items() if value not in ("", [], {})}
    return normalized


def load_artwork_manifest() -> dict:
    try:
        return json.loads(ARTWORK_MANIFEST_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}


def save_artwork_manifest(manifest: dict) -> None:
    ARTWORK_MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    ARTWORK_MANIFEST_PATH.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def is_valid_thumb(path: Path) -> bool:
    if not path.exists():
        return False
    try:
        with Image.open(path) as image:
            return image.size == (THUMB_SIZE, THUMB_SIZE)
    except Exception:
        return False


def write_normalized_thumb(payload: bytes, destination_path: Path) -> None:
    with Image.open(io.BytesIO(payload)) as image:
        image = image.convert("RGBA")
        image.thumbnail((THUMB_SIZE, THUMB_SIZE), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", (THUMB_SIZE, THUMB_SIZE), (0, 0, 0, 0))
        offset_x = (THUMB_SIZE - image.width) // 2
        offset_y = (THUMB_SIZE - image.height) // 2
        canvas.alpha_composite(image, (offset_x, offset_y))
        destination_path.parent.mkdir(parents=True, exist_ok=True)
        canvas.save(destination_path, format="PNG")


def mirror_thumbnail(entry: dict, public_base_url: str, manifest: dict) -> dict:
    entry_id = str(entry.get("id") or "").strip()
    if not entry_id:
        return entry

    primary_url, fallback_url = choose_artwork_urls(entry)
    if not primary_url:
        return entry

    thumb_path = DIST_THUMBS_DIR / f"{entry_id}.png"
    manifest_entry = manifest.get(entry_id) or {}
    current = (
        manifest_entry.get("primaryUrl") == primary_url
        and manifest_entry.get("fallbackUrl") == fallback_url
        and manifest_entry.get("size") == THUMB_SIZE
        and is_valid_thumb(thumb_path)
    )

    if not current:
        success = False
        for candidate in (primary_url, fallback_url):
            if not candidate:
                continue
            try:
                payload = download_bytes(candidate)
                write_normalized_thumb(payload, thumb_path)
                success = is_valid_thumb(thumb_path)
                if success:
                    break
            except Exception:
                success = False
        if not success:
            manifest.pop(entry_id, None)
            return entry

    manifest[entry_id] = {
        "primaryUrl": primary_url,
        "fallbackUrl": fallback_url,
        "size": THUMB_SIZE,
    }
    entry["thumbnailUrl"] = f"{public_base_url}thumbs/{entry_id}.png"
    return entry


def normalize_entry(entry: dict, defaults: dict, public_base_url: str, artwork_manifest: dict) -> dict:
    merged = dict(defaults)
    merged.update(entry)

    missing = [field for field in REQUIRED_FIELDS if not merged.get(field)]
    if missing:
        raise ValueError(f"Entrada '{entry.get('id', '<sem id>')}' sem campos obrigatorios: {', '.join(missing)}")

    merged["titleId"] = str(merged["titleId"]).upper()
    merged.setdefault("summary", "")
    merged.setdefault("author", defaults.get("author", "M.I.L."))
    merged.setdefault("detailsUrl", defaults.get("detailsUrl", "https://miltraducoes.com/"))
    merged.setdefault("language", defaults.get("language", "pt-BR"))
    merged.setdefault("featured", False)
    merged.setdefault("tags", [])
    merged = enrich_entry_with_titledb(merged)

    official_name = str(merged.get("_titledbName") or "").strip()
    merged["name"] = official_name or str(merged.get("name") or "").strip()
    if not merged.get("name"):
        raise ValueError(f"Entrada '{merged['id']}' sem nome oficial ou nome manual")

    content_types = merged.get("contentTypes") or default_content_types_for_section(merged.get("section"))
    if not isinstance(content_types, list):
        content_types = default_content_types_for_section(merged.get("section"))
    merged["contentTypes"] = []
    for item in content_types:
        normalized = normalize_content_type(item)
        if normalized and normalized not in merged["contentTypes"]:
            merged["contentTypes"].append(normalized)

    intro_pt = str(merged.get("introPtBr") or merged.get("intro") or merged.get("_titledbIntroPtBr") or merged.get("_titledbIntroEnUs") or "").strip()
    intro_en = str(merged.get("introEnUs") or merged.get("_titledbIntroEnUs") or intro_pt).strip()
    summary_pt = str(merged.get("summaryPtBr") or merged.get("summary") or "").strip()
    summary_en = str(merged.get("summaryEnUs") or summary_pt).strip()

    merged["introPtBr"] = intro_pt
    merged["introEnUs"] = intro_en
    merged["intro"] = intro_pt
    merged["summaryPtBr"] = summary_pt
    merged["summaryEnUs"] = summary_en
    merged["summary"] = summary_pt

    merged.pop("_titledbName", None)
    merged.pop("_titledbIntroPtBr", None)
    merged.pop("_titledbIntroEnUs", None)
    merged = mirror_thumbnail(merged, public_base_url, artwork_manifest)

    merged["compatibility"] = normalize_compatibility(merged.get("compatibility"), merged["id"])

    variants = merged.get("variants") or []
    if not isinstance(variants, list):
        raise ValueError(f"Entrada '{merged['id']}' com variants invalido")
    merged["variants"] = [normalize_variant(variant, merged["id"]) for variant in variants]

    merged["downloadUrl"] = str(merged.get("downloadUrl") or "").strip()
    if not merged["downloadUrl"] and not merged["variants"]:
        raise ValueError(f"Entrada '{merged['id']}' sem downloadUrl e sem variants")
    if not merged["downloadUrl"]:
        merged.pop("downloadUrl", None)

    return merged


def build_index() -> dict:
    source = load_source()
    defaults = source.get("defaults") or {}
    public_base_url = normalize_public_base_url(source)
    artwork_manifest = load_artwork_manifest()
    entries = [normalize_entry(entry, defaults, public_base_url, artwork_manifest) for entry in source.get("entries", [])]
    entries.sort(key=lambda item: (item.get("section", ""), item.get("name", "").lower()))
    save_artwork_manifest(artwork_manifest)

    generated_at = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    catalog_revision = source.get(
        "catalogRevision",
        generated_at.replace("-", ".").replace(":", ".").replace("T", ".").replace("Z", ""),
    )

    return {
        "catalogName": source.get("catalogName", "MIL Traducoes"),
        "channel": source.get("channel", "stable"),
        "schemaVersion": source.get("schemaVersion", "1.0"),
        "catalogRevision": catalog_revision,
        "generatedAt": generated_at,
        "thumbPackRevision": catalog_revision,
        "thumbPackUrl": f"{public_base_url}thumbs-pack.zip",
        "entries": entries,
    }


def write_output(index_data: dict) -> None:
    serialized = json.dumps(index_data, ensure_ascii=False, indent=2) + "\n"
    for path in DIST_PATHS:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(serialized, encoding="utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def write_thumb_pack(index_data: dict) -> None:
    DIST_THUMBS_PACK_PATH.parent.mkdir(parents=True, exist_ok=True)

    manifest = {
        "schemaVersion": 1,
        "revision": index_data.get("thumbPackRevision") or index_data.get("catalogRevision", ""),
        "packUrl": index_data.get("thumbPackUrl", ""),
        "imageSize": THUMB_SIZE,
        "entries": {},
    }

    with zipfile.ZipFile(DIST_THUMBS_PACK_PATH, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        if DIST_THUMBS_DIR.exists():
            for thumb_path in sorted(DIST_THUMBS_DIR.glob("*.png")):
                archive.write(thumb_path, arcname=thumb_path.name)
                manifest["entries"][thumb_path.stem] = thumb_path.name

        archive.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")

    manifest["packSha256"] = "sha256:" + sha256_file(DIST_THUMBS_PACK_PATH)
    index_data["thumbPackSha256"] = manifest["packSha256"]
    write_output(index_data)

    DIST_THUMBS_MANIFEST_PATH.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    index_data = build_index()
    write_output(index_data)
    write_thumb_pack(index_data)
    print(f"Index gerado com {len(index_data['entries'])} entradas.")
    print(f"Revisao: {index_data['catalogRevision']}")
    print(f"Saida principal: {DIST_PATHS[0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
