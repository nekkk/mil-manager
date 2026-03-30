# Build And Release

## Requisitos

- devkitPro
- libnx / toolchain Switch do devkitPro
- CMake 3.20+
- Python 3 para as tools locais de emulador

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
- idiomas em `romfs/lang/*.json`

Sempre que o NRO for recompilado, o ROMFS atualizado entra junto no artefato.

## Fluxo de release recomendado

1. implementar uma mudanca
2. validar no console e nos emuladores suportados
3. salvar commit da etapa
4. marcar tag quando fizer sentido
5. compilar build final
6. anexar NRO e, se aplicavel, NSP forwarder na GitHub Release do repositório publico
7. se a mudanca tocar o catalogo, operar isso no repositório `mil-manager-catalog`

## Publicacao do app

Repositorio alvo:

- `mil-manager`

Comandos comuns:

```powershell
git push origin main
git push origin --tags
```

## Publicacao do catalogo e delivery

O `mil-manager` nao gera mais o catalogo nem publica o delivery diretamente.

Essas operacoes agora pertencem ao repositório:

- `mil-manager-catalog`

Consulte a documentacao operacional desse repositório para:

- geracao dos indices
- publicacao do `mil-manager-delivery`
- operacao do painel admin
- automacao via GitHub Actions
