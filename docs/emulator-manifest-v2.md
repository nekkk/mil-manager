# Emulator Manifest V2

`installed-titles-cache.json` is the official emulator-facing manifest for MIL.

Goals:
- make emulator title discovery deterministic
- avoid guest-side parsing of host libraries
- preserve backward compatibility with the legacy v1 manifest

## Top-level schema

```json
{
  "schemaVersion": "2",
  "generatedAt": "2026-03-23T18:00:00Z",
  "generator": "ryujinx-mil-export",
  "environment": "emulator",
  "emulator": {
    "name": "ryujinx",
    "root": "C:/Users/user/AppData/Roaming/Ryujinx",
    "configPath": "C:/Users/user/AppData/Roaming/Ryujinx/Config.json",
    "gamesCachePath": "C:/Users/user/AppData/Roaming/Ryujinx/games",
    "gameDirs": [
      "D:/Switch/Roms"
    ]
  },
  "titles": [
    {
      "titleId": "0100B6E00A420000",
      "baseTitleId": "0100B6E00A420000",
      "name": "Dust: An Elysian Tail",
      "publisher": "Humble Hearts",
      "displayVersion": "1.06.181214",
      "metadataAvailable": true,
      "favorite": false,
      "lastPlayedUtc": "",
      "playTime": "",
      "fileType": "nsp",
      "source": "application-library",
      "emulator": "ryujinx",
      "localIconPath": "sdmc:/config/mil_manager/cache/imported-icons/0100b6e00a420000.jpg",
      "sourcePath": "D:/Switch/Roms/Dust.nsp",
      "basePath": "D:/Switch/Roms/Dust.nsp",
      "updatePath": "D:/Switch/Updates/Dust Update.nsp",
      "dlcPaths": [],
      "paths": {
        "source": "D:/Switch/Roms/Dust.nsp",
        "base": "D:/Switch/Roms/Dust.nsp",
        "update": "D:/Switch/Updates/Dust Update.nsp",
        "dlc": []
      }
    }
  ]
}
```

## Required fields

Top-level:
- `schemaVersion`
- `generatedAt`
- `generator`
- `environment`
- `emulator.name`
- `titles`

Per title:
- `titleId`
- `name`
- `displayVersion`
- `metadataAvailable`

## Recommended fields

Per title:
- `publisher`
- `baseTitleId`
- `localIconPath`
- `sourcePath`
- `basePath`
- `updatePath`
- `dlcPaths`
- `fileType`
- `source`
- `favorite`
- `lastPlayedUtc`
- `playTime`
- `paths`

## Compatibility rules

MIL should:
- prefer v2 when available
- accept legacy v1 keys such as `title_id` and `display_version`
- ignore unknown fields

## Path expectations

The manifest should be written to:

`sdmc:/switch/mil_manager/installed-titles-cache.json`

When exporting icons, emulators should prefer:

`sdmc:/config/mil_manager/cache/imported-icons/<titleId>.jpg`

This keeps all emulator-sourced assets in one predictable place.
