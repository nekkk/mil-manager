#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence


APPDATA = Path(os.environ.get("APPDATA", ""))
LOCALAPPDATA = Path(os.environ.get("LOCALAPPDATA", ""))

DEFAULT_SD_SUBDIR = Path("sdcard") / "switch" / "mil_manager" / "cache"
DEFAULT_CACHE_NAME = "installed-titles-cache.json"
SUPPORTED_EXTENSIONS = {".nsp", ".pfs0", ".xci", ".nca", ".nro", ".nso"}
TITLE_ID_PATTERN = re.compile(r"(?i)(?:\[)?([0-9a-f]{16})(?:\])?")


def now_utc_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def normalize_title_id(value: str) -> str:
    return value.strip().upper().zfill(16)


def classify_title_id(title_id: str) -> str:
    value = int(title_id, 16)
    suffix = value & 0x1FFF
    if suffix == 0x800:
        return "update"
    if suffix & 0x1000:
        return "dlc"
    return "base"


def base_title_id(title_id: str) -> str:
    value = int(title_id, 16)
    return f"{value & ~0x1FFF:016X}"


def extract_title_ids(text: str) -> List[str]:
    return [normalize_title_id(match.group(1)) for match in TITLE_ID_PATTERN.finditer(text)]


def guess_name_from_path(path: str) -> str:
    stem = Path(path).stem
    stem = re.sub(r"\[[^\]]+\]", "", stem)
    stem = re.sub(r"\([^)]*\)", "", stem)
    stem = re.sub(r"\s{2,}", " ", stem)
    return stem.strip(" -_") or Path(path).stem


def detect_version_from_cache(title_dir: Path) -> str:
    cpu_cache_dir = title_dir / "cache" / "cpu"
    versions = set()
    if cpu_cache_dir.exists():
        for cache_file in cpu_cache_dir.rglob("*.cache"):
            version = cache_file.stem.split("-", 1)[0].strip()
            if version and version.lower() != "default":
                versions.add(version)
    return sorted(versions)[-1] if versions else ""


def load_json(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8-sig"))


@dataclass
class TitleRecord:
    titleId: str
    baseTitleId: str = ""
    name: str = ""
    publisher: str = ""
    displayVersion: str = ""
    metadataAvailable: bool = False
    paths: List[str] = field(default_factory=list)
    basePath: str = ""
    updatePath: str = ""
    dlcPaths: List[str] = field(default_factory=list)
    emulator: str = ""
    source: str = ""
    favorite: bool = False
    lastPlayedUtc: str = ""
    playTime: str = ""
    fileType: str = ""
    localIconPath: str = ""

    def ensure_name(self) -> None:
        if not self.name:
            candidate = self.basePath or self.updatePath or (self.paths[0] if self.paths else "")
            self.name = guess_name_from_path(candidate) if candidate else f"Title {self.titleId}"

    def merge_path(self, path: str, kind: str) -> None:
        if path and path not in self.paths:
            self.paths.append(path)
        if kind == "base" and not self.basePath:
            self.basePath = path
        elif kind == "update":
            self.updatePath = path
        elif kind == "dlc" and path and path not in self.dlcPaths:
            self.dlcPaths.append(path)


@dataclass
class EmulatorInfo:
    name: str
    root: str
    configPath: str = ""
    gamesCachePath: str = ""
    gameDirs: List[str] = field(default_factory=list)


@dataclass
class CachePayload:
    schemaVersion: str
    generatedAt: str
    generator: str
    environment: str
    emulator: EmulatorInfo
    titles: List[TitleRecord]

    def to_json(self) -> str:
        payload = asdict(self)
        normalized_titles = []
        for title in self.titles:
            item = asdict(title)
            item["paths"] = {
                "source": title.basePath or title.updatePath or (title.paths[0] if title.paths else ""),
                "base": title.basePath,
                "update": title.updatePath,
                "dlc": title.dlcPaths,
            }
            normalized_titles.append(item)
        payload["titles"] = normalized_titles
        return json.dumps(payload, ensure_ascii=False, indent=2) + "\n"


class EmulatorAdapter:
    name = "unknown"
    implemented = False

    def detect_default_root(self) -> Optional[Path]:
        return None

    def scan(self, root: Optional[Path] = None) -> CachePayload:
        raise NotImplementedError


class RyujinxAdapter(EmulatorAdapter):
    name = "ryujinx"
    implemented = True

    def detect_default_root(self) -> Optional[Path]:
        root = APPDATA / "Ryujinx"
        return root if (root / "Config.json").exists() else None

    def _load_config(self, root: Path) -> Dict[str, object]:
        config_path = root / "Config.json"
        if not config_path.exists():
            return {}
        data = load_json(config_path)
        return data if isinstance(data, dict) else {}

    def _scan_configured_dirs(self, game_dirs: Sequence[str], extensions: Iterable[str]) -> Dict[str, TitleRecord]:
        records: Dict[str, TitleRecord] = {}
        normalized_exts = {ext.lower() for ext in extensions}

        for game_dir in game_dirs:
            root = Path(game_dir)
            if not root.exists():
                continue
            for current_root, _, files in os.walk(root):
                for file_name in files:
                    path = Path(current_root) / file_name
                    if path.suffix.lower() not in normalized_exts:
                        continue

                    title_ids = extract_title_ids(file_name)
                    if not title_ids:
                        continue

                    for raw_title_id in title_ids:
                        kind = classify_title_id(raw_title_id)
                        parent_title_id = base_title_id(raw_title_id)
                        record = records.setdefault(
                            parent_title_id, TitleRecord(titleId=parent_title_id, baseTitleId=parent_title_id)
                        )
                        if not record.fileType:
                            record.fileType = path.suffix.lower().lstrip(".")
                        record.merge_path(str(path), kind)
                        if not record.name and kind == "base":
                            record.name = guess_name_from_path(str(path))
        return records

    def _load_game_cache(self, root: Path) -> Dict[str, TitleRecord]:
        games_dir = root / "games"
        records: Dict[str, TitleRecord] = {}
        if not games_dir.exists():
            return records

        for child in sorted(games_dir.iterdir()):
            if not child.is_dir():
                continue
            if not re.fullmatch(r"(?i)[0-9a-f]{16}", child.name):
                continue

            title_id = normalize_title_id(child.name)
            record = TitleRecord(
                titleId=title_id,
                baseTitleId=title_id,
                emulator=self.name,
                source="games-cache",
            )

            metadata_path = child / "gui" / "metadata.json"
            if metadata_path.exists():
                try:
                    metadata = load_json(metadata_path)
                    if isinstance(metadata, dict):
                        record.name = metadata.get("title") or ""
                        record.favorite = bool(metadata.get("favorite", False))
                        record.lastPlayedUtc = str(metadata.get("last_played_utc") or "")
                        record.playTime = str(metadata.get("timespan_played") or "")
                except json.JSONDecodeError:
                    pass

            record.displayVersion = detect_version_from_cache(child)

            updates_path = child / "updates.json"
            if updates_path.exists():
                try:
                    updates = load_json(updates_path)
                    if isinstance(updates, dict):
                        selected = updates.get("selected")
                        if isinstance(selected, str) and selected:
                            record.merge_path(selected, "update")
                        for update_path in updates.get("paths", []):
                            if isinstance(update_path, str):
                                record.merge_path(update_path, "update")
                except json.JSONDecodeError:
                    pass

            dlc_path = child / "dlc.json"
            if dlc_path.exists():
                try:
                    dlc_items = load_json(dlc_path)
                    if isinstance(dlc_items, list):
                        for dlc_item in dlc_items:
                            if isinstance(dlc_item, dict):
                                path = dlc_item.get("path")
                                if isinstance(path, str) and path:
                                    record.merge_path(path, "dlc")
                except json.JSONDecodeError:
                    pass

            record.metadataAvailable = bool(record.name)
            record.ensure_name()
            records[title_id] = record

        return records

    def scan(self, root: Optional[Path] = None) -> CachePayload:
        resolved_root = root or self.detect_default_root()
        if resolved_root is None:
            raise FileNotFoundError("Ryujinx root not found. Use --root explicitly.")

        config = self._load_config(resolved_root)
        game_dirs = [str(path) for path in config.get("game_dirs", []) if isinstance(path, str)]
        shown_file_types = config.get("shown_file_types", {})
        enabled_exts = SUPPORTED_EXTENSIONS
        if isinstance(shown_file_types, dict):
            enabled_exts = {
                f".{file_type.lower()}"
                for file_type, enabled in shown_file_types.items()
                if enabled and f".{file_type.lower()}" in SUPPORTED_EXTENSIONS
            } or SUPPORTED_EXTENSIONS

        discovered = self._scan_configured_dirs(game_dirs, enabled_exts)
        cached = self._load_game_cache(resolved_root)

        merged: Dict[str, TitleRecord] = {}
        for title_id, record in cached.items():
            merged[title_id] = record

        for title_id, discovered_record in discovered.items():
            target = merged.setdefault(
                title_id,
                TitleRecord(titleId=title_id, baseTitleId=title_id, emulator=self.name, source="scan"),
            )
            for path in discovered_record.paths:
                kind = "base"
                if path == discovered_record.updatePath:
                    kind = "update"
                elif path in discovered_record.dlcPaths:
                    kind = "dlc"
                target.merge_path(path, kind)
            if not target.name and discovered_record.name:
                target.name = discovered_record.name

        titles = sorted(merged.values(), key=lambda item: (item.name.lower(), item.titleId))
        for title in titles:
            title.emulator = self.name
            title.ensure_name()

        return CachePayload(
            schemaVersion="2",
            generatedAt=now_utc_iso(),
            generator="mil_emulator_sync",
            environment="emulator",
            emulator=EmulatorInfo(
                name=self.name,
                root=str(resolved_root),
                configPath=str(resolved_root / "Config.json"),
                gamesCachePath=str(resolved_root / "games"),
                gameDirs=game_dirs,
            ),
            titles=titles,
        )


class EdenAdapter(EmulatorAdapter):
    name = "eden"
    implemented = False


class YuzuLikeAdapter(EmulatorAdapter):
    name = "yuzu-like"
    implemented = False


ADAPTERS: Dict[str, EmulatorAdapter] = {
    "ryujinx": RyujinxAdapter(),
    "eden": EdenAdapter(),
    "yuzu-like": YuzuLikeAdapter(),
}


def detect_default_output(root: Path) -> Path:
    return root / DEFAULT_SD_SUBDIR / DEFAULT_CACHE_NAME


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Synchronize emulator titles into MIL cache format.")
    parser.add_argument("--emulator", default="ryujinx", choices=sorted(ADAPTERS.keys()))
    parser.add_argument("--root", default="", help="Emulator root directory.")
    parser.add_argument("--output", default="", help="Normalized cache output path.")
    parser.add_argument("--list-adapters", action="store_true", help="List known adapters and exit.")
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)

    if args.list_adapters:
        for name, adapter in ADAPTERS.items():
            status = "ready" if adapter.implemented else "planned"
            print(f"{name}: {status}")
        return 0

    adapter = ADAPTERS[args.emulator]
    if not adapter.implemented:
        print(f"Adapter '{args.emulator}' is planned but not implemented yet.", file=sys.stderr)
        return 2

    root = Path(args.root) if args.root else adapter.detect_default_root()
    if root is None:
        print(f"Could not locate default root for emulator '{args.emulator}'.", file=sys.stderr)
        return 2

    payload = adapter.scan(root)

    output_path = Path(args.output) if args.output else detect_default_output(root)
    write_text(output_path, payload.to_json())
    print(f"Wrote {len(payload.titles)} titles to {output_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
