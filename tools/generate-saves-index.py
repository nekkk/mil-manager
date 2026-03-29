#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import io
import json
import os
import re
import time
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "catalog-source"
ENTRIES_DIR = SOURCE_DIR / "entries"
METADATA_PATH = SOURCE_DIR / "catalog-metadata.json"
SAVES_SOURCES_PATH = SOURCE_DIR / "saves-sources.json"
MANUAL_SAVES_DIR = SOURCE_DIR / "manual-saves"
DIST_DIR = ROOT / "dist"
DIST_SAVE_PACKS_DIR = DIST_DIR / "save-packs"
DIST_DELIVERY_DIR = DIST_DIR / "delivery"
DIST_DELIVERY_SAVES_DIR = DIST_DELIVERY_DIR / "saves"
DIST_SAVES_INDEX_PATH = DIST_DIR / "saves-index.json"
CACHE_DIR = ROOT / ".cache" / "saves"
HTTP_CACHE_DIR = CACHE_DIR / "http"
TITLEDB_NAME_SOURCES = {
    "pt": "https://raw.githubusercontent.com/blawar/titledb/master/BR.pt.json",
    "en": "https://raw.githubusercontent.com/blawar/titledb/master/US.en.json",
}
HTTP_CACHE_TTL_SECONDS = 24 * 60 * 60
TITLEDB_CACHE_TTL_SECONDS = 24 * 60 * 60
DEFAULT_PUBLIC_BASE_URL = "https://nekkk.github.io/mil-manager-delivery/"

LANGUAGE_HINTS = {
    "ESP": "es",
    "EN": "en",
    "ENG": "en",
    "PT": "pt",
    "PTBR": "pt-BR",
}

MOJIBAKE_REPLACEMENTS = {
    "Ã¢â€žÂ¢": "TM",
    "Ã¢â‚¬Â¢": "•",
    "Ã¢â‚¬â€œ": "-",
    "Ã¢â‚¬â€": "-",
    "Ã¢â‚¬Ëœ": "'",
    "Ã¢â‚¬â„¢": "'",
    "Ã¢â‚¬Å“": "\"",
    "Ã¢â‚¬Â": "\"",
    "ÃƒÂ¡": "á",
    "ÃƒÂ¢": "â",
    "ÃƒÂ£": "ã",
    "ÃƒÂ¤": "ä",
    "ÃƒÂ§": "ç",
    "ÃƒÂ©": "é",
    "ÃƒÂª": "ê",
    "ÃƒÂ­": "í",
    "ÃƒÂ³": "ó",
    "ÃƒÂ´": "ô",
    "ÃƒÂµ": "õ",
    "ÃƒÂº": "ú",
    "Ãƒâ€°": "É",
    "Ãƒâ€œ": "Ó",
    "ÃƒÅ¡": "Ú",
}

_TITLEDB_BY_LOCALE: dict[str, dict[str, dict]] = {}
_TITLEDB_NAME_INDEX: dict[str, list[tuple[str, str]]] | None = None


@dataclass
class SavePayloadFile:
    path: str
    data: bytes


@dataclass
class SaveCandidate:
    title_id: str
    name: str
    label: str
    category: str
    author: str
    language: str
    updated_at: str
    source: str
    origin_url: str
    payload_files: list[SavePayloadFile] = field(default_factory=list)
    priority_rank: int = 999


@dataclass
class MergedSaveVariant:
    title_id: str
    name: str
    label: str
    category: str
    author: str
    language: str
    updated_at: str
    origins: list[str]
    payload_files: list[SavePayloadFile]
    payload_hash: str
    priority_rank: int = 999


def now_utc_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def normalize_title_id(value: str) -> str:
    return str(value or "").strip().upper()


def normalize_name_key(value: str) -> str:
    value = repair_mojibake(str(value or "")).lower()
    value = re.sub(r"[^a-z0-9]+", "", value)
    return value


def repair_mojibake(value: str) -> str:
    repaired = str(value or "")
    for source, target in MOJIBAKE_REPLACEMENTS.items():
        repaired = repaired.replace(source, target)
    return repaired.strip()


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def http_cache_path(url: str) -> Path:
    digest = hashlib.sha256(url.encode("utf-8")).hexdigest()
    return HTTP_CACHE_DIR / f"{digest}.bin"


def http_get_bytes_cached(url: str, ttl_seconds: int = HTTP_CACHE_TTL_SECONDS) -> bytes:
    cache_path = http_cache_path(url)
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    if cache_path.exists():
        age_seconds = time.time() - cache_path.stat().st_mtime
        if age_seconds <= ttl_seconds:
            return cache_path.read_bytes()

    parsed = urllib.parse.urlsplit(url)
    normalized_url = urllib.parse.urlunsplit(
        (
            parsed.scheme,
            parsed.netloc,
            urllib.parse.quote(parsed.path, safe="/:%._-~"),
            parsed.query,
            parsed.fragment,
        )
    )
    request = urllib.request.Request(normalized_url, headers={"User-Agent": "MILSaveIndexGenerator/1.0"})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read()
    cache_path.write_bytes(data)
    return data


def http_get_text_cached(url: str, ttl_seconds: int = HTTP_CACHE_TTL_SECONDS) -> str:
    return http_get_bytes_cached(url, ttl_seconds=ttl_seconds).decode("utf-8", "replace")


def http_get_json_cached(url: str, ttl_seconds: int = HTTP_CACHE_TTL_SECONDS) -> dict | list:
    return json.loads(http_get_text_cached(url, ttl_seconds=ttl_seconds))


def normalize_public_base_url(metadata: dict) -> str:
    base_url = (
        str(metadata.get("publicBaseUrl") or "").strip()
        or os.environ.get("MIL_CATALOG_PUBLIC_BASE_URL", "").strip()
        or DEFAULT_PUBLIC_BASE_URL
    )
    if not base_url.endswith("/"):
        base_url += "/"
    return base_url


def make_logical_asset_id(asset_type: str, *parts: str) -> str:
    payload = "::".join([asset_type, *[str(part).strip().lower() for part in parts if str(part).strip()]])
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()[:32]


def make_delivery_relative_path(group: str, asset_id: str, extension: str) -> Path:
    normalized_extension = extension.lstrip(".")
    return Path("delivery") / group / asset_id[:2] / asset_id[2:4] / f"{asset_id}.{normalized_extension}"


def load_catalog_entries() -> tuple[dict, list[dict]]:
    metadata = load_json(METADATA_PATH) if METADATA_PATH.exists() else {}
    entries: list[dict] = []
    if ENTRIES_DIR.exists():
        for path in sorted(ENTRIES_DIR.glob("*.json")):
            item = load_json(path)
            if isinstance(item, dict):
                entries.append(item)
    return metadata, entries


def normalize_path(path: str) -> str:
    return path.replace("\\", "/").lstrip("./")


def sanitize_folder_name(value: str) -> str:
    cleaned = repair_mojibake(value)
    cleaned = re.sub(r'[<>:"/\\\\|?*]+', "_", cleaned)
    cleaned = re.sub(r"\s+", " ", cleaned).strip(" .")
    return cleaned or "MIL Save"


def save_category_from_label(label: str) -> str:
    haystack = label.lower()
    if "100%" in haystack or "complete" in haystack or "story complete" in haystack:
        return "complete"
    if "starter" in haystack or "start" in haystack:
        return "starter"
    if "event" in haystack or "bcat" in haystack:
        return "event"
    if "ng+" in haystack or "new game+" in haystack:
        return "ngplus"
    if "unlock" in haystack:
        return "unlocked"
    if "mod" in haystack:
        return "modded"
    return "other"


def slugify(value: str) -> str:
    value = repair_mojibake(value).lower()
    value = re.sub(r"[^a-z0-9]+", "-", value).strip("-")
    return value or "save"


def normalize_manual_variant_id(value: str) -> str:
    return slugify(value).replace("-", "_")


def load_titledb_names(locale: str) -> dict[str, dict]:
    cached = _TITLEDB_BY_LOCALE.get(locale)
    if cached is not None:
        return cached

    cache_path = CACHE_DIR / f"titledb-{locale}.json"
    should_refresh = True
    if cache_path.exists():
        age_seconds = time.time() - cache_path.stat().st_mtime
        should_refresh = age_seconds > TITLEDB_CACHE_TTL_SECONDS

    if should_refresh:
        data = http_get_text_cached(TITLEDB_NAME_SOURCES[locale], ttl_seconds=TITLEDB_CACHE_TTL_SECONDS)
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_path.write_text(data, encoding="utf-8")

    raw = json.loads(cache_path.read_text(encoding="utf-8"))
    by_title_id: dict[str, dict] = {}
    for item in raw.values():
        if not isinstance(item, dict):
            continue
        title_id = normalize_title_id(item.get("id") or "")
        if title_id:
            by_title_id[title_id] = item

    _TITLEDB_BY_LOCALE[locale] = by_title_id
    return by_title_id


def load_titledb_name_index() -> dict[str, list[tuple[str, str]]]:
    global _TITLEDB_NAME_INDEX
    if _TITLEDB_NAME_INDEX is not None:
        return _TITLEDB_NAME_INDEX

    mapping: dict[str, list[tuple[str, str]]] = {}
    for locale in ("pt", "en"):
        for title_id, item in load_titledb_names(locale).items():
            name = repair_mojibake(str(item.get("name") or "").strip())
            if not name:
                continue
            key = normalize_name_key(name)
            if not key:
                continue
            mapping.setdefault(key, [])
            if (title_id, name) not in mapping[key]:
                mapping[key].append((title_id, name))

    _TITLEDB_NAME_INDEX = mapping
    return mapping


def normalize_category_token(value: object, default: str = "other") -> str:
    token = slugify(str(value or "")).replace("_", "-")
    return token or default


def load_manual_save_sidecar(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Invalid manual save metadata: {path}") from exc


def resolve_title_id_by_name(name: str) -> tuple[str, str] | tuple[None, None]:
    candidates = load_titledb_name_index().get(normalize_name_key(name), [])
    if len(candidates) == 1:
        return candidates[0]
    return None, None


def resolve_title_name(title_id: str, catalog_name_map: dict[str, str], fallback: str = "") -> str:
    if catalog_name_map.get(title_id):
        return repair_mojibake(catalog_name_map[title_id])

    for locale in ("pt", "en"):
        item = load_titledb_names(locale).get(title_id, {})
        name = repair_mojibake(str(item.get("name") or "").strip())
        if name:
            return name

    return repair_mojibake(fallback) or title_id


def build_catalog_name_map(entries: list[dict]) -> dict[str, str]:
    result: dict[str, str] = {}
    for entry in entries:
        title_id = normalize_title_id(entry.get("titleId") or "")
        name = str(entry.get("name") or "").strip()
        if title_id and name and title_id not in result:
            result[title_id] = name
    return result


def make_payload_hash(files: list[SavePayloadFile]) -> str:
    digest = hashlib.sha256()
    for item in sorted(files, key=lambda file: file.path):
        digest.update(item.path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(item.data)
        digest.update(b"\0")
    return digest.hexdigest()


def parse_viren_filename(filename: str) -> tuple[str, str, str, str, str]:
    stem = filename[:-4] if filename.lower().endswith(".zip") else filename
    title_match = re.search(r"\[([0-9A-Fa-f]{16})\]", stem)
    if not title_match:
        return "", "", "", "", ""

    title_id = normalize_title_id(title_match.group(1))
    bracket_tokens = re.findall(r"\[([^\]]+)\]", stem)
    author = ""
    language = ""
    label = "Save"
    for token in bracket_tokens:
        token_clean = repair_mojibake(token.strip())
        token_upper = token_clean.upper().replace("-", "").replace("_", "")
        if re.fullmatch(r"[0-9A-Fa-f]{16}", token_clean):
            continue
        if re.fullmatch(r"\d{3,}", token_clean):
            continue
        if token_upper in LANGUAGE_HINTS:
            language = LANGUAGE_HINTS[token_upper]
            continue
        if label == "Save":
            label = token_clean
        elif not author:
            author = token_clean

    game_name = repair_mojibake(re.sub(r"\s*\[[^\]]+\]", "", stem).strip())
    game_name = re.sub(r"\s+", " ", game_name).strip(" -")
    return title_id, game_name, label, author, language


def load_zip_payload_files(url: str) -> list[SavePayloadFile]:
    data = http_get_bytes_cached(url)
    with zipfile.ZipFile(io.BytesIO(data), "r") as archive:
        files: list[SavePayloadFile] = []
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = normalize_path(info.filename)
            if not name:
                continue
            files.append(SavePayloadFile(path=name, data=archive.read(info)))
        return files


def iter_viren_candidates(source_config: dict, catalog_name_map: dict[str, str]) -> list[SaveCandidate]:
    repo_contents_url = str(source_config.get("repoContentsUrl") or "").strip()
    if not repo_contents_url:
        return []

    items = http_get_json_cached(repo_contents_url)
    candidates: list[SaveCandidate] = []
    for item in items:
        if item.get("type") != "file":
            continue
        name = str(item.get("name") or "")
        if not name.lower().endswith(".zip"):
            continue
        title_id, game_name, label, author, language = parse_viren_filename(name)
        if not title_id:
            continue
        download_url = str(item.get("download_url") or "").strip()
        if not download_url:
            continue
        payload_files = load_zip_payload_files(download_url)
        if not payload_files:
            continue
        resolved_name = resolve_title_name(title_id, catalog_name_map, fallback=game_name)
        candidates.append(
            SaveCandidate(
                title_id=title_id,
                name=resolved_name,
                label=repair_mojibake(label),
                category=save_category_from_label(label),
                author=repair_mojibake(author) or "Viren070",
                language=language,
                updated_at="",
                source="viren070",
                origin_url=download_url,
                payload_files=payload_files,
            )
        )
    return candidates


def parse_ghost_dir_name(name: str) -> tuple[str, str]:
    match = re.match(r"^(.*?)\s*\(([0-9A-Fa-f]{16})\)\s*$", name)
    if not match:
        return "", ""
    return normalize_title_id(match.group(2)), repair_mojibake(match.group(1).strip())


def iter_ghostlydark_candidates(source_config: dict, catalog_name_map: dict[str, str]) -> list[SaveCandidate]:
    repo_contents_url = str(source_config.get("repoContentsUrl") or "").strip()
    if not repo_contents_url:
        return []

    roots = http_get_json_cached(repo_contents_url)
    candidates: list[SaveCandidate] = []
    for root in roots:
        if root.get("type") != "dir":
            continue
        title_id, fallback_name = parse_ghost_dir_name(str(root.get("name") or ""))
        if not title_id:
            continue
        children = http_get_json_cached(root["url"])
        for child in children:
            download_url = str(child.get("download_url") or "").strip()
            if not download_url:
                continue
            payload_files: list[SavePayloadFile] = []
            label = repair_mojibake(Path(str(child.get("name") or "")).stem) or "Backup"
            if str(child.get("name") or "").lower().endswith(".zip"):
                payload_files = load_zip_payload_files(download_url)
            elif child.get("type") == "file":
                payload_files = [SavePayloadFile(path=normalize_path(str(child.get("name") or "")),
                                                 data=http_get_bytes_cached(download_url))]
            if not payload_files:
                continue
            candidates.append(
                SaveCandidate(
                    title_id=title_id,
                    name=resolve_title_name(title_id, catalog_name_map, fallback=fallback_name),
                    label=label,
                    category=save_category_from_label(label),
                    author="GhostlyDark",
                    language="",
                    updated_at="",
                    source="ghostlydark",
                    origin_url=download_url,
                    payload_files=payload_files,
                )
            )
    return candidates


def collect_repo_dir_files(contents_url: str, prefix: str = "") -> list[SavePayloadFile]:
    items = http_get_json_cached(contents_url)
    results: list[SavePayloadFile] = []
    for item in items:
        item_type = item.get("type")
        name = str(item.get("name") or "")
        if name in {"README.md", ".lock"}:
            continue
        if item_type == "dir":
            results.extend(collect_repo_dir_files(item["url"], prefix=normalize_path(f"{prefix}/{name}")))
        elif item_type == "file":
            download_url = str(item.get("download_url") or "").strip()
            if download_url:
                results.append(
                    SavePayloadFile(
                        path=normalize_path(f"{prefix}/{name}"),
                        data=http_get_bytes_cached(download_url),
                    )
                )
    return results


def iter_niemasd_candidates(source_config: dict, catalog_name_map: dict[str, str]) -> list[SaveCandidate]:
    repo_contents_url = str(source_config.get("repoContentsUrl") or "").strip()
    if not repo_contents_url:
        return []

    roots = http_get_json_cached(repo_contents_url)
    candidates: list[SaveCandidate] = []
    for root in roots:
        if root.get("type") != "dir":
            continue
        folder_name = repair_mojibake(str(root.get("name") or "").strip())
        title_id, resolved_name = resolve_title_id_by_name(folder_name)
        if not title_id:
            continue

        readme_url = ""
        children = http_get_json_cached(root["url"])
        for child in children:
            if str(child.get("name") or "").lower() == "readme.md":
                readme_url = str(child.get("download_url") or "").strip()
                break

        if not readme_url:
            continue
        readme = http_get_text_cached(readme_url)
        if "Type:** Switch" not in readme and "**Type:** Switch" not in readme:
            continue

        payload_files = collect_repo_dir_files(root["url"])
        if not payload_files:
            continue

        candidates.append(
            SaveCandidate(
                title_id=title_id,
                name=resolve_title_name(title_id, catalog_name_map, fallback=resolved_name or folder_name),
                label="Switch Save",
                category="other",
                author="niemasd",
                language="",
                updated_at="",
                source="niemasd",
                origin_url=root.get("html_url") or root.get("url") or "",
                payload_files=payload_files,
                priority_rank=999,
            )
        )
    return candidates


def load_manual_variant_payload_files(variant_dir: Path) -> list[SavePayloadFile]:
    payload_root = variant_dir / "payload"
    base_dir = payload_root if payload_root.exists() else variant_dir
    payload_files: list[SavePayloadFile] = []
    for path in sorted(base_dir.rglob("*")):
        if not path.is_file():
            continue
        relative_path = path.relative_to(base_dir).as_posix()
        if relative_path in {"metadata.json", ".gitkeep"}:
            continue
        payload_files.append(SavePayloadFile(path=relative_path, data=path.read_bytes()))
    return payload_files


def iter_manual_save_candidates(catalog_name_map: dict[str, str]) -> list[SaveCandidate]:
    if not MANUAL_SAVES_DIR.exists():
        return []

    candidates: list[SaveCandidate] = []
    for title_dir in sorted(MANUAL_SAVES_DIR.iterdir()):
        if not title_dir.is_dir():
            continue
        title_id = normalize_title_id(title_dir.name)
        if not re.fullmatch(r"[0-9A-F]{16}", title_id):
            continue

        for zip_path in sorted(title_dir.glob("*.zip")):
            metadata = load_manual_save_sidecar(zip_path.with_suffix(".json"))
            payload_files = load_zip_payload_files(zip_path.as_uri())
            if not payload_files:
                continue
            variant_id = normalize_manual_variant_id(zip_path.stem)
            label = repair_mojibake(str(metadata.get("label") or variant_id).strip()) or variant_id
            category = normalize_category_token(metadata.get("category") or save_category_from_label(label))
            candidates.append(
                SaveCandidate(
                    title_id=title_id,
                    name=resolve_title_name(title_id, catalog_name_map, fallback=str(metadata.get("name") or title_id)),
                    label=label,
                    category=category,
                    author=repair_mojibake(str(metadata.get("author") or "Manual").strip()) or "Manual",
                    language=str(metadata.get("language") or "").strip(),
                    updated_at=str(metadata.get("updatedAt") or "").strip(),
                    source="manual",
                    origin_url=f"manual://saves/{title_id}/{variant_id}",
                    payload_files=payload_files,
                    priority_rank=0,
                )
            )

        for variant_dir in sorted(title_dir.iterdir()):
            if not variant_dir.is_dir():
                continue
            metadata = load_manual_save_sidecar(variant_dir / "metadata.json")
            payload_files = load_manual_variant_payload_files(variant_dir)
            if not payload_files:
                continue
            variant_id = normalize_manual_variant_id(variant_dir.name)
            label = repair_mojibake(str(metadata.get("label") or variant_id).strip()) or variant_id
            category = normalize_category_token(metadata.get("category") or save_category_from_label(label))
            candidates.append(
                SaveCandidate(
                    title_id=title_id,
                    name=resolve_title_name(title_id, catalog_name_map, fallback=str(metadata.get("name") or title_id)),
                    label=label,
                    category=category,
                    author=repair_mojibake(str(metadata.get("author") or "Manual").strip()) or "Manual",
                    language=str(metadata.get("language") or "").strip(),
                    updated_at=str(metadata.get("updatedAt") or "").strip(),
                    source="manual",
                    origin_url=f"manual://saves/{title_id}/{variant_id}",
                    payload_files=payload_files,
                    priority_rank=0,
                )
            )

    return candidates


def merge_candidates(candidates: list[SaveCandidate]) -> list[MergedSaveVariant]:
    merged: dict[tuple[str, str], MergedSaveVariant] = {}
    for candidate in candidates:
        if not candidate.payload_files:
            continue
        payload_hash = make_payload_hash(candidate.payload_files)
        key = (candidate.title_id, payload_hash)
        target = merged.get(key)
        if target is None:
            target = MergedSaveVariant(
                title_id=candidate.title_id,
                name=candidate.name,
                label=candidate.label,
                category=candidate.category,
                author=candidate.author,
                language=candidate.language,
                updated_at=candidate.updated_at,
                origins=[candidate.source],
                payload_files=candidate.payload_files,
                payload_hash=payload_hash,
                priority_rank=candidate.priority_rank,
            )
            merged[key] = target
        else:
            target.origins = sorted(set(target.origins + [candidate.source]))
            if candidate.priority_rank < target.priority_rank:
                target.name = candidate.name or target.name
                target.label = candidate.label or target.label
                target.category = candidate.category or target.category
                target.author = candidate.author or target.author
                target.language = candidate.language or target.language
                target.updated_at = candidate.updated_at or target.updated_at
                target.priority_rank = candidate.priority_rank
            elif len(candidate.label) > len(target.label):
                target.label = candidate.label
            if not target.author and candidate.author:
                target.author = candidate.author
            if not target.language and candidate.language:
                target.language = candidate.language
            if not target.updated_at and candidate.updated_at:
                target.updated_at = candidate.updated_at
    return sorted(merged.values(), key=lambda item: (item.title_id, item.label.lower(), item.payload_hash))


def public_base_url_for_path(base_url: str, relative_path: str) -> str:
    if not base_url.endswith("/"):
        base_url += "/"
    return base_url + relative_path.lstrip("/")


def build_pack_and_index(variants: list[MergedSaveVariant], public_base_url: str) -> dict:
    for root_dir in (DIST_SAVE_PACKS_DIR, DIST_DELIVERY_SAVES_DIR):
        if not root_dir.exists():
            continue
        for path in root_dir.rglob("*"):
            if path.is_file():
                path.unlink()
        for path in sorted(root_dir.rglob("*"), reverse=True):
            if path.is_dir():
                try:
                    path.rmdir()
                except OSError:
                    pass

    titles: dict[str, dict] = {}
    seen_variant_folder_names: dict[str, set[str]] = {}

    for variant in variants:
        title_bucket = titles.setdefault(
            variant.title_id,
            {
                "titleId": variant.title_id,
                "name": variant.name,
                "categories": set(),
                "variants": [],
            },
        )
        title_bucket["categories"].add(variant.category)

        variant_slug = slugify(variant.label)
        variant_id = f"{variant_slug}-{variant.payload_hash[:8]}"
        asset_id = make_logical_asset_id("save-pack", variant.title_id, variant_id, variant.payload_hash)
        relative_zip_path = make_delivery_relative_path("saves", asset_id, "zip")
        output_zip_path = DIST_DIR / relative_zip_path
        output_zip_path.parent.mkdir(parents=True, exist_ok=True)

        title_folder = sanitize_folder_name(variant.name)
        variant_folder = sanitize_folder_name(variant.label)
        used_names = seen_variant_folder_names.setdefault(variant.title_id, set())
        if variant_folder in used_names:
            variant_folder = f"{variant_folder}-{variant.payload_hash[:8]}"
        used_names.add(variant_folder)

        with zipfile.ZipFile(output_zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for file_item in sorted(variant.payload_files, key=lambda item: item.path):
                archive_path = normalize_path(f"JKSV/{title_folder}/{variant_folder}/{file_item.path}")
                info = zipfile.ZipInfo(archive_path)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.date_time = (2026, 1, 1, 0, 0, 0)
                archive.writestr(info, file_item.data)

        title_bucket["variants"].append(
            {
                "id": variant_id,
                "label": variant.label,
                "category": variant.category,
                "saveKind": "account",
                "layoutType": "jksv-backup",
                "platform": "switch",
                "author": variant.author,
                "language": variant.language,
                "updatedAt": variant.updated_at,
                "assetId": asset_id,
                "assetType": "save-pack",
                "relativePath": relative_zip_path.as_posix(),
                "contentHash": "sha256:" + hashlib.sha256(output_zip_path.read_bytes()).hexdigest(),
                "sha256": "sha256:" + hashlib.sha256(output_zip_path.read_bytes()).hexdigest(),
                "size": output_zip_path.stat().st_size,
                "origins": sorted(variant.origins),
            }
        )

    output_titles = []
    for title_id in sorted(titles.keys()):
        bucket = titles[title_id]
        bucket["variants"].sort(key=lambda item: (item["label"].lower(), item["id"]))
        output_titles.append(
            {
                "titleId": bucket["titleId"],
                "name": bucket["name"],
                "categories": sorted(bucket["categories"]),
                "variants": bucket["variants"],
            }
        )

    return {"titles": output_titles}


def main() -> int:
    metadata, entries = load_catalog_entries()
    config = load_json(SAVES_SOURCES_PATH)
    public_base_url = normalize_public_base_url(metadata)
    catalog_name_map = build_catalog_name_map(entries)
    priorities = config.get("priorities") or {}
    sources = config.get("sources") or {}

    all_candidates: list[SaveCandidate] = []
    all_candidates.extend(iter_manual_save_candidates(catalog_name_map))
    if isinstance(sources.get("viren070"), dict) and sources["viren070"].get("enabled", False):
        all_candidates.extend(iter_viren_candidates(sources["viren070"], catalog_name_map))
    if isinstance(sources.get("ghostlydark"), dict) and sources["ghostlydark"].get("enabled", False):
        all_candidates.extend(iter_ghostlydark_candidates(sources["ghostlydark"], catalog_name_map))
    if isinstance(sources.get("niemasd"), dict) and sources["niemasd"].get("enabled", False):
        all_candidates.extend(iter_niemasd_candidates(sources["niemasd"], catalog_name_map))

    all_candidates.sort(key=lambda item: (normalize_title_id(item.title_id), int(priorities.get(item.source, 999)), item.label.lower()))
    merged_variants = merge_candidates(all_candidates)
    built = build_pack_and_index(merged_variants, public_base_url)

    payload = {
        "schemaVersion": "1.0",
        "generatedAt": now_utc_iso(),
        "generator": "MILSaveGamesAggregator",
        "catalogRevision": str(metadata.get("catalogRevision") or ""),
        "deliveryBaseUrl": public_base_url,
        "titles": built["titles"],
    }

    DIST_SAVES_INDEX_PATH.parent.mkdir(parents=True, exist_ok=True)
    DIST_SAVES_INDEX_PATH.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(f"Saves index gerado com {len(built['titles'])} titulos.")
    print(f"Saida principal: {DIST_SAVES_INDEX_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
