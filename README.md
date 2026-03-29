# mil-manager

Aplicativo homebrew para Nintendo Switch focado em listar, baixar e instalar:

- traducoes e dublagens
- modificacoes
- trapacas
- jogos salvos

O app consome catalogos sanitizados publicados em `mil-manager-delivery` e usa `mil-manager-catalog` apenas como origem de gerenciamento e geracao.

## Inicio Rapido

Build local:

```powershell
cmake -S . -B build-switch2 -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=C:/devkitPro/cmake/Switch.cmake -DDEVKITPRO=C:/devkitPro
cmake --build build-switch2 --config Release
```

Artefato principal:

- [mil_manager.nro](C:/Users/lordd/source/codex/mil-manager/build-switch2/mil_manager.nro)

Catalogo default atual:

- [https://nekkk.github.io/mil-manager-delivery/index.json](https://nekkk.github.io/mil-manager-delivery/index.json)

## Documentacao

Visao geral:

- [features.md](C:/Users/lordd/source/codex/mil-manager/docs/features.md)
- [build-and-release.md](C:/Users/lordd/source/codex/mil-manager/docs/build-and-release.md)
- [app-structure.md](C:/Users/lordd/source/codex/mil-manager/docs/app-structure.md)
- [technical-stack.md](C:/Users/lordd/source/codex/mil-manager/docs/technical-stack.md)
- [credits-and-references.md](C:/Users/lordd/source/codex/mil-manager/docs/credits-and-references.md)

Operacao:

- [tools-and-operations.md](C:/Users/lordd/source/codex/mil-manager/docs/tools-and-operations.md)
- [implementation-plan.md](C:/Users/lordd/source/codex/mil-manager/docs/implementation-plan.md)
- [delivery-architecture-v1.md](C:/Users/lordd/source/codex/mil-manager/docs/delivery-architecture-v1.md)
- [repo-split-rollout.md](C:/Users/lordd/source/codex/mil-manager/docs/repo-split-rollout.md)

Schemas e notas especificas:

- [cheats-index-v1.md](C:/Users/lordd/source/codex/mil-manager/docs/cheats-index-v1.md)
- [savegames-aggregation-v1.md](C:/Users/lordd/source/codex/mil-manager/docs/savegames-aggregation-v1.md)
- [emulator-manifest-v2.md](C:/Users/lordd/source/codex/mil-manager/docs/emulator-manifest-v2.md)

## Repositorios

- `mil-manager`: codigo-fonte publico do app, releases e documentacao
- `mil-manager-catalog`: repositorio de gerenciamento do catalogo e do painel admin
- `mil-manager-delivery`: artefatos sanitizados publicados para consumo do app

## Estrutura Local Importante

Na SD:

- `sdmc:/switch/mil_manager/`
- `sdmc:/switch/mil_manager/cache/`
- `sdmc:/switch/mil_manager/receipts/`

Configuracao:

- `sdmc:/config/mil_manager/settings.ini`

Compatibilidade legada:

- o app ainda tolera leitura de caminhos antigos em `sdmc:/config/mil-manager/`
- novas gravacoes usam `mil_manager`

## Emuladores

O homebrew nao enxerga sozinho a biblioteca do host. O fluxo suportado e sincronizar o catalogo e a biblioteca antes de abrir o app:

```powershell
python tools\mil_emulator_sync.py --emulator auto
powershell -ExecutionPolicy Bypass -File tools\sync-emulator.ps1
powershell -ExecutionPolicy Bypass -File tools\start-emulator-with-sync.ps1
```

Detalhes e cuidados operacionais:

- [tools-and-operations.md](C:/Users/lordd/source/codex/mil-manager/docs/tools-and-operations.md)

## Seguranca Operacional

- o repositorio publico do app nao deve conter tokens, PATs ou URLs privadas
- o delivery publico publica apenas artefatos sanitizados
- as tools locais de sincronizacao trabalham com caminhos do usuario, nao com segredos embutidos
- o painel admin usa token informado pelo operador e esse token nao deve ser commitado em arquivo algum
