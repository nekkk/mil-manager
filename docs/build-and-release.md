# Build And Release

## Requisitos

- devkitPro
- libnx / toolchain Switch do devkitPro
- CMake 3.20+
- Python 3 para as tools do catalogo

## Build local

```powershell
cmake -S . -B build-switch2 -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=C:/devkitPro/cmake/Switch.cmake -DDEVKITPRO=C:/devkitPro
cmake --build build-switch2 --config Release
```

Artefatos principais:

- [mil_manager.nro](C:/Users/lordd/source/codex/mil-manager/build-switch2/mil_manager.nro)
- `mil_manager.elf`
- `mil_manager.nacp`

## ROMFS

O `romfs/` embute:

- fontes
- icones
- imagens
- idiomas em `romfs/lang/*.json`

Sempre que o NRO for recompilado, o ROMFS atualizado entra junto no artefato.

## Fluxo de release recomendado

1. implementar uma mudanca
2. validar no console e nos emuladores suportados
3. salvar commit da etapa
4. marcar tag quando fizer sentido
5. compilar build final
6. anexar NRO na release do repositório publico
7. atualizar o delivery sanitizado quando a mudanca tocar catalogo

## Publicacao do app

Repositorio alvo:

- `mil-manager`

Comandos comuns:

```powershell
git push origin main
git push origin --tags
```

## Publicacao do delivery

Geracao:

```powershell
python tools\generate-index.py
python tools\generate-cheats-index.py
python tools\generate-saves-index.py
python tools\prepare-pages-site.py
```

Sincronizacao:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit -Push
```

Quando o lote for grande:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit -Push -BatchLargePush
```
