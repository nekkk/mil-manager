param(
    [string]$CatalogUrl = "https://SEU_USUARIO.github.io/SEU_REPOSITORIO/index.json",
    [string]$MegaFolderUrl = "",
    [string]$EmulatorRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-DefaultEmulatorRoot {
    $candidates = @(
        (Join-Path $env:APPDATA "Ryujinx"),
        (Join-Path $env:APPDATA "eden")
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Nao foi possivel localizar a raiz padrao do emulador. Informe -EmulatorRoot explicitamente."
}

function Get-EmulatorSdRoot {
    param([Parameter(Mandatory = $true)][string]$EmulatorRootPath)

    $sdmcPath = Join-Path $EmulatorRootPath "sdmc"
    if (Test-Path $sdmcPath) {
        return $sdmcPath
    }

    return (Join-Path $EmulatorRootPath "sdcard")
}

function Convert-Base64UrlToBytes {
    param([Parameter(Mandatory = $true)][string]$Value)

    $normalized = $Value.Replace('-', '+').Replace('_', '/')
    switch ($normalized.Length % 4) {
        2 { $normalized += '==' }
        3 { $normalized += '=' }
    }
    return [Convert]::FromBase64String($normalized)
}

function Parse-MegaFolderLink {
    param([Parameter(Mandatory = $true)][string]$Url)

    if ($Url -match '/folder/([^#]+)#(.+)$') {
        return @{
            FolderId = $Matches[1]
            FolderKey = $Matches[2]
        }
    }

    if ($Url -match '#F!([^!]+)!(.+)$') {
        return @{
            FolderId = $Matches[1]
            FolderKey = $Matches[2]
        }
    }

    throw "Link de pasta do MEGA invalido: $Url"
}

function Invoke-MegaApi {
    param(
        [Parameter(Mandatory = $true)][string]$FolderId,
        [Parameter(Mandatory = $true)][string]$Body
    )

    $uri = "https://g.api.mega.co.nz/cs?id=0&n=$FolderId"
    return Invoke-RestMethod -Method Post -Uri $uri -ContentType "application/json" -Body $Body -TimeoutSec 12
}

function New-AesTransform {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Key,
        [Parameter(Mandatory = $true)][string]$Mode,
        [Parameter(Mandatory = $true)][string]$Transform
    )

    $aes = [System.Security.Cryptography.Aes]::Create()
    $aes.Mode = $Mode
    $aes.Padding = [System.Security.Cryptography.PaddingMode]::None
    $aes.Key = $Key
    if ($Mode -eq "CBC") {
        $aes.IV = [byte[]]::new(16)
    }

    if ($Transform -eq "Encrypt") {
        return @{
            Aes = $aes
            Transform = $aes.CreateEncryptor()
        }
    }

    return @{
        Aes = $aes
        Transform = $aes.CreateDecryptor()
    }
}

function Invoke-AesEcbDecrypt {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Key,
        [Parameter(Mandatory = $true)][byte[]]$Data
    )

    $ctx = New-AesTransform -Key $Key -Mode "ECB" -Transform "Decrypt"
    try {
        return $ctx.Transform.TransformFinalBlock($Data, 0, $Data.Length)
    } finally {
        $ctx.Transform.Dispose()
        $ctx.Aes.Dispose()
    }
}

function Invoke-AesCbcDecrypt {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Key,
        [Parameter(Mandatory = $true)][byte[]]$Data
    )

    $ctx = New-AesTransform -Key $Key -Mode "CBC" -Transform "Decrypt"
    try {
        return $ctx.Transform.TransformFinalBlock($Data, 0, $Data.Length)
    } finally {
        $ctx.Transform.Dispose()
        $ctx.Aes.Dispose()
    }
}

function Invoke-AesCtrCrypt {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Key,
        [Parameter(Mandatory = $true)][byte[]]$InitialCounter,
        [Parameter(Mandatory = $true)][byte[]]$Data
    )

    $ctx = New-AesTransform -Key $Key -Mode "ECB" -Transform "Encrypt"
    try {
        $counter = [byte[]]::new(16)
        [Array]::Copy($InitialCounter, 0, $counter, 0, [Math]::Min($InitialCounter.Length, 16))

        $output = [byte[]]::new($Data.Length)
        $offset = 0
        while ($offset -lt $Data.Length) {
            $keystream = $ctx.Transform.TransformFinalBlock($counter, 0, 16)
            $blockLength = [Math]::Min(16, $Data.Length - $offset)
            for ($index = 0; $index -lt $blockLength; $index++) {
                $output[$offset + $index] = $Data[$offset + $index] -bxor $keystream[$index]
            }

            for ($counterIndex = 15; $counterIndex -ge 0; $counterIndex--) {
                $counter[$counterIndex] = ($counter[$counterIndex] + 1) -band 0xFF
                if ($counter[$counterIndex] -ne 0) {
                    break
                }
            }

            $offset += $blockLength
        }
        return $output
    } finally {
        $ctx.Transform.Dispose()
        $ctx.Aes.Dispose()
    }
}

function Get-MegaFileName {
    param(
        [Parameter(Mandatory = $true)][string]$Attributes,
        [Parameter(Mandatory = $true)][byte[]]$FileKey
    )

    $encrypted = Convert-Base64UrlToBytes $Attributes
    $decrypted = Invoke-AesCbcDecrypt -Key $FileKey -Data $encrypted
    $text = [System.Text.Encoding]::UTF8.GetString($decrypted).Trim([char]0)
    if (-not $text.StartsWith("MEGA")) {
        return $null
    }

    $json = $text.Substring(4) | ConvertFrom-Json
    return [string]$json.n
}

function Get-MegaFileCryptoFromFolderEntry {
    param(
        [Parameter(Mandatory = $true)][byte[]]$FolderKey,
        [Parameter(Mandatory = $true)][string]$EntryKey
    )

    $separator = $EntryKey.IndexOf(':')
    $encodedNodeKey = if ($separator -ge 0) { $EntryKey.Substring($separator + 1) } else { $EntryKey }
    $encryptedNodeKey = Convert-Base64UrlToBytes $encodedNodeKey
    $decryptedNodeKey = Invoke-AesEcbDecrypt -Key $FolderKey -Data $encryptedNodeKey

    $fileKey = [byte[]]::new(16)
    for ($index = 0; $index -lt 16; $index++) {
        $fileKey[$index] = $decryptedNodeKey[$index] -bxor $decryptedNodeKey[$index + 16]
    }

    $iv = [byte[]]::new(16)
    [Array]::Copy($decryptedNodeKey, 16, $iv, 0, 8)

    return @{
        FileKey = $fileKey
        InitialCounter = $iv
    }
}

function Save-MegaFolderIndex {
    param(
        [Parameter(Mandatory = $true)][string]$FolderUrl,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    $link = Parse-MegaFolderLink -Url $FolderUrl
    $folderKey = Convert-Base64UrlToBytes $link.FolderKey

    $listResponse = Invoke-MegaApi -FolderId $link.FolderId -Body '[{"a":"f","c":1,"ca":1,"r":1}]'
    $nodes = @($listResponse[0].f)
    if (-not $nodes) {
        throw "A pasta do MEGA nao retornou arquivos."
    }

    $root = $nodes | Where-Object { $_.t -eq 1 } | Select-Object -First 1
    if (-not $root) {
        throw "Nao foi possivel identificar a raiz da pasta do MEGA."
    }

    $selected = $null
    foreach ($entry in $nodes) {
        if ($entry.t -eq 1 -or $entry.p -ne $root.h) {
            continue
        }

        $crypto = Get-MegaFileCryptoFromFolderEntry -FolderKey $folderKey -EntryKey ([string]$entry.k)
        $fileName = Get-MegaFileName -Attributes ([string]$entry.a) -FileKey $crypto.FileKey
        if (-not $selected) {
            $selected = @{
                Entry = $entry
                Crypto = $crypto
                Name = $fileName
            }
        }
        if ($fileName -and $fileName.ToLowerInvariant() -eq "index.json") {
            $selected = @{
                Entry = $entry
                Crypto = $crypto
                Name = $fileName
            }
            break
        }
    }

    if (-not $selected) {
        throw "Nenhum arquivo valido foi encontrado na pasta do MEGA."
    }

    $downloadBody = '[{"a":"g","g":1,"n":"' + $selected.Entry.h + '"}]'
    $downloadResponse = Invoke-MegaApi -FolderId $link.FolderId -Body $downloadBody
    $downloadUrl = [string]$downloadResponse[0].g
    if (-not $downloadUrl) {
        throw "A pasta do MEGA nao retornou URL de download para o indice."
    }

    $tempPath = [System.IO.Path]::GetTempFileName()
    try {
        Invoke-WebRequest -Uri $downloadUrl -OutFile $tempPath -TimeoutSec 20 | Out-Null
        $encryptedBytes = [System.IO.File]::ReadAllBytes($tempPath)
        $aesCtr = [System.Security.Cryptography.Aes]::Create()
        $aesCtr.Mode = [System.Security.Cryptography.CipherMode]::ECB
        $aesCtr.Padding = [System.Security.Cryptography.PaddingMode]::None
        $aesCtr.Key = $selected.Crypto.FileKey
        $encryptor = $aesCtr.CreateEncryptor()
        try {
            $counter = [byte[]]::new(16)
            [Array]::Copy($selected.Crypto.InitialCounter, 0, $counter, 0, 16)
            $plainBytes = [byte[]]::new($encryptedBytes.Length)

            for ($offset = 0; $offset -lt $encryptedBytes.Length; $offset += 16) {
                $keystream = $encryptor.TransformFinalBlock($counter, 0, 16)
                $blockLength = [Math]::Min(16, $encryptedBytes.Length - $offset)
                for ($index = 0; $index -lt $blockLength; $index++) {
                    $plainBytes[$offset + $index] = $encryptedBytes[$offset + $index] -bxor $keystream[$index]
                }

                for ($counterIndex = 15; $counterIndex -ge 0; $counterIndex--) {
                    $counter[$counterIndex] = ($counter[$counterIndex] + 1) -band 0xFF
                    if ($counter[$counterIndex] -ne 0) {
                        break
                    }
                }
            }
        } finally {
            $encryptor.Dispose()
            $aesCtr.Dispose()
        }

        $destinationDir = Split-Path -Parent $DestinationPath
        New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
        [System.IO.File]::WriteAllBytes($DestinationPath, $plainBytes)
    } finally {
        Remove-Item $tempPath -ErrorAction SilentlyContinue
    }
}

function Save-RemoteIndex {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    if ($Url -match 'mega\.nz/(folder|#F!)') {
        Save-MegaFolderIndex -FolderUrl $Url -DestinationPath $DestinationPath
        return
    }

    $destinationDir = Split-Path -Parent $DestinationPath
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    Invoke-WebRequest -Uri $Url -OutFile $DestinationPath -TimeoutSec 20 | Out-Null
}

function Save-UrlToFile {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    $destinationDir = Split-Path -Parent $DestinationPath
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    Invoke-WebRequest -Uri $Url -OutFile $DestinationPath -TimeoutSec 20 -Headers @{
        "User-Agent" = "MILManagerCatalogSync/1.0"
    } | Out-Null
}

function Get-ThumbnailSourceUrl {
    param([Parameter(Mandatory = $true)]$Entry)

    $iconUrl = [string]$Entry.iconUrl
    $thumbUrl = [string]$Entry.thumbnailUrl
    $coverUrl = [string]$Entry.coverUrl

    if ($thumbUrl) {
        return @{
            Primary = $thumbUrl
            Fallback = if ($coverUrl -and $coverUrl -ne $thumbUrl) { $coverUrl } elseif ($iconUrl -and $iconUrl -ne $thumbUrl) { $iconUrl } else { "" }
        }
    }

    if ($coverUrl) {
        return @{
            Primary = $coverUrl
            Fallback = if ($iconUrl -and $iconUrl -ne $coverUrl) { $iconUrl } else { "" }
        }
    }

    if ($iconUrl) {
        return @{
            Primary = $iconUrl
            Fallback = ""
        }
    }

    return @{
        Primary = ""
        Fallback = ""
    }
}

function Test-ThumbnailImageValid {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$ExpectedSize
    )

    if (-not (Test-Path $Path)) {
        return $false
    }

    try {
        Add-Type -AssemblyName System.Drawing
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            $image = [System.Drawing.Image]::FromStream($stream, $true, $true)
            try {
                return $image.Width -eq $ExpectedSize -and $image.Height -eq $ExpectedSize
            } finally {
                $image.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
    } catch {
        return $false
    }
}

function Convert-ImageToNormalizedPng {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][int]$TargetSize
    )

    Add-Type -AssemblyName System.Drawing
    Add-Type -AssemblyName System.Drawing.Drawing2D
    Add-Type -AssemblyName System.Drawing.Imaging

    $sourceStream = [System.IO.File]::OpenRead($SourcePath)
    try {
        $sourceImage = [System.Drawing.Image]::FromStream($sourceStream, $true, $true)
        try {
            $bitmap = New-Object System.Drawing.Bitmap($TargetSize, $TargetSize, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
                try {
                    $graphics.Clear([System.Drawing.Color]::Transparent)
                    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
                    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

                    $scale = [Math]::Min($TargetSize / [double]$sourceImage.Width, $TargetSize / [double]$sourceImage.Height)
                    $drawWidth = [Math]::Max(1, [int][Math]::Round($sourceImage.Width * $scale))
                    $drawHeight = [Math]::Max(1, [int][Math]::Round($sourceImage.Height * $scale))
                    $drawX = [int][Math]::Floor(($TargetSize - $drawWidth) / 2.0)
                    $drawY = [int][Math]::Floor(($TargetSize - $drawHeight) / 2.0)

                    $graphics.DrawImage($sourceImage, $drawX, $drawY, $drawWidth, $drawHeight)
                } finally {
                    $graphics.Dispose()
                }

                $bitmap.Save($DestinationPath, [System.Drawing.Imaging.ImageFormat]::Png)
            } finally {
                $bitmap.Dispose()
            }
        } finally {
            $sourceImage.Dispose()
        }
    } finally {
        $sourceStream.Dispose()
    }
}

function Save-UrlToNormalizedImage {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][int]$TargetSize
    )

    $tempPath = [System.IO.Path]::GetTempFileName()
    try {
        Save-UrlToFile -Url $Url -DestinationPath $tempPath
        Convert-ImageToNormalizedPng -SourcePath $tempPath -DestinationPath $DestinationPath -TargetSize $TargetSize
    } finally {
        Remove-Item $tempPath -ErrorAction SilentlyContinue
    }
}

function Save-CatalogThumbnails {
    param(
        [Parameter(Mandatory = $true)][string]$IndexPath,
        [Parameter(Mandatory = $true)][string]$DestinationDir
    )

    if (-not (Test-Path $IndexPath)) {
        return
    }

    $raw = Get-Content $IndexPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $entries = @($raw.entries)
    if (-not $entries) {
        return
    }

    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null

    $downloaded = 0
    $skipped = 0
    $failed = 0

    foreach ($entry in $entries) {
        $id = [string]$entry.id
        if (-not $id) {
            continue
        }

        $destinationPath = Join-Path $DestinationDir ($id + ".img")
        $primaryUrl = [string]$entry.thumbnailUrl
        $fallbackUrl = [string]$entry.coverUrl
        if (-not $primaryUrl) {
            $primaryUrl = [string]$entry.iconUrl
        }
        if (-not $primaryUrl) {
            $primaryUrl = $fallbackUrl
        }
        if (-not $primaryUrl) {
            $skipped++
            continue
        }

        if ((Test-Path $destinationPath) -and ((Get-Item $destinationPath).Length -gt 0)) {
            $skipped++
            continue
        }

        try {
            Save-UrlToFile -Url $primaryUrl -DestinationPath $destinationPath
            if (-not (Test-Path $destinationPath) -or ((Get-Item $destinationPath).Length -le 0)) {
                throw "Arquivo vazio"
            }
            $downloaded++
            continue
        } catch {
            Remove-Item $destinationPath -ErrorAction SilentlyContinue
            if ($fallbackUrl -and $fallbackUrl -ne $primaryUrl) {
                try {
                    Save-UrlToFile -Url $fallbackUrl -DestinationPath $destinationPath
                    if (-not (Test-Path $destinationPath) -or ((Get-Item $destinationPath).Length -le 0)) {
                        throw "Arquivo vazio"
                    }
                    $downloaded++
                    continue
                } catch {
                    Remove-Item $destinationPath -ErrorAction SilentlyContinue
                }
            }
            $failed++
        }
    }

    Write-Host "Thumbs do catálogo: baixados=$downloaded ignorados=$skipped falhas=$failed"
}

function Save-CatalogThumbnailsNormalized {
    param(
        [Parameter(Mandatory = $true)][string]$IndexPath,
        [Parameter(Mandatory = $true)][string]$DestinationDir
    )

    if (-not (Test-Path $IndexPath)) {
        return
    }

    $raw = Get-Content $IndexPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $entries = @($raw.entries)
    if (-not $entries) {
        return
    }

    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null

    $thumbSize = 110
    $manifestPath = Join-Path $DestinationDir "thumbs-manifest.json"
    $manifest = @{}
    if (Test-Path $manifestPath) {
        try {
            $loadedManifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
            if ($loadedManifest) {
                $manifest = $loadedManifest
            }
        } catch {
            $manifest = @{}
        }
    }

    $downloaded = 0
    $skipped = 0
    $failed = 0

    foreach ($entry in $entries) {
        $id = [string]$entry.id
        if (-not $id) {
            continue
        }

        $destinationPath = Join-Path $DestinationDir ($id + ".img")
        $sources = Get-ThumbnailSourceUrl -Entry $entry
        $primaryUrl = [string]$sources.Primary
        $fallbackUrl = [string]$sources.Fallback
        if (-not $primaryUrl) {
            $skipped++
            continue
        }

        $existingManifest = $manifest[$id]
        $isCurrent = $false
        if ($existingManifest) {
            $manifestUrl = [string]$existingManifest.primaryUrl
            $manifestFallback = [string]$existingManifest.fallbackUrl
            $manifestSize = [int]$existingManifest.size
            $isCurrent = $manifestUrl -eq $primaryUrl -and
                         $manifestFallback -eq $fallbackUrl -and
                         $manifestSize -eq $thumbSize -and
                         (Test-ThumbnailImageValid -Path $destinationPath -ExpectedSize $thumbSize)
        }

        if ($isCurrent) {
            $skipped++
            continue
        }

        try {
            Save-UrlToNormalizedImage -Url $primaryUrl -DestinationPath $destinationPath -TargetSize $thumbSize
            if (-not (Test-ThumbnailImageValid -Path $destinationPath -ExpectedSize $thumbSize)) {
                throw "Arquivo inválido"
            }
            $manifest[$id] = @{
                primaryUrl = $primaryUrl
                fallbackUrl = $fallbackUrl
                size = $thumbSize
            }
            $downloaded++
            continue
        } catch {
            Remove-Item $destinationPath -ErrorAction SilentlyContinue
            if ($fallbackUrl -and $fallbackUrl -ne $primaryUrl) {
                try {
                    Save-UrlToNormalizedImage -Url $fallbackUrl -DestinationPath $destinationPath -TargetSize $thumbSize
                    if (-not (Test-ThumbnailImageValid -Path $destinationPath -ExpectedSize $thumbSize)) {
                        throw "Arquivo inválido"
                    }
                    $manifest[$id] = @{
                        primaryUrl = $fallbackUrl
                        fallbackUrl = ""
                        size = $thumbSize
                    }
                    $downloaded++
                    continue
                } catch {
                    Remove-Item $destinationPath -ErrorAction SilentlyContinue
                }
            }
            $manifest.Remove($id)
            $failed++
        }
    }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5), $utf8NoBom)

    Write-Host "Thumbs do catálogo: baixados=$downloaded ignorados=$skipped falhas=$failed"
}

function Get-EmulatorGameVersion {
    param([Parameter(Mandatory = $true)][string]$TitleDir)

    $cpuCacheDir = Join-Path $TitleDir "cache\\cpu"
    if (-not (Test-Path $cpuCacheDir)) {
        return ""
    }

    $versions = Get-ChildItem $cpuCacheDir -Recurse -Filter *.cache -File |
        ForEach-Object {
            $parts = $_.BaseName.Split('-', 2)
            if ($parts[0] -and $parts[0].ToLowerInvariant() -ne "default") { $parts[0] }
        } |
        Sort-Object -Unique

    if (-not $versions) {
        return ""
    }

    return [string]($versions | Select-Object -Last 1)
}

function Export-InstalledTitlesFromGamesCache {
    param(
        [Parameter(Mandatory = $true)][string]$EmulatorRootPath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    $gamesDir = Join-Path $EmulatorRootPath "games"
    $titles = @()
    if (Test-Path $gamesDir) {
        foreach ($child in Get-ChildItem $gamesDir -Directory | Sort-Object Name) {
            if ($child.Name -notmatch '^[0-9A-Fa-f]{16}$') {
                continue
            }

            $titles += [ordered]@{
                titleId = $child.Name.ToUpperInvariant()
                displayVersion = (Get-EmulatorGameVersion -TitleDir $child.FullName)
            }
        }
    }

    $payload = [ordered]@{
        generatedAt = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        source = $gamesDir
        titles = $titles
    }

    $destinationDir = Split-Path -Parent $DestinationPath
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    $json = $payload | ConvertTo-Json -Depth 5
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($DestinationPath, $json, $utf8NoBom)
}

$resolvedRoot = if ($EmulatorRoot) { $EmulatorRoot } else { Resolve-DefaultEmulatorRoot }
$sdRoot = Get-EmulatorSdRoot -EmulatorRootPath $resolvedRoot
$switchDir = Join-Path $sdRoot "switch\\mil_manager"
$cacheDir = Join-Path $switchDir "cache"
$thumbnailCacheDir = Join-Path $cacheDir "images"
$indexPath = Join-Path $cacheDir "index.json"
$cheatsIndexPath = Join-Path $cacheDir "cheats-index.json"
$normalizedInstalledPath = Join-Path $cacheDir "installed-titles-cache.json"
$cheatPackZipPath = Join-Path $cacheDir "cheats-pack.zip"
$cheatPackRootDir = Join-Path $cacheDir "cheat-packs"
$cheatPackRevisionPath = Join-Path $cacheDir "cheat-pack-revision.txt"
$thumbnailHelper = Join-Path $PSScriptRoot "prepare_catalog_thumbs.py"
$python = Get-Command python -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path $switchDir | Out-Null
New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
Remove-Item (Join-Path $switchDir "index.json") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $switchDir "cheats-index.json") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $switchDir "installed-titles-cache.json") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $switchDir "emulator-installed.json") -ErrorAction SilentlyContinue

$resolvedCatalogUrl = if ($MegaFolderUrl) { $MegaFolderUrl } else { $CatalogUrl }
Save-RemoteIndex -Url $resolvedCatalogUrl -DestinationPath $indexPath
if ($resolvedCatalogUrl -match 'index\.json$') {
    $resolvedCheatsIndexUrl = $resolvedCatalogUrl -replace 'index\.json$', 'cheats-index.json'
    Save-RemoteIndex -Url $resolvedCheatsIndexUrl -DestinationPath $cheatsIndexPath

    if (Test-Path $cheatsIndexPath) {
        $cheatsIndex = Get-Content $cheatsIndexPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $cheatsPackUrl = ""
        $cheatsPackRevision = ""

        if ($cheatsIndex.PSObject.Properties.Name -contains "cheatsPackUrl") {
            $cheatsPackUrl = [string]$cheatsIndex.cheatsPackUrl
        } elseif ($resolvedCheatsIndexUrl -match 'cheats-index\.json$') {
            $cheatsPackUrl = $resolvedCheatsIndexUrl -replace 'cheats-index\.json$', 'cheats-pack.zip'
        }

        if ($cheatsIndex.PSObject.Properties.Name -contains "cheatsPackRevision") {
            $cheatsPackRevision = [string]$cheatsIndex.cheatsPackRevision
        } elseif ($cheatsIndex.PSObject.Properties.Name -contains "catalogRevision") {
            $cheatsPackRevision = [string]$cheatsIndex.catalogRevision
        } elseif ($cheatsIndex.PSObject.Properties.Name -contains "generatedAt") {
            $cheatsPackRevision = [string]$cheatsIndex.generatedAt
        }

        if ($cheatsPackUrl -and $cheatsPackRevision) {
            $revision = $cheatsPackRevision
            $packDir = Join-Path $cheatPackRootDir $revision
            New-Item -ItemType Directory -Force -Path $packDir | Out-Null
            try {
                Invoke-WebRequest -UseBasicParsing -Uri $cheatsPackUrl -OutFile $cheatPackZipPath
                if (Test-Path $cheatPackZipPath) {
                    Expand-Archive -LiteralPath $cheatPackZipPath -DestinationPath $packDir -Force
                    [System.IO.File]::WriteAllText($cheatPackRevisionPath, "$revision`n", (New-Object System.Text.UTF8Encoding($false)))
                    Remove-Item $cheatPackZipPath -ErrorAction SilentlyContinue
                }
            } catch {
                Remove-Item $cheatPackZipPath -ErrorAction SilentlyContinue
            }
        }
    }
}
if ($python -and (Test-Path $thumbnailHelper)) {
    & $python.Source $thumbnailHelper $indexPath $thumbnailCacheDir 110
} else {
    Save-CatalogThumbnails -IndexPath $indexPath -DestinationDir $thumbnailCacheDir
}

$syncUtility = Join-Path $PSScriptRoot "mil_emulator_sync.py"
if ($python -and (Test-Path $syncUtility)) {
    & $python.Source $syncUtility --emulator auto --root $resolvedRoot --output $normalizedInstalledPath
} else {
    Export-InstalledTitlesFromGamesCache -EmulatorRootPath $resolvedRoot -DestinationPath $normalizedInstalledPath
}

Write-Host "Catalogo sincronizado em: $indexPath"
Write-Host "Cheats sincronizados em: $cheatsIndexPath"
Write-Host "Thumbs sincronizados em: $thumbnailCacheDir"
Write-Host "Cache normalizado em: $normalizedInstalledPath"
function Resolve-DefaultEmulatorRoot {
    $candidates = @(
        (Join-Path $env:APPDATA "Ryujinx"),
        (Join-Path $env:APPDATA "eden")
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Nao foi possivel localizar a raiz padrao do emulador. Informe -EmulatorRoot explicitamente."
}
