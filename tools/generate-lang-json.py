#!/usr/bin/env python3
from __future__ import annotations

import ast
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_CPP = ROOT / "src" / "app.cpp"
LANG_DIR = ROOT / "romfs" / "lang"


STRING_LITERAL = r'(?:u8)?\"((?:[^\"\\\\]|\\\\.)*)\"'


def decode_cpp_string(value: str) -> str:
    return ast.literal_eval('"' + value + '"')


def normalize_language_key_part(text: str) -> str:
    normalized: list[str] = []
    last_underscore = False
    for ch in text:
        if ch.isascii() and (ch.islower() or ch.isdigit()):
            normalized.append(ch)
            last_underscore = False
            continue
        if ch.isascii() and ch.isupper():
            normalized.append(ch.lower())
            last_underscore = False
            continue
        if not last_underscore:
            normalized.append("_")
            last_underscore = True
    result = "".join(normalized).strip("_")
    return result or "text"


def build_auto_language_key(portuguese: str, english: str) -> str:
    hash_value = 2166136261
    combined = portuguese + "\x1f" + english
    for byte in combined.encode("utf-8"):
        hash_value ^= byte
        hash_value = (hash_value * 16777619) & 0xFFFFFFFF
    return f"auto.{normalize_language_key_part(english)}.{hash_value:08x}"


PORTUGUESE_FIXUPS = {
    " Variantes dispon?veis: ": " Variantes disponíveis: ",
    "Cat?logo sem title IDs para sondagem local.": "Catálogo sem title IDs para sondagem local.",
    "Escolher manualmente outro build?": "Escolher manualmente outro build?",
    "Compat?vel com a vers?o instalada: ": "Compatível com a versão instalada: ",
    "Jogo n?o encontrado no console/emulador.": "Jogo não encontrado no console/emulador.",
    "Jogo n?o encontrado no console/emulador. Variantes dispon?veis: ": "Jogo não encontrado no console/emulador. Variantes disponíveis: ",
    "Vers?o do jogo indispon?vel.": "Versão do jogo indisponível.",
    "Vers?o do jogo indispon?vel. Variantes dispon?veis: ": "Versão do jogo indisponível. Variantes disponíveis: ",
    "T?tulos instalados carregados por scan completo.": "Títulos instalados carregados por scan completo.",
    "Arquivo installed-titles-cache.json inv?lido.": "Arquivo installed-titles-cache.json inválido.",
    "nsInitialize falhou. Emulador ou servi?o NS indispon?vel.": "nsInitialize falhou. Emulador ou serviço NS indisponível.",
    "nsInitialize falhou. Servi?o NS indispon?vel.": "nsInitialize falhou. Serviço NS indisponível.",
    "Biblioteca do emulador n?o fica vis?vel ao homebrew. Sincronize os t?tulos para sdmc:/switch/mil_manager/cache/installed-titles-cache.json.": "Biblioteca do emulador não fica visível ao homebrew. Sincronize os títulos para sdmc:/switch/mil_manager/cache/installed-titles-cache.json.",
    "T?tulos detectados por sondagem do cat?logo.": "Títulos detectados por sondagem do catálogo.",
    "T?tulos importados de installed-titles-cache.json.": "Títulos importados de installed-titles-cache.json.",
    "Aten??o: pacote fora da faixa suportada para o jogo instalado (": "Atenção: pacote fora da faixa suportada para o jogo instalado (",
    "Trapa?a": "Trapaça",
    "Tradu??o": "Tradução",
    "T?tulos n?o identificados. Utilize a Pesquisa para localizar a trapa?a desejada.": "Títulos não identificados. Utilize a Pesquisa para localizar a trapaça desejada.",
    "Ainda n?o h? conte?do dispon?vel para esta se??o no cat?logo atual.": "Ainda não há conteúdo disponível para esta seção no catálogo atual.",
    "T?tulos n?o identificados. Utilize a Pesquisa para localizar o save desejado.": "Títulos não identificados. Utilize a Pesquisa para localizar o save desejado.",
    "Trapa?as": "Trapaças",
    "Tradu??es & Dublagens": "Traduções & Dublagens",
    "Relev?ncia": "Relevância",
    "Cache local do cat?logo": "Cache local do catálogo",
    "Cat?logo sincronizado do emulador": "Catálogo sincronizado do emulador",
    "Cat?logo local": "Catálogo local",
    "Cat?logo online": "Catálogo online",
}


def fix_portuguese_text(value: str) -> str:
    return PORTUGUESE_FIXUPS.get(value, value)


def set_nested_value(target: dict, dotted_key: str, value: str) -> None:
    parts = dotted_key.split(".")
    cursor = target
    for part in parts[:-1]:
        cursor = cursor.setdefault(part, {})
    cursor[parts[-1]] = value


def flatten_nested_value(value: object, prefix: str, output: dict[str, str]) -> None:
    if isinstance(value, str):
        output[prefix] = value
        return
    if not isinstance(value, dict):
        return
    for key, child in value.items():
        child_key = f"{prefix}.{key}" if prefix else key
        flatten_nested_value(child, child_key, output)


def load_existing_values(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    raw = json.loads(path.read_text(encoding="utf-8"))
    flattened: dict[str, str] = {}
    flatten_nested_value(raw, "", flattened)
    return flattened


def collect_strings() -> tuple[dict[str, str], dict[str, str]]:
    text = APP_CPP.read_text(encoding="utf-8")

    pt_values: dict[str, str] = {"ui.missing_key": "[[chave ausente]]"}
    en_values: dict[str, str] = {"ui.missing_key": "[[missing key]]"}

    ui_string_pattern = re.compile(
        rf'UiString\(\s*state\s*,\s*"([^"]+)"\s*,\s*{STRING_LITERAL}\s*,\s*{STRING_LITERAL}\s*\)',
        re.DOTALL,
    )
    ui_text_pattern = re.compile(
        rf'UiText\(\s*state\s*,\s*{STRING_LITERAL}\s*,\s*{STRING_LITERAL}\s*\)',
        re.DOTALL,
    )

    for key, pt_raw, en_raw in ui_string_pattern.findall(text):
        pt = fix_portuguese_text(decode_cpp_string(pt_raw))
        en = decode_cpp_string(en_raw)
        existing_pt = pt_values.get(key)
        existing_en = en_values.get(key)
        if existing_pt is not None and existing_pt != pt:
            raise ValueError(f"Conflicting PT value for key {key!r}")
        if existing_en is not None and existing_en != en:
            raise ValueError(f"Conflicting EN value for key {key!r}")
        pt_values[key] = pt
        en_values[key] = en

    for pt_raw, en_raw in ui_text_pattern.findall(text):
        pt = fix_portuguese_text(decode_cpp_string(pt_raw))
        en = decode_cpp_string(en_raw)
        key = build_auto_language_key(pt, en)
        existing_pt = pt_values.get(key)
        existing_en = en_values.get(key)
        if existing_pt is not None and existing_pt != pt:
            raise ValueError(f"Conflicting PT value for auto key {key!r}")
        if existing_en is not None and existing_en != en:
            raise ValueError(f"Conflicting EN value for auto key {key!r}")
        pt_values[key] = pt
        en_values[key] = en

    return pt_values, en_values


def write_language_file(path: Path, values: dict[str, str]) -> None:
    existing_values = load_existing_values(path)
    nested: dict[str, object] = {}
    for key in sorted(values):
        set_nested_value(nested, key, existing_values.get(key, values[key]))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(nested, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    pt_values, en_values = collect_strings()
    write_language_file(LANG_DIR / "pt-BR.json", pt_values)
    write_language_file(LANG_DIR / "en-US.json", en_values)
    print(f"Generated {len(pt_values)} PT-BR strings and {len(en_values)} EN-US strings.")


if __name__ == "__main__":
    main()
