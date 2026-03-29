# Inclusao manual de trapaças

Use esta pasta para adicionar ou sobrescrever trapaças manualmente, sem depender das fontes agregadas.

## Estrutura

```text
catalog-source/
  manual-cheats/
    0100AAAA0000BBBB/
      1122334455667788.txt
      1122334455667788.json
```

## Regras

1. A pasta deve usar o `titleId` em hexadecimal com 16 caracteres.
2. O arquivo `.txt` deve usar o `buildId` em hexadecimal com 16 caracteres.
3. O arquivo `.json` ao lado do `.txt` e opcional.
4. Entradas manuais entram com prioridade maxima e prevalecem sobre as fontes agregadas quando houver empate funcional.

## Sidecar opcional

Exemplo de `1122334455667788.json`:

```json
{
  "title": "General",
  "categories": ["manual", "community"]
}
```

Campos aceitos:

- `title`: titulo exibido para a entrada manual
- `categories`: lista de categorias para a entrada

## Observacoes

- Se o conteudo manual for identico ao agregado, o indice final deduplica o payload e registra a origem `manual` junto das demais.
- Se o `titleId` nao existir no catalogo principal, o gerador tenta resolver o nome pelo TitleDB; se nao conseguir, usa o proprio `titleId`.
