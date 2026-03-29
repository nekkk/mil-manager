# Inclusao manual de jogos salvos

Use esta pasta para adicionar variantes de save manualmente, com prioridade maxima sobre a agregacao automatica.

## Formatos aceitos

### 1. ZIP com sidecar JSON

```text
catalog-source/
  manual-saves/
    01007EF00011E000/
      super-starter.zip
      super-starter.json
```

### 2. Pasta com metadata e payload

```text
catalog-source/
  manual-saves/
    01007EF00011E000/
      super-starter/
        metadata.json
        payload/
          SAVE.DAT
          progress.bin
```

Se a pasta nao tiver `payload/`, o gerador usa todos os arquivos dela, exceto `metadata.json` e `.gitkeep`.

## Metadata

Exemplo de `super-starter.json` ou `metadata.json`:

```json
{
  "name": "The Legend of Zelda: Breath of the Wild - Nintendo Switch 2 Edition",
  "label": "Super Starter",
  "category": "starter",
  "author": "M.I.L.",
  "language": "",
  "updatedAt": "2026-03-28"
}
```

Campos aceitos:

- `name`: nome do jogo
- `label`: nome da variante
- `category`: categoria da variante
- `author`: autoria exibida
- `language`: idioma associado, quando fizer sentido
- `updatedAt`: data ou carimbo textual da atualizacao

## Regras

1. A pasta do jogo deve usar o `titleId` com 16 caracteres hexadecimais.
2. Entradas manuais entram com prioridade maxima.
3. Se o payload manual for identico ao agregado, o indice final deduplica o conteudo e preserva `manual` como origem.
4. O delivery final continua saindo no mesmo formato sanitizado por hash.
