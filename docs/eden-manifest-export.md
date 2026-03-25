# Eden Manifest Export Notes

Eden currently builds the game library host-side in:

- [game_list_worker.cpp](C:/Users/lordd/source/codex/_ext/eden-src/src/yuzu/game/game_list_worker.cpp)
- [game_list_worker.h](C:/Users/lordd/source/codex/_ext/eden-src/src/yuzu/game/game_list_worker.h)

Important observations:
- it scans configured game dirs with `ScanFileSystem(...)`
- it reads metadata with `ReadProgramId`, `ReadTitle`, `ReadIcon`
- it can parse control content with `PatchManager::ParseControlNCA(...)`
- it already caches icon and app name in `CacheDir/game_list`

## Recommended hook

Export the MIL manifest once the worker finishes populating the library, near the same flow that ends in:

- `RecordEvent([this](GameList* game_list) { game_list->DonePopulating(watch_list); });`

The exporter should write:

`sdcard/switch/mil_manager/installed-titles-cache.json`

## Fields mapping

Game list data -> manifest:

- `program_id` -> `titleId`
- `program_id` -> `baseTitleId`
- title/app name -> `name`
- patch/display version summary when available -> `displayVersion`
- icon bytes -> exported `localIconPath`
- scanned file path -> `sourcePath`
- source suffix -> `fileType`
- exporter id such as `game-list-worker` -> `source`

## Why this is the best Eden path

- it reuses the exact library logic Eden already trusts
- it uses already-decoded metadata and icon bytes
- it avoids any guest-side probing or runtime SSL/network dependency
- it gives MIL the same view of titles the Eden UI already has
