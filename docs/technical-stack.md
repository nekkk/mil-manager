# Technical Stack

## Linguagem e toolchain

- C++17
- CMake
- devkitPro Switch toolchain

## Bibliotecas de runtime

Pelo [CMakeLists.txt](C:/Users/lordd/source/codex/mil-manager/CMakeLists.txt), o app usa:

- `libnx`
- `curl`
- `libarchive`
- `bz2`
- `lz4`
- `lzma`
- `mbedtls`
- `mbedcrypto`
- `mbedx509`
- `z`
- `zstd`

## Bibliotecas vendorizadas

- `external/font8x8`
- `external/stb`
- `external/picojson`

## Papel de cada dependencia

- `libnx`
  - homebrew runtime, input, FS, servicos e integracao com Switch
- `curl`
  - downloads HTTP/HTTPS
- `libarchive` + libs de compressao
  - leitura e extracao de zip e outros formatos suportados
- `mbedtls`
  - base TLS usada pelo stack de rede da build
- `stb`
  - utilitarios de imagem
- `font8x8`
  - glyphs/fontes auxiliares
- `picojson`
  - parse e geracao leve de JSON

## Python tooling

As tools de catalogo usam Python 3 e dependencias em:

- [requirements-catalog.txt](C:/Users/lordd/source/codex/mil-manager/tools/requirements-catalog.txt)

Uso principal:

- gerar `index.json`
- gerar `cheats-index.json` e `cheats-summary.json`
- gerar `saves-index.json` e `saves-summary.json`
- preparar `site/`
- sincronizar bibliotecas de emuladores

## Internacionalizacao

As strings da UI vivem em:

- [pt-BR.json](C:/Users/lordd/source/codex/mil-manager/romfs/lang/pt-BR.json)
- [en-US.json](C:/Users/lordd/source/codex/mil-manager/romfs/lang/en-US.json)

Novas strings devem entrar no `lang`, e nao como literal visivel no codigo.
