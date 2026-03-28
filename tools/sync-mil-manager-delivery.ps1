param(
    [string]$DeliveryRepoPath = (Join-Path $PSScriptRoot "..\..\mil-manager-delivery"),
    [string]$SourceRepoPath = (Join-Path $PSScriptRoot ".."),
    [string]$CommitMessage = "",
    [switch]$Commit,
    [switch]$Push,
    [switch]$BatchLargePush
)

$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Assert-PathExists {
    param(
        [string]$Path,
        [string]$Description
    )

    if (-not (Test-Path $Path)) {
        throw "$Description não encontrado: $Path"
    }
}

function Invoke-RobocopyMirror {
    param(
        [string]$SourcePath,
        [string]$TargetPath
    )

    New-Item -ItemType Directory -Path $TargetPath -Force | Out-Null
    & robocopy $SourcePath $TargetPath /MIR /NFL /NDL /NJH /NJS /NC /NS /NP | Out-Null
    $exitCode = $LASTEXITCODE
    if ($exitCode -gt 7) {
        throw "Falha ao sincronizar diretório com robocopy: $SourcePath -> $TargetPath (código $exitCode)"
    }
}

function Sync-DeliveryTree {
    param(
        [string]$SourceRoot,
        [string]$TargetRoot
    )

    $sourceItems = Get-ChildItem -LiteralPath $SourceRoot -Force
    $sourceNames = @{}
    foreach ($item in $sourceItems) {
        $sourceNames[$item.Name] = $true
    }

    Get-ChildItem -LiteralPath $TargetRoot -Force |
        Where-Object { $_.Name -ne ".git" -and -not $sourceNames.ContainsKey($_.Name) } |
        Remove-Item -Recurse -Force

    foreach ($item in $sourceItems) {
        $targetPath = Join-Path $TargetRoot $item.Name
        if ($item.PSIsContainer) {
            Invoke-RobocopyMirror -SourcePath $item.FullName -TargetPath $targetPath
        } else {
            Copy-Item -LiteralPath $item.FullName -Destination $targetPath -Force
        }
    }
}

function Has-RemoteMain {
    & git ls-remote --exit-code --heads origin main *> $null
    return $LASTEXITCODE -eq 0
}

function Sync-WithRemoteMain {
    if (-not (Has-RemoteMain)) {
        return $false
    }

    Write-Step "Sincronizando com origin/main antes do push"
    & git fetch origin main
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao executar git fetch."
    }

    & git rebase origin/main
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao executar git rebase origin/main."
    }

    return $true
}

function Invoke-PushMain {
    param([switch]$Initial)

    if ($Initial) {
        Write-Step "Primeira publicação detectada; enviando branch main inicial"
        & git push -u origin main
        if ($LASTEXITCODE -ne 0) {
            throw "Falha ao executar o primeiro git push."
        }
        return
    }

    Write-Step "Enviando para origin/main"
    & git push origin main
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao executar git push."
    }
}

function Commit-PathsIfNeeded {
    param(
        [string]$Message,
        [string[]]$Paths
    )

    if (-not $Paths -or $Paths.Count -eq 0) {
        return $false
    }

    & git add -A -- @Paths
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao executar git add para o lote."
    }

    & git diff --cached --quiet -- @Paths
    $diffExit = $LASTEXITCODE
    if ($diffExit -eq 0) {
        return $false
    }
    if ($diffExit -ne 1) {
        throw "Falha ao verificar alterações staged do lote."
    }

    & git commit -m $Message
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao criar commit do lote."
    }

    return $true
}

function Invoke-BatchedPublish {
    $remoteMainExists = Sync-WithRemoteMain
    $initialPushDone = $false
    $baseCommitMessage = $CommitMessage
    if ([string]::IsNullOrWhiteSpace($baseCommitMessage)) {
        $baseCommitMessage = "Publish sanitized delivery base assets"
    }

    $basePaths = @(
        ".nojekyll",
        "index.html",
        "index.json",
        "cheats-index.json",
        "cheats-summary.json",
        "saves-index.json",
        "saves-summary.json",
        "cheats-manifest.json",
        "cheats-pack.zip",
        "thumbs-manifest.json",
        "thumbs-pack.zip",
        "admin",
        "thumbs",
        "delivery/cheats",
        "delivery/packages",
        "delivery/packs"
    ) | Where-Object { Test-Path $_ }

    if (Commit-PathsIfNeeded -Message $baseCommitMessage -Paths $basePaths) {
        Invoke-PushMain -Initial:(-not $remoteMainExists -and -not $initialPushDone)
        $initialPushDone = $true
        $remoteMainExists = $true
    }

    $saveRoot = "delivery/saves"
    if (Test-Path $saveRoot) {
        $groups = @(
            @{ Label = "0-3"; Prefixes = @("0","1","2","3") },
            @{ Label = "4-7"; Prefixes = @("4","5","6","7") },
            @{ Label = "8-b"; Prefixes = @("8","9","a","b") },
            @{ Label = "c-f"; Prefixes = @("c","d","e","f") }
        )

        foreach ($group in $groups) {
            $paths = @()
            foreach ($prefix in $group.Prefixes) {
                $candidate = Join-Path $saveRoot $prefix
                if (Test-Path $candidate) {
                    $paths += $candidate
                }
            }
            if (-not $paths) {
                continue
            }

            if (Commit-PathsIfNeeded -Message ("Publish sanitized delivery saves batch " + $group.Label) -Paths $paths) {
                Invoke-PushMain -Initial:(-not $remoteMainExists -and -not $initialPushDone)
                $initialPushDone = $true
                $remoteMainExists = $true
            }
        }
    }
}

$resolvedSourceRepo = (Resolve-Path $SourceRepoPath).Path
$resolvedDeliveryRepo = (Resolve-Path $DeliveryRepoPath).Path
$sitePath = Join-Path $resolvedSourceRepo "site"

Assert-PathExists -Path (Join-Path $resolvedDeliveryRepo ".git") -Description "Repositório do delivery"
Assert-PathExists -Path $sitePath -Description "Saída sanitizada do site"

Write-Step "Sincronizando site sanitizado para o repositório mil-manager-delivery"
Sync-DeliveryTree -SourceRoot $sitePath -TargetRoot $resolvedDeliveryRepo

Push-Location $resolvedDeliveryRepo
try {
    Write-Step "Status atual do repositório do delivery"
    & git status --short
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao consultar git status."
    }

    if ($BatchLargePush -and ($Commit -or $Push)) {
        Invoke-BatchedPublish
    } elseif ($Commit -or $Push) {
        $finalCommitMessage = $CommitMessage
        if ([string]::IsNullOrWhiteSpace($finalCommitMessage)) {
            $finalCommitMessage = "Publish sanitized delivery artifacts"
        }

        Write-Step "Adicionando alterações ao git"
        & git add -A .
        if ($LASTEXITCODE -ne 0) {
            throw "Falha ao executar git add."
        }

        & git diff --cached --quiet
        $diffExit = $LASTEXITCODE
        if ($diffExit -eq 1) {
            Write-Step "Criando commit"
            & git commit -m $finalCommitMessage
            if ($LASTEXITCODE -ne 0) {
                throw "Falha ao criar commit."
            }
        } elseif ($diffExit -ne 0) {
            throw "Falha ao verificar alterações staged."
        } else {
            Write-Host "Nenhuma alteração nova para commit." -ForegroundColor Yellow
        }

        if ($Push) {
            $remoteMainExists = Sync-WithRemoteMain
            Invoke-PushMain -Initial:(-not $remoteMainExists)
        }
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "Publicação do delivery concluída." -ForegroundColor Green
Write-Host "Repositório do delivery: $resolvedDeliveryRepo"
Write-Host ""
Write-Host "Uso rápido:" -ForegroundColor Cyan
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit -Push"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit -Push -BatchLargePush"
