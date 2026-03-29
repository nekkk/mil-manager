# App Structure

## Modulos principais

- [main.cpp](C:/Users/lordd/source/codex/mil-manager/src/main.cpp)
  - bootstrap do homebrew
- [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp)
  - loop principal, UI, navegacao, integracao entre modulos
- [catalog.cpp](C:/Users/lordd/source/codex/mil-manager/src/catalog.cpp)
  - leitura do catalogo principal
- [cheats.cpp](C:/Users/lordd/source/codex/mil-manager/src/cheats.cpp)
  - parsing e suporte de indices de trapacas
- [savegames.cpp](C:/Users/lordd/source/codex/mil-manager/src/savegames.cpp)
  - parsing e suporte de indices de saves
- [installer.cpp](C:/Users/lordd/source/codex/mil-manager/src/installer.cpp)
  - download, extracao, copia, backups e remocao
- [http.cpp](C:/Users/lordd/source/codex/mil-manager/src/http.cpp)
  - downloads HTTP e callbacks de progresso
- [platform_switch.cpp](C:/Users/lordd/source/codex/mil-manager/src/platform_switch.cpp)
  - deteccao de ambiente, leitura local e integracao com servicos do Switch
- [graphics.cpp](C:/Users/lordd/source/codex/mil-manager/src/graphics.cpp)
  - desenho e suporte visual
- [config.cpp](C:/Users/lordd/source/codex/mil-manager/src/config.cpp)
  - leitura e persistencia de configuracao

## Headers publicos internos

- [app.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/app.hpp)
- [catalog.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/catalog.hpp)
- [cheats.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/cheats.hpp)
- [config.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/config.hpp)
- [graphics.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/graphics.hpp)
- [http.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/http.hpp)
- [installer.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/installer.hpp)
- [models.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/models.hpp)
- [platform.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/platform.hpp)
- [savegames.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/savegames.hpp)

## Estrutura de dados

- `CatalogIndex`
  - entradas de traducoes, mods e metadados gerais
- `CheatsIndex`
  - indice agregado de trapacas
- `SavesIndex`
  - indice agregado de saves
- `InstallReceipt`
  - recibos de instalacao/remocao

## Fluxos principais

### Catalogo

1. carregar `index.json`
2. carregar catalogos derivados quando necessario
3. combinar com biblioteca detectada
4. montar lista visivel e detalhes

### Instalacao

1. resolver item/variante/build
2. baixar arquivo do delivery
3. validar e preparar staging
4. aplicar no destino correto
5. gravar recibo

### Remocao

1. localizar recibo
2. remover ou restaurar conforme o tipo
3. atualizar cache e UI
