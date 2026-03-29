# Tools And Operations

## Visao geral

As tools locais do `mil-manager` existem para:

- sincronizar bibliotecas de emuladores
- exportar estado instalado para cache local
- auxiliar a execucao e validacao do app

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
- `export-emulator-installed.py`
  - wrapper util para exportacao do estado instalado do emulador
