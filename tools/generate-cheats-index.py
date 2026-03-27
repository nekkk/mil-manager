#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import html
import json
import os
import re
import time
import urllib.parse
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "catalog-source"
ENTRIES_DIR = SOURCE_DIR / "entries"
METADATA_PATH = SOURCE_DIR / "catalog-metadata.json"
CHEAT_SOURCES_PATH = SOURCE_DIR / "cheats-sources.json"
DIST_DIR = ROOT / "dist"
DIST_CHEATS_DIR = DIST_DIR / "cheats"
DIST_CHEATS_INDEX_PATH = DIST_DIR / "cheats-index.json"
DIST_CHEATS_PACK_PATH = DIST_DIR / "cheats-pack.zip"
DIST_CHEATS_MANIFEST_PATH = DIST_DIR / "cheats-manifest.json"
CACHE_DIR = ROOT / ".cache" / "cheats"
HTTP_CACHE_DIR = CACHE_DIR / "http"
TITLEDB_NAME_SOURCES = {
    "pt": "https://raw.githubusercontent.com/blawar/titledb/master/BR.pt.json",
    "en": "https://raw.githubusercontent.com/blawar/titledb/master/US.en.json",
}
TITLEDB_NAME_CACHE_TTL_SECONDS = 24 * 60 * 60
HTTP_CACHE_TTL_SECONDS = 24 * 60 * 60
MAX_FETCH_WORKERS = 16
DEFAULT_PUBLIC_BASE_URL = "https://nekkk.github.io/mil-manager-catalog/"
TXT_PATH_PATTERNS = [
    re.compile(r"titles/(?P<title>[0-9A-Fa-f]{16})/cheats/(?P<build>[0-9A-Fa-f]{16})\.txt$"),
    re.compile(r"atmosphere/titles/(?P<title>[0-9A-Fa-f]{16})/cheats/(?P<build>[0-9A-Fa-f]{16})\.txt$"),
]
CHEAT_BLOCK_PATTERN = re.compile(r"^\[(?P<title>[^\]]+)\]\s*$", re.MULTILINE)
HTML_BREAK_RE = re.compile(r"<br\s*/?>", re.IGNORECASE)
TAG_RE = re.compile(r"<[^>]+>")

_TITLEDB_BY_LOCALE: dict[str, dict[str, dict]] = {}

MOJIBAKE_REPLACEMENTS = {
    "â„¢": "TM",
    "â€¢": "•",
    "â€“": "-",
    "â€”": "-",
    "â€˜": "'",
    "â€™": "'",
    "â€œ": "\"",
    "â€": "\"",
    "Ã¡": "á",
    "Ã¢": "â",
    "Ã£": "ã",
    "Ã¤": "ä",
    "Ã§": "ç",
    "Ã©": "é",
    "Ãª": "ê",
    "Ã­": "í",
    "Ã³": "ó",
    "Ã´": "ô",
    "Ãµ": "õ",
    "Ãº": "ú",
    "Ã‰": "É",
    "Ã“": "Ó",
    "Ãš": "Ú",
}


@dataclass
class CheatCandidate:
    title_id: str
    build_id: str
    source: str
    content: str
    categories: list[str]
    origin_url: str
    title: str = ""
    priority_rank: int = 999


@dataclass
class MergedCheatEntry:
    title_id: str
    build_id: str
    content: str
    content_hash: str
    sources: list[str] = field(default_factory=list)
    categories: list[str] = field(default_factory=list)
    origin_urls: list[str] = field(default_factory=list)
    priority_rank: int = 999
    title: str = ""


def now_utc_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def normalize_title_id(value: str) -> str:
    return str(value or "").strip().upper()


def repair_mojibake(value: str) -> str:
    repaired = str(value or "")
    for source, target in MOJIBAKE_REPLACEMENTS.items():
        repaired = repaired.replace(source, target)
    return repaired


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def http_get_text(url: str, accept: str | None = None) -> str:
    headers = {"User-Agent": "MILCheatsIndexGenerator/1.0"}
    if accept:
        headers["Accept"] = accept
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read().decode("utf-8", "replace")


def http_cache_path(url: str, accept: str | None) -> Path:
    key = json.dumps({"url": url, "accept": accept or ""}, sort_keys=True).encode("utf-8")
    digest = hashlib.sha256(key).hexdigest()
    return HTTP_CACHE_DIR / f"{digest}.txt"


def http_get_text_cached(url: str, accept: str | None = None, ttl_seconds: int = HTTP_CACHE_TTL_SECONDS) -> str:
    cache_path = http_cache_path(url, accept)
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    if cache_path.exists():
        age_seconds = time.time() - cache_path.stat().st_mtime
        if age_seconds <= ttl_seconds:
            return cache_path.read_text(encoding="utf-8")

    content = http_get_text(url, accept=accept)
    cache_path.write_text(content, encoding="utf-8")
    return content


def http_get_json(url: str, accept: str | None = None) -> dict:
    return json.loads(http_get_text_cached(url, accept=accept))


def normalize_public_base_url(source: dict) -> str:
    base_url = (
        str(source.get("publicBaseUrl") or "").strip()
        or os.environ.get("MIL_CATALOG_PUBLIC_BASE_URL", "").strip()
        or DEFAULT_PUBLIC_BASE_URL
    )
    if not base_url.endswith("/"):
        base_url += "/"
    return base_url


def normalize_cheat_text(text: str) -> str:
    normalized = text.replace("\r\n", "\n").replace("\r", "\n").replace("\ufeff", "")
    normalized = "\n".join(line.rstrip() for line in normalized.split("\n"))
    normalized = re.sub(r"\n{3,}", "\n\n", normalized).strip()
    return normalized + "\n" if normalized else ""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def count_cheat_blocks(text: str) -> int:
    return len(CHEAT_BLOCK_PATTERN.findall(text))


def choose_entry_title(categories: list[str]) -> str:
    normalized = set(categories)
    if "fps" in normalized and "graphics" in normalized:
        return "FPS / Graphics"
    if "graphics" in normalized:
        return "Graphics"
    if "fps" in normalized:
        return "FPS"
    if "community" in normalized:
        return "Community"
    return "General"


def infer_categories(source_name: str, title_hint: str, content: str) -> list[str]:
    haystack = f"{title_hint}\n{content}".lower()
    categories: list[str] = []

    if source_name == "chansey":
        categories.append("graphics")
        if "fps" in haystack:
            categories.append("fps")
    elif source_name == "cheatSlips":
        categories.append("community")
    else:
        categories.append("general")

    if "fps" in haystack and "fps" not in categories:
        categories.append("fps")
    if any(token in haystack for token in ("gfx", "resolution", "dynamic res", "graphic")) and "graphics" not in categories:
        categories.append("graphics")

    return categories


def make_cheat_file_text_from_titledb(build_data: dict) -> str:
    blocks: list[str] = []
    for cheat_hash in sorted(build_data.keys()):
        cheat = build_data[cheat_hash]
        if not isinstance(cheat, dict):
            continue
        title = str(cheat.get("title") or "").strip()
        source = str(cheat.get("source") or "").strip()
        if not source:
            continue
        header = title if title.startswith("[") else f"[{title or 'Cheat'}]"
        blocks.append(f"{header}\n{source.strip()}")
    return normalize_cheat_text("\n\n".join(blocks))


def download_to_cache(url: str, cache_path: Path) -> None:
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    content = http_get_text(url)
    cache_path.write_text(content, encoding="utf-8")


def load_titledb_names(locale: str) -> dict[str, dict]:
    cached = _TITLEDB_BY_LOCALE.get(locale)
    if cached is not None:
        return cached

    cache_path = CACHE_DIR / f"titledb-{locale}.json"
    should_refresh = True
    if cache_path.exists():
        age_seconds = time.time() - cache_path.stat().st_mtime
        should_refresh = age_seconds > TITLEDB_NAME_CACHE_TTL_SECONDS

    if should_refresh:
        try:
            download_to_cache(TITLEDB_NAME_SOURCES[locale], cache_path)
        except Exception:
            if not cache_path.exists():
                raise

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


def resolve_title_name(title_id: str, catalog_name_map: dict[str, str]) -> str:
    if catalog_name_map.get(title_id):
        return repair_mojibake(catalog_name_map[title_id])

    for locale in ("pt", "en"):
        try:
            item = load_titledb_names(locale).get(title_id, {})
        except Exception:
            item = {}
        name = str(item.get("name") or "").strip()
        if name:
            return repair_mojibake(name)

    return title_id


def load_catalog_entries() -> tuple[dict, list[dict]]:
    metadata = load_json(METADATA_PATH) if METADATA_PATH.exists() else {}
    entries: list[dict] = []
    if ENTRIES_DIR.exists():
        for path in sorted(ENTRIES_DIR.glob("*.json")):
            item = load_json(path)
            if isinstance(item, dict):
                entries.append(item)
    return metadata, entries


def build_watch_list(config: dict, entries: list[dict]) -> set[str]:
    watch = config.get("watch") or {}
    watch_sections = {str(item).strip().lower() for item in (watch.get("catalogSections") or [])}
    watched = {normalize_title_id(item) for item in (watch.get("titleIds") or []) if str(item).strip()}

    for entry in entries:
        section = str(entry.get("section") or "").strip().lower()
        title_id = normalize_title_id(entry.get("titleId") or "")
        if title_id and section in watch_sections:
            watched.add(title_id)

    return {title_id for title_id in watched if re.fullmatch(r"[0-9A-F]{16}", title_id)}


def build_catalog_name_map(entries: list[dict]) -> dict[str, str]:
    result: dict[str, str] = {}
    for entry in entries:
        title_id = normalize_title_id(entry.get("titleId") or "")
        name = str(entry.get("name") or "").strip()
        if title_id and name and title_id not in result:
            result[title_id] = name
    return result


def parse_path_title_build(path: str) -> tuple[str, str] | None:
    for pattern in TXT_PATH_PATTERNS:
        match = pattern.search(path)
        if match:
            return normalize_title_id(match.group("title")), normalize_title_id(match.group("build"))
    return None


def iter_repo_text_candidates(source_name: str, source_config: dict, watched_title_ids: set[str], priorities: dict[str, int]) -> list[CheatCandidate]:
    tree_api_url = str(source_config.get("treeApiUrl") or "").strip()
    raw_base_url = str(source_config.get("rawBaseUrl") or "").strip()
    if not tree_api_url or not raw_base_url:
        return []

    tree = http_get_json(tree_api_url, accept="application/vnd.github+json").get("tree", [])
    matches: list[tuple[str, str, str]] = []
    for item in tree:
        path = item.get("path")
        if not isinstance(path, str) or "/cheats/" not in path or not path.endswith(".txt"):
            continue
        parsed = parse_path_title_build(path)
        if not parsed:
            continue
        title_id, build_id = parsed
        if watched_title_ids and title_id not in watched_title_ids:
            continue
        matches.append((path, title_id, build_id))

    candidates: list[CheatCandidate] = []
    for path, title_id, build_id in matches:
        raw_url = raw_base_url + urllib.parse.quote(path)
        candidates.append(
            CheatCandidate(
                title_id=title_id,
                build_id=build_id,
                source=source_name,
                content=path,
                categories=[],
                origin_url=raw_url,
                priority_rank=int(priorities.get(source_name, 999)),
            )
        )
    resolved: list[CheatCandidate] = []
    with ThreadPoolExecutor(max_workers=MAX_FETCH_WORKERS) as executor:
        futures = {
            executor.submit(http_get_text_cached, candidate.origin_url, None, HTTP_CACHE_TTL_SECONDS): candidate
            for candidate in candidates
        }
        for future in as_completed(futures):
            candidate = futures[future]
            try:
                raw_text = future.result()
            except Exception:
                continue
            content = normalize_cheat_text(raw_text)
            if not content:
                continue
            resolved.append(
                CheatCandidate(
                    title_id=candidate.title_id,
                    build_id=candidate.build_id,
                    source=candidate.source,
                    content=content,
                    categories=infer_categories(source_name, candidate.content, content),
                    origin_url=candidate.origin_url,
                    priority_rank=candidate.priority_rank,
                )
            )
    return sorted(resolved, key=lambda item: (item.title_id, item.build_id, item.origin_url))


def iter_titledb_candidates(source_config: dict, watched_title_ids: set[str], priorities: dict[str, int]) -> list[CheatCandidate]:
    url = str(source_config.get("url") or "").strip()
    if not url:
        return []

    data = http_get_json(url)
    candidates: list[CheatCandidate] = []
    for title_id, builds in data.items():
        normalized_title_id = normalize_title_id(title_id)
        if watched_title_ids and normalized_title_id not in watched_title_ids:
            continue
        if not isinstance(builds, dict):
            continue
        for build_id, build_data in builds.items():
            if not isinstance(build_data, dict):
                continue
            content = make_cheat_file_text_from_titledb(build_data)
            if not content:
                continue
            candidates.append(
                CheatCandidate(
                    title_id=normalized_title_id,
                    build_id=normalize_title_id(build_id),
                    source="titledb",
                    content=content,
                    categories=infer_categories("titledb", "", content),
                    origin_url=url,
                    priority_rank=int(priorities.get("titledb", 999)),
                )
            )
    return candidates


def parse_cheatslips_page(page_url: str) -> CheatCandidate | None:
    html_text = http_get_text(page_url)
    title_match = re.search(r"<strong>Title Id</strong>:\s*<span[^>]*>([0-9A-Fa-f]{16})</span>", html_text)
    build_match = re.search(r"<strong>Build Id</strong>:\s*<span[^>]*>([0-9A-Fa-f]{16})</span>", html_text)
    pre_match = re.search(r"<pre[^>]*>(.*?)</pre>", html_text, re.IGNORECASE | re.DOTALL)
    textarea_match = re.search(r"<textarea[^>]*>(.*?)</textarea>", html_text, re.IGNORECASE | re.DOTALL)
    content_raw = ""
    if pre_match:
        content_raw = pre_match.group(1)
    elif textarea_match:
        content_raw = textarea_match.group(1)
    if not (title_match and build_match and content_raw):
        return None
    content = normalize_cheat_text(html.unescape(HTML_BREAK_RE.sub("\n", TAG_RE.sub("", content_raw))))
    if not content:
        return None
    return CheatCandidate(
        title_id=normalize_title_id(title_match.group(1)),
        build_id=normalize_title_id(build_match.group(1)),
        source="cheatSlips",
        content=content,
        categories=infer_categories("cheatSlips", "", content),
        origin_url=page_url,
    )


def iter_cheatslips_candidates(source_config: dict, watched_title_ids: set[str], priorities: dict[str, int]) -> list[CheatCandidate]:
    seed_pages = source_config.get("seedPages") or []
    candidates: list[CheatCandidate] = []
    for page_url in seed_pages:
        try:
            candidate = parse_cheatslips_page(str(page_url).strip())
        except Exception:
            candidate = None
        if candidate is None:
            continue
        if watched_title_ids and candidate.title_id not in watched_title_ids:
            continue
        candidate.priority_rank = int(priorities.get("cheatSlips", 999))
        candidates.append(candidate)
    return candidates


def merge_candidates(candidates: list[CheatCandidate]) -> list[MergedCheatEntry]:
    merged: dict[tuple[str, str, str], MergedCheatEntry] = {}
    for candidate in candidates:
        content_hash = hashlib.sha256(candidate.content.encode("utf-8")).hexdigest()
        key = (candidate.title_id, candidate.build_id, content_hash)
        target = merged.get(key)
        if target is None:
            target = MergedCheatEntry(
                title_id=candidate.title_id,
                build_id=candidate.build_id,
                content=candidate.content,
                content_hash=content_hash,
                priority_rank=candidate.priority_rank,
                title=candidate.title or choose_entry_title(candidate.categories),
            )
            merged[key] = target
        target.sources = sorted(set(target.sources + [candidate.source]))
        target.categories = sorted(set(target.categories + candidate.categories))
        target.origin_urls = sorted(set(target.origin_urls + [candidate.origin_url]))
        if candidate.priority_rank < target.priority_rank:
            target.priority_rank = candidate.priority_rank
            target.title = candidate.title or choose_entry_title(target.categories)

    return sorted(
        merged.values(),
        key=lambda item: (item.title_id, item.build_id, item.priority_rank, item.content_hash),
    )


def write_dist_outputs(entries: list[MergedCheatEntry], public_base_url: str, catalog_name_map: dict[str, str]) -> dict:
    if DIST_CHEATS_DIR.exists():
        for path in DIST_CHEATS_DIR.rglob("*"):
            if path.is_file():
                try:
                    path.unlink()
                except FileNotFoundError:
                    pass
        for path in sorted(DIST_CHEATS_DIR.rglob("*"), reverse=True):
            if path.is_dir():
                try:
                    path.rmdir()
                except OSError:
                    pass

    titles_map: dict[str, dict] = {}
    source_summary: dict[str, int] = {}

    for entry in entries:
        entry_id = f"{entry.title_id.lower()}-{entry.build_id.lower()}-{entry.content_hash[:12]}"
        relative_path = Path("cheats") / entry.title_id / entry.build_id / f"{entry_id}.txt"
        output_path = DIST_DIR / relative_path
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(entry.content, encoding="utf-8")

        for source_name in entry.sources:
            source_summary[source_name] = source_summary.get(source_name, 0) + 1

        title_bucket = titles_map.setdefault(
            entry.title_id,
            {
                "titleId": entry.title_id,
                "name": resolve_title_name(entry.title_id, catalog_name_map),
                "builds": {},
            },
        )
        build_bucket = title_bucket["builds"].setdefault(
            entry.build_id,
            {
                "buildId": entry.build_id,
                "categories": set(),
                "sources": set(),
                "primarySource": "",
                "priorityRank": 999,
                "contents": [],
                "entries": [],
            },
        )
        build_bucket["categories"].update(entry.categories)
        build_bucket["sources"].update(entry.sources)
        build_bucket["contents"].append(entry.content)
        if not build_bucket["primarySource"]:
            build_bucket["primarySource"] = min(entry.sources, key=lambda item: item)
        build_bucket["priorityRank"] = min(int(build_bucket["priorityRank"]), int(entry.priority_rank))
        build_bucket["entries"].append(
            {
                "id": entry_id,
                "title": entry.title or choose_entry_title(entry.categories),
                "primarySource": min(entry.sources, key=lambda item: item),
                "sources": entry.sources,
                "categories": entry.categories,
                "contentHash": f"sha256:{entry.content_hash}",
                "cheatCount": count_cheat_blocks(entry.content),
                "lineCount": len([line for line in entry.content.splitlines() if line.strip()]),
                "relativePath": str(relative_path).replace("\\", "/"),
                "downloadUrl": public_base_url + str(relative_path).replace("\\", "/"),
                "originUrls": entry.origin_urls,
                "priorityRank": entry.priority_rank,
            }
        )

    titles = []
    summaries = []
    for title_id in sorted(titles_map.keys()):
        title_bucket = titles_map[title_id]
        builds = []
        title_categories: set[str] = set()
        title_sources: set[str] = set()
        total_cheat_count = 0
        for build_id in sorted(title_bucket["builds"].keys()):
            build_bucket = title_bucket["builds"][build_id]
            build_bucket["entries"].sort(key=lambda item: (item["priorityRank"], item["title"], item["id"]))
            combined_text = normalize_cheat_text("\n\n".join(build_bucket["contents"]))
            build_relative_path = Path("cheats") / title_bucket["titleId"] / f"{build_id}.txt"
            build_output_path = DIST_DIR / build_relative_path
            build_output_path.parent.mkdir(parents=True, exist_ok=True)
            build_output_path.write_text(combined_text, encoding="utf-8")
            build_cheat_count = count_cheat_blocks(combined_text)
            build_line_count = len([line for line in combined_text.splitlines() if line.strip()])
            build_content_hash = hashlib.sha256(combined_text.encode("utf-8")).hexdigest()
            for item in build_bucket["entries"]:
                title_categories.update(item["categories"])
                title_sources.update(item["sources"])
                total_cheat_count += int(item["cheatCount"])
            builds.append(
                {
                    "buildId": build_id,
                    "categories": sorted(build_bucket["categories"]),
                    "primarySource": build_bucket["primarySource"],
                    "sources": sorted(build_bucket["sources"]),
                    "contentHash": f"sha256:{build_content_hash}",
                    "cheatCount": build_cheat_count,
                    "lineCount": build_line_count,
                    "relativePath": str(build_relative_path).replace("\\", "/"),
                    "downloadUrl": public_base_url + str(build_relative_path).replace("\\", "/"),
                    "priorityRank": int(build_bucket["priorityRank"]),
                    "entries": build_bucket["entries"],
                }
            )
        titles.append(
            {
                "titleId": title_bucket["titleId"],
                "name": title_bucket["name"],
                "builds": builds,
            }
        )
        summaries.append(
            {
                "titleId": title_bucket["titleId"],
                "name": title_bucket["name"],
                "categories": sorted(title_categories),
                "sources": sorted(title_sources),
                "buildCount": len(builds),
                "cheatCount": total_cheat_count,
                "builds": [
                    {
                        "buildId": build["buildId"],
                        "categories": build["categories"],
                        "primarySource": build["primarySource"],
                        "sources": build["sources"],
                        "contentHash": build["contentHash"],
                        "cheatCount": build["cheatCount"],
                        "lineCount": build["lineCount"],
                        "relativePath": build["relativePath"],
                        "downloadUrl": build["downloadUrl"],
                        "priorityRank": build["priorityRank"],
                    }
                    for build in builds
                ],
            }
        )

    return {
        "titles": titles,
        "summaries": summaries,
        "sourceSummary": source_summary,
    }


def build_pack_files(catalog_revision: str, public_base_url: str) -> tuple[str, str, str]:
    revision = str(catalog_revision or "").strip() or now_utc_iso().replace(":", "-")
    manifest = {
        "schemaVersion": "1.0",
        "generatedAt": now_utc_iso(),
        "revision": revision,
        "packFile": DIST_CHEATS_PACK_PATH.name,
    }
    DIST_CHEATS_MANIFEST_PATH.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    DIST_CHEATS_PACK_PATH.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(DIST_CHEATS_PACK_PATH, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.write(DIST_CHEATS_MANIFEST_PATH, "manifest.json")
        if DIST_CHEATS_DIR.exists():
            for path in sorted(DIST_CHEATS_DIR.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(DIST_DIR).as_posix())

    pack_sha256 = "sha256:" + sha256_file(DIST_CHEATS_PACK_PATH)
    manifest["packSha256"] = pack_sha256
    DIST_CHEATS_MANIFEST_PATH.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    return revision, public_base_url_for_path(public_base_url, DIST_CHEATS_PACK_PATH.name), pack_sha256


def public_base_url_for_path(base_url: str, relative_path: str) -> str:
    if not base_url.endswith("/"):
        base_url += "/"
    return base_url + relative_path.lstrip("/")


def main() -> int:
    metadata, entries = load_catalog_entries()
    config = load_json(CHEAT_SOURCES_PATH)
    public_base_url = normalize_public_base_url(metadata)
    watched_title_ids = build_watch_list(config, entries)
    catalog_name_map = build_catalog_name_map(entries)
    priorities = config.get("priorities") or {}
    sources = config.get("sources") or {}

    all_candidates: list[CheatCandidate] = []
    enabled_sources: dict[str, dict] = {}

    for source_name, source_config in sources.items():
        if not isinstance(source_config, dict) or not source_config.get("enabled", False):
            continue
        enabled_sources[source_name] = source_config
        if source_name == "titledb":
            all_candidates.extend(iter_titledb_candidates(source_config, watched_title_ids, priorities))
        elif source_name == "cheatSlips":
            all_candidates.extend(iter_cheatslips_candidates(source_config, watched_title_ids, priorities))
        else:
            all_candidates.extend(iter_repo_text_candidates(source_name, source_config, watched_title_ids, priorities))

    merged_entries = merge_candidates(all_candidates)
    output = write_dist_outputs(merged_entries, public_base_url, catalog_name_map)

    cheats_pack_revision, cheats_pack_url, cheats_pack_sha256 = build_pack_files(str(metadata.get("catalogRevision") or ""), public_base_url)

    cheats_summary_payload = {
        "schemaVersion": "1.0",
        "generatedAt": now_utc_iso(),
        "generator": "MILCheatsAggregator",
        "catalogRevision": str(metadata.get("catalogRevision") or ""),
        "cheatsPackRevision": cheats_pack_revision,
        "cheatsPackUrl": cheats_pack_url,
        "cheatsPackSha256": cheats_pack_sha256,
        "watchedTitleIds": sorted(watched_title_ids),
        "titles": output["summaries"],
    }

    dist_summary_path = DIST_DIR / "cheats-summary.json"
    dist_summary_path.parent.mkdir(parents=True, exist_ok=True)
    dist_summary_path.write_text(json.dumps(cheats_summary_payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    payload = {
        "schemaVersion": "1.0",
        "generatedAt": now_utc_iso(),
        "generator": "MILCheatsAggregator",
        "catalogRevision": str(metadata.get("catalogRevision") or ""),
        "cheatsPackRevision": cheats_pack_revision,
        "cheatsPackUrl": cheats_pack_url,
        "cheatsPackSha256": cheats_pack_sha256,
        "watchedTitleIds": sorted(watched_title_ids),
        "sources": {
            source_name: {
                "enabled": True,
                "priorityRank": int(priorities.get(source_name, 999)),
                "records": output["sourceSummary"].get(source_name, 0),
            }
            for source_name in enabled_sources.keys()
        },
        "titles": output["titles"],
    }

    DIST_CHEATS_INDEX_PATH.parent.mkdir(parents=True, exist_ok=True)
    DIST_CHEATS_INDEX_PATH.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(f"Cheats index gerado com {len(output['titles'])} titulos.")
    print(f"Saida principal: {DIST_CHEATS_INDEX_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
