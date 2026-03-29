# mil-manager

Aplicativo homebrew para Nintendo Switch focado em listar, baixar e instalar:

- traduções e dublagens
- modificações
- trapaças
- jogos salvos

O app consome catálogos sanitizados publicados em `mil-manager-delivery`. Este repositório público fica focado no código do aplicativo, documentação e releases.

## Início Rapido

Build local:

```powershell
cmake -S . -B build-switch2 -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=C:/devkitPro/cmake/Switch.cmake -DDEVKITPRO=C:/devkitPro
cmake --build build-switch2 --config Release
```

Artefato principal:

- [mil_manager.nro](C:/Users/lordd/source/codex/mil-manager/build-switch2/mil_manager.nro)

Release oficial atual:

- [releases/v1.0](C:/Users/lordd/source/codex/mil-manager/releases/v1.0)

Catálogo default atual:

- [https://nekkk.github.io/mil-manager-delivery/index.json](https://nekkk.github.io/mil-manager-delivery/index.json)

## Documentação

Visão geral:

- [features.md](C:/Users/lordd/source/codex/mil-manager/docs/features.md)
- [build-and-release.md](C:/Users/lordd/source/codex/mil-manager/docs/build-and-release.md)
- [app-structure.md](C:/Users/lordd/source/codex/mil-manager/docs/app-structure.md)
- [code-maintenance-guide.md](C:/Users/lordd/source/codex/mil-manager/docs/code-maintenance-guide.md)
- [technical-stack.md](C:/Users/lordd/source/codex/mil-manager/docs/technical-stack.md)
- [credits-and-references.md](C:/Users/lordd/source/codex/mil-manager/docs/credits-and-references.md)

Operação:

- [tools-and-operations.md](C:/Users/lordd/source/codex/mil-manager/docs/tools-and-operations.md)

Schemas e notas específicas:

- [emulator-manifest-v2.md](C:/Users/lordd/source/codex/mil-manager/docs/emulator-manifest-v2.md)

## Repositórios

- `mil-manager`: codigo-fonte publico do app, releases e documentacao
- `mil-manager-delivery`: artefatos sanitizados publicados para consumo do app

## Estrutura Local Importante

No SD:

- `sdmc:/switch/mil_manager/`
- `sdmc:/switch/mil_manager/cache/`
- `sdmc:/switch/mil_manager/receipts/`

Configuração:

- `sdmc:/config/mil_manager/settings.ini`

## Emuladores

O homebrew não enxerga sozinho a biblioteca do host. O fluxo suportado é sincronizar o catálogo e a biblioteca antes de abrir o app:

```powershell
python tools\mil_emulator_sync.py --emulator auto
powershell -ExecutionPolicy Bypass -File tools\sync-emulator.ps1
powershell -ExecutionPolicy Bypass -File tools\start-emulator-with-sync.ps1
```

Detalhes e cuidados operacionais:

- [tools-and-operations.md](C:/Users/lordd/source/codex/mil-manager/docs/tools-and-operations.md)
