#!/usr/bin/env python3
import io
import base64
import hashlib
import json
import os
import time
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


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
DIST_DELIVERY_DIR = ROOT / "dist" / "delivery"
DIST_DELIVERY_PACKAGES_DIR = DIST_DELIVERY_DIR / "packages"
DIST_THUMBS_PACK_PATH = ROOT / "dist" / "thumbs-pack.zip"
DIST_THUMBS_MANIFEST_PATH = ROOT / "dist" / "thumbs-manifest.json"
ARTWORK_MANIFEST_PATH = CACHE_DIR / "artwork-manifest.json"
PACKAGE_CACHE_DIR = ROOT / ".cache" / "packages"
DEFAULT_PUBLIC_BASE_URL = "https://nekkk.github.io/mil-manager-catalog/"
THUMB_SIZE = 110

REQUIRED_FIELDS = ("id", "section", "titleId")
_TITLEDB_BY_LOCALE = {}
MIRROR_WARNINGS: list[str] = []


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


def make_logical_asset_id(asset_type: str, *parts: str) -> str:
    payload = "::".join([asset_type, *[str(part).strip().lower() for part in parts if str(part).strip()]])
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()[:32]


def asset_type_for_section(section: str) -> str:
    normalized = str(section or "").strip().lower()
    if normalized == "translations":
        return "translation-package"
    if normalized == "mods":
        return "mod-package"
    if normalized == "cheats":
        return "cheat-package"
    if normalized in ("savegames", "saves"):
        return "save-package"
    return "package"


def make_delivery_relative_path(group: str, asset_id: str, extension: str) -> Path:
    normalized_extension = extension.lstrip(".")
    return Path("delivery") / group / asset_id[:2] / asset_id[2:4] / f"{asset_id}.{normalized_extension}"


def infer_asset_extension(url: str) -> str:
    parsed = urllib.parse.urlparse(url)
    extension = Path(parsed.path).suffix.lower()
    return extension if extension else ".zip"


def http_post_json(url: str, payload: object) -> object:
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "User-Agent": "MILCatalogGenerator/1.0",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=90) as response:
        return json.loads(response.read().decode("utf-8", "replace"))


def is_mega_url(url: str) -> bool:
    return "mega.nz/" in url or "mega.co.nz/" in url


def extract_mega_id(url: str) -> str:
    file_pos = url.find("/file/")
    if file_pos != -1:
        id_begin = file_pos + 6
        separator = url.find("#", id_begin)
        return url[id_begin:] if separator == -1 else url[id_begin:separator]
    legacy_pos = url.find("#!")
    if legacy_pos != -1:
        id_begin = legacy_pos + 2
        separator = url.find("!", id_begin)
        return url[id_begin:] if separator == -1 else url[id_begin:separator]
    return ""


def extract_mega_node_key(url: str) -> str:
    modern_pos = url.find("#")
    if modern_pos != -1 and modern_pos + 1 < len(url):
        return url[modern_pos + 1 :]
    legacy_pos = url.rfind("!")
    if legacy_pos != -1 and legacy_pos + 1 < len(url):
        return url[legacy_pos + 1 :]
    return ""


def base64_url_decode(value: str) -> bytes:
    normalized = value.replace("-", "+").replace("_", "/")
    while len(normalized) % 4 != 0:
        normalized += "="
    return base64.b64decode(normalized)


def decode_mega_node_key(encoded_key: str) -> tuple[bytes, bytes]:
    decoded = base64_url_decode(encoded_key)
    if len(decoded) != 32:
        raise ValueError("Invalid MEGA key length")
    key = bytes(decoded[index] ^ decoded[index + 16] for index in range(16))
    iv = bytes(decoded[index + 16] if index < 8 else 0 for index in range(16))
    return key, iv


def resolve_mega_file(public_url: str) -> tuple[str, bytes, bytes]:
    file_id = extract_mega_id(public_url)
    encoded_key = extract_mega_node_key(public_url)
    if not file_id or not encoded_key:
        raise ValueError("Invalid MEGA file link")
    key, iv = decode_mega_node_key(encoded_key)
    response = http_post_json("https://g.api.mega.co.nz/cs", [{"a": "g", "g": 1, "p": file_id}])
    if not isinstance(response, list) or not response or not isinstance(response[0], dict):
        raise ValueError("Invalid MEGA response")
    direct_url = str(response[0].get("g") or "").strip()
    if not direct_url:
        raise ValueError("MEGA did not return a direct download URL")
    return direct_url, key, iv


def decrypt_mega_payload(payload: bytes, key: bytes, iv: bytes) -> bytes:
    cipher = Cipher(algorithms.AES(key), modes.CTR(iv))
    decryptor = cipher.decryptor()
    return decryptor.update(payload) + decryptor.finalize()


def cached_source_path(url: str, extension: str) -> Path:
    digest = hashlib.sha256(url.encode("utf-8")).hexdigest()
    return PACKAGE_CACHE_DIR / f"{digest}{extension}"


def download_asset_bytes(url: str) -> bytes:
    extension = infer_asset_extension(url)
    cache_path = cached_source_path(url, extension)
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    if cache_path.exists():
        return cache_path.read_bytes()

    if is_mega_url(url):
        direct_url, key, iv = resolve_mega_file(url)
        request = urllib.request.Request(direct_url, headers={"User-Agent": "MILCatalogGenerator/1.0"})
        with urllib.request.urlopen(request, timeout=180) as response:
            encrypted_payload = response.read()
        payload = decrypt_mega_payload(encrypted_payload, key, iv)
    else:
        request = urllib.request.Request(url, headers={"User-Agent": "MILCatalogGenerator/1.0"})
        with urllib.request.urlopen(request, timeout=180) as response:
            payload = response.read()

    cache_path.write_bytes(payload)
    return payload


def mirror_delivery_asset(source_url: str, asset_type: str, asset_id: str) -> tuple[str, str, int]:
    extension = infer_asset_extension(source_url)
    relative_path = make_delivery_relative_path("packages", asset_id, extension)
    output_path = ROOT / "dist" / relative_path
    output_path.parent.mkdir(parents=True, exist_ok=True)
    payload = download_asset_bytes(source_url)
    output_path.write_bytes(payload)
    return (
        f"sha256:{hashlib.sha256(payload).hexdigest()}",
        relative_path.as_posix(),
        len(payload),
    )


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


def normalize_variant(variant: dict, entry_id: str) -> dict | None:
    if not isinstance(variant, dict):
        raise ValueError(f"Entrada '{entry_id}' com variant invalida")
    normalized = {
        "id": str(variant.get("id") or "").strip(),
        "label": str(variant.get("label") or "").strip(),
        "assetId": str(variant.get("assetId") or "").strip(),
        "assetType": str(variant.get("assetType") or "").strip(),
        "contentHash": str(variant.get("contentHash") or "").strip(),
        "relativePath": str(variant.get("relativePath") or "").strip(),
        "downloadUrl": str(variant.get("downloadUrl") or "").strip(),
        "size": int(variant.get("size") or 0),
        "packageVersion": str(variant.get("packageVersion") or variant.get("version") or "").strip(),
        "contentRevision": str(variant.get("contentRevision") or "").strip(),
        "compatibility": normalize_compatibility(variant.get("compatibility"), entry_id),
    }
    if not normalized["id"] or (not normalized["downloadUrl"] and not normalized["relativePath"]):
        raise ValueError(f"Entrada '{entry_id}' com variant sem id e payload")
    if not normalized["assetType"]:
        normalized["assetType"] = "package-variant"
    if not normalized["assetId"]:
        normalized["assetId"] = make_logical_asset_id(
            normalized["assetType"],
            entry_id,
            normalized["id"],
            normalized["packageVersion"] or normalized["contentRevision"] or normalized["downloadUrl"] or normalized["relativePath"],
        )
    if normalized["downloadUrl"] and not normalized["relativePath"]:
        try:
            content_hash, relative_path, size = mirror_delivery_asset(
                normalized["downloadUrl"],
                normalized["assetType"],
                normalized["assetId"],
            )
            normalized["contentHash"] = normalized["contentHash"] or content_hash
            normalized["relativePath"] = relative_path
            normalized["size"] = int(normalized.get("size") or 0) or size
        except Exception as exc:
            MIRROR_WARNINGS.append(f"Variant {entry_id}/{normalized['id']}: {exc}")
    if normalized["downloadUrl"] and not normalized["relativePath"]:
        MIRROR_WARNINGS.append(
            f"Variant {entry_id}/{normalized['id']} skipped from public catalog because it could not be mirrored to delivery/"
        )
        return None
    normalized = {key: value for key, value in normalized.items() if value not in ("", [], {})}
    normalized.pop("downloadUrl", None)
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
    merged["variants"] = [variant for variant in (normalize_variant(variant, merged["id"]) for variant in variants) if variant]

    merged["assetType"] = str(merged.get("assetType") or "").strip() or asset_type_for_section(merged.get("section"))
    merged["assetId"] = str(merged.get("assetId") or "").strip() or make_logical_asset_id(
        merged["assetType"],
        merged["section"],
        merged["id"],
        merged["titleId"],
    )
    merged["contentHash"] = str(merged.get("contentHash") or "").strip()
    merged["relativePath"] = str(merged.get("relativePath") or "").strip()
    merged["downloadUrl"] = str(merged.get("downloadUrl") or "").strip()
    if not merged["downloadUrl"] and not merged["relativePath"] and not merged["variants"]:
        raise ValueError(f"Entrada '{merged['id']}' sem downloadUrl/relativePath e sem variants")
    if merged["downloadUrl"] and not merged["relativePath"]:
        try:
            content_hash, relative_path, size = mirror_delivery_asset(
                merged["downloadUrl"],
                merged["assetType"],
                merged["assetId"],
            )
            merged["contentHash"] = merged["contentHash"] or content_hash
            merged["relativePath"] = relative_path
            merged["size"] = int(merged.get("size") or 0) or size
        except Exception as exc:
            MIRROR_WARNINGS.append(f"Entry {merged['id']}: {exc}")
    if merged["downloadUrl"] and not merged.get("relativePath"):
        MIRROR_WARNINGS.append(
            f"Entry {merged['id']} skipped from public catalog because it could not be mirrored to delivery/"
        )
        return None
    merged.pop("downloadUrl", None)
    merged["size"] = int(merged.get("size") or 0)
    if not merged["relativePath"]:
        merged.pop("relativePath", None)
    if not merged["contentHash"]:
        merged.pop("contentHash", None)
    if not merged["size"]:
        merged.pop("size", None)

    return merged


def build_index() -> dict:
    source = load_source()
    defaults = source.get("defaults") or {}
    public_base_url = normalize_public_base_url(source)
    artwork_manifest = load_artwork_manifest()
    if DIST_DELIVERY_PACKAGES_DIR.exists():
        for path in DIST_DELIVERY_PACKAGES_DIR.rglob("*"):
            if path.is_file():
                try:
                    path.unlink()
                except FileNotFoundError:
                    pass
        for path in sorted(DIST_DELIVERY_PACKAGES_DIR.rglob("*"), reverse=True):
            if path.is_dir():
                try:
                    path.rmdir()
                except OSError:
                    pass
    entries = [
        entry
        for entry in (normalize_entry(entry, defaults, public_base_url, artwork_manifest) for entry in source.get("entries", []))
        if entry is not None
    ]
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
        "deliveryBaseUrl": public_base_url,
        "thumbPackRevision": catalog_revision,
        "thumbPackAssetId": make_logical_asset_id("thumb-pack", catalog_revision),
        "thumbPackAssetType": "thumb-pack",
        "thumbPackRelativePath": "thumbs-pack.zip",
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
    index_data["thumbPackSize"] = DIST_THUMBS_PACK_PATH.stat().st_size
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
    if MIRROR_WARNINGS:
        print("Avisos de espelhamento:")
        for warning in MIRROR_WARNINGS:
            print(f"- {warning}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
