# Tools And Operations

## Visao geral

As tools locais do `mil-manager` existem para:

- gerar catalogos
- preparar o delivery publico
- sincronizar bibliotecas de emuladores
- publicar diferencas com menos trabalho manual

## Geracao de catalogo

```powershell
python tools\generate-index.py
python tools\generate-cheats-index.py
python tools\generate-saves-index.py
python tools\prepare-pages-site.py
```

Saidas principais:

- `dist/index.json`
- `dist/cheats-index.json`
- `dist/cheats-summary.json`
- `dist/saves-index.json`
- `dist/saves-summary.json`
- `site/`

## Publicacao para delivery

Script:

- [sync-mil-manager-delivery.ps1](C:/Users/lordd/source/codex/mil-manager/tools/sync-mil-manager-delivery.ps1)

Uso:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit -Push
```

Quando o lote estiver grande:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit -Push -BatchLargePush
```

## Sincronizacao de emuladores

Utilitario base:

```powershell
python tools\mil_emulator_sync.py --emulator auto
```

Sync simplificado:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-emulator.ps1
```

Launcher com sincronizacao:

```powershell
powershell -ExecutionPolicy Bypass -File tools\start-emulator-with-sync.ps1
```

## Nomes e objetivos atuais

- `sync-emulator.ps1`
  - sincroniza catalogo, thumbs e cache normalizado para o emulador detectado
- `start-emulator-with-sync.ps1`
  - sincroniza e depois abre o executavel do emulador
- `mil_emulator_sync.py`
  - gera `installed-titles-cache.json` e aplica operacoes de save pendentes
