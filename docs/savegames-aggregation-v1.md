# Save Games Aggregation V1

## Baseline
- Git tag: `V1.2-saves-baseline`
- Commit: `50756fc`

## Goal
- Aggregate Nintendo Switch save packs from multiple online sources.
- Match installed titles by `titleId` only.
- Default UI should list saves only for detected titles.
- Full corpus should be available through search.

## Source Priority
1. `Viren070/NX_Saves`
2. `GhostlyDark/Savegames`
3. `niemasd/Game-Saves` (`Type: Switch` only)
4. `gbatemp` category later/manual

## Why
- `Viren070` has the easiest machine-readable naming convention, including `titleId`.
- `GhostlyDark` is structured by game/title folder and is also usable.
- `niemasd` mixes Switch and emulator saves, so it needs filtering.
- `gbatemp` is useful, but it is not a stable machine-friendly source for the first pass.

## Install/Restore Model
- Do not treat save packs like `atmosphere/contents`.
- Saves are not build-bound, but they are container/account/device sensitive.
- The safest V1 path is to aggregate and stage backups, not to write directly into live save containers.
- Later direct restore work should follow JKSV-like semantics.

## Recommended Published Format
- `saves-index.json`
  - lightweight summary for all titles
- `saves/<titleId>/index.json`
  - per-title variants and metadata
- `save-packs/<titleId>/<variantId>.zip`
  - actual payload archive

## Suggested Summary Schema
```json
{
  "schemaVersion": 1,
  "generatedAt": "2026-03-25T12:00:00Z",
  "catalogRevision": "2026.03.25.1",
  "titles": [
    {
      "titleId": "01007300020FA000",
      "name": "ASTRAL CHAIN",
      "variantCount": 2,
      "categories": ["complete", "starter"],
      "latestUpdatedAt": "2026-03-25",
      "detailsUrl": "https://.../saves/01007300020FA000/index.json"
    }
  ]
}
```

## Suggested Per-Title Schema
```json
{
  "schemaVersion": 1,
  "titleId": "01007300020FA000",
  "name": "ASTRAL CHAIN",
  "variants": [
    {
      "id": "story-complete-001",
      "label": "Story Complete",
      "category": "complete",
      "saveKind": "account",
      "layoutType": "jksv-folder",
      "platform": "switch",
      "author": "ABDULSALAM",
      "language": "es",
      "updatedAt": "2026-03-25",
      "downloadUrl": "https://.../save-packs/01007300020FA000/story-complete-001.zip",
      "sha256": "<hash>",
      "size": 123456,
      "origins": ["viren070"]
    }
  ]
}
```

## Deduplication
- Primary dedupe key:
  - `titleId`
  - `layoutType`
  - normalized payload hash
- If hashes match, merge origins and aliases.
- If hashes differ, keep separate variants for the same title.

## UI
- `Save Games` without search:
  - show only detected `titleId`s
- `Search`:
  - search full `saves-index.json`
- Empty state:
  - PT-BR: `Títulos não identificados. Utilize a Pesquisa para localizar o save desejado.`
  - EN-US: `Titles not identified. Use Search to find the desired save.`

## Categories
- `complete`
- `starter`
- `event`
- `ngplus`
- `unlocked`
- `modded`
- `bcat`
- `other`

## Notes
- `aio-switch-updater` is most useful here as a download/progress reference.
- `EmuSAK` is most useful as an emulator-oriented distribution reference.
- `JKSV` is the key semantic reference for actual save backup/restore behavior.
