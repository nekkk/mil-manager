# Ryujinx Manifest Export Notes

Ryujinx already has all required data host-side in:

- [ApplicationLibrary.cs](C:/Users/lordd/source/codex/_ext/ryujinx-src/src/Ryujinx/Systems/AppLibrary/ApplicationLibrary.cs)
- [ApplicationData.cs](C:/Users/lordd/source/codex/_ext/ryujinx-src/src/Ryujinx/Systems/AppLibrary/ApplicationData.cs)

## Recommended hook

Export the MIL manifest immediately after the application library refresh completes.

The exporter should serialize each `ApplicationData` entry into schema v2 and write to the mounted SD path:

`sdcard/switch/mil_manager/installed-titles-cache.json`

## Fields mapping

`ApplicationData` -> manifest:

- `IdBaseString` -> `titleId`
- `IdBaseString` -> `baseTitleId`
- `Name` -> `name`
- `Developer` -> `publisher`
- `Version` -> `displayVersion`
- `HasControlHolder` -> `metadataAvailable`
- `Favorite` -> `favorite`
- `LastPlayed` -> `lastPlayedUtc`
- `TimePlayedString` -> `playTime`
- `Path` -> `sourcePath`
- `FileExtension` -> `fileType`

If icon bytes are available:
- save them as `sdcard/config/mil_manager/cache/imported-icons/<titleId>.jpg`
- set `localIconPath` accordingly

## Why this is the best Ryujinx path

- no reimplementation of NSP/XCI/NRO parsing
- no dependency on guest-side services
- identical data source to the emulator UI
- can be refreshed whenever the app library refreshes
