#!/usr/bin/env python3
import json
import os
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DIST_INDEX = ROOT / "dist" / "index.json"
DIST_CHEATS_INDEX = ROOT / "dist" / "cheats-index.json"
DIST_CHEATS_SUMMARY = ROOT / "dist" / "cheats-summary.json"
DIST_SAVES_INDEX = ROOT / "dist" / "saves-index.json"
DIST_CHEATS_DIR = ROOT / "dist" / "cheats"
DIST_SAVE_PACKS_DIR = ROOT / "dist" / "save-packs"
DIST_CHEATS_PACK = ROOT / "dist" / "cheats-pack.zip"
DIST_CHEATS_MANIFEST = ROOT / "dist" / "cheats-manifest.json"
DIST_THUMBS_DIR = ROOT / "dist" / "thumbs"
DIST_THUMBS_PACK = ROOT / "dist" / "thumbs-pack.zip"
DIST_THUMBS_MANIFEST = ROOT / "dist" / "thumbs-manifest.json"
SITE_DIR = ROOT / "site"
SITE_SRC_DIR = ROOT / "site-src"
SITE_INDEX_JSON = SITE_DIR / "index.json"
SITE_CHEATS_INDEX_JSON = SITE_DIR / "cheats-index.json"
SITE_CHEATS_SUMMARY_JSON = SITE_DIR / "cheats-summary.json"
SITE_SAVES_INDEX_JSON = SITE_DIR / "saves-index.json"
SITE_INDEX_HTML = SITE_DIR / "index.html"
SITE_NOJEKYLL = SITE_DIR / ".nojekyll"
SITE_CNAME = SITE_DIR / "CNAME"
SITE_CHEATS_DIR = SITE_DIR / "cheats"
SITE_SAVE_PACKS_DIR = SITE_DIR / "save-packs"
SITE_CHEATS_PACK = SITE_DIR / "cheats-pack.zip"
SITE_CHEATS_MANIFEST = SITE_DIR / "cheats-manifest.json"
SITE_THUMBS_DIR = SITE_DIR / "thumbs"
SITE_THUMBS_PACK = SITE_DIR / "thumbs-pack.zip"
SITE_THUMBS_MANIFEST = SITE_DIR / "thumbs-manifest.json"


def build_html(index_data: dict) -> str:
    catalog_name = index_data.get("catalogName", "MIL Traducoes")
    channel = index_data.get("channel", "stable")
    revision = index_data.get("catalogRevision", "")
    generated_at = index_data.get("generatedAt", "")
    total_entries = len(index_data.get("entries", []))

    return f"""<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{catalog_name} Catalog</title>
  <style>
    :root {{
      color-scheme: dark;
      --bg: #0b1220;
      --panel: #121b2d;
      --border: #2f486e;
      --text: #eef4ff;
      --muted: #9fb2d3;
      --accent: #7eb0ff;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      min-height: 100vh;
      font-family: "Segoe UI", system-ui, sans-serif;
      background: linear-gradient(180deg, #0a1020 0%, #121b2d 100%);
      color: var(--text);
      display: grid;
      place-items: center;
      padding: 24px;
    }}
    main {{
      width: min(720px, 100%);
      background: rgba(18, 27, 45, 0.92);
      border: 1px solid var(--border);
      padding: 28px;
      border-radius: 18px;
      box-shadow: 0 20px 60px rgba(0, 0, 0, 0.35);
    }}
    h1 {{
      margin: 0 0 10px;
      font-size: 1.9rem;
    }}
    p {{
      margin: 0 0 16px;
      color: var(--muted);
      line-height: 1.5;
    }}
    code {{
      color: var(--accent);
      font-family: Consolas, monospace;
      font-size: 0.95rem;
    }}
    ul {{
      margin: 18px 0 0;
      padding-left: 18px;
      color: var(--muted);
    }}
    li {{
      margin-bottom: 8px;
    }}
    .card {{
      background: rgba(10, 16, 32, 0.65);
      border: 1px solid rgba(126, 176, 255, 0.22);
      border-radius: 12px;
      padding: 16px;
      margin-top: 18px;
    }}
  </style>
</head>
<body>
  <main>
    <h1>{catalog_name}</h1>
    <p>Endpoint estavel do catalogo do aplicativo Gerenciador MIL Traducoes.</p>
    <div class="card">
      <p><strong>Arquivo JSON:</strong> <code>/index.json</code></p>
      <p><strong>Cheats JSON:</strong> <code>/cheats-index.json</code></p>
      <p><strong>Saves JSON:</strong> <code>/saves-index.json</code></p>
      <p><strong>Canal:</strong> {channel}</p>
      <p><strong>Revisao:</strong> {revision}</p>
      <p><strong>Gerado em:</strong> {generated_at}</p>
      <p><strong>Entradas:</strong> {total_entries}</p>
    </div>
    <ul>
      <li>Use este endpoint no app via <code>catalog_url=https://seu-endereco/index.json</code>.</li>
      <li>Os pacotes ZIP podem continuar hospedados no MEGA ou em outra origem.</li>
      <li>O conteudo deste site e gerado automaticamente a partir de <code>catalog-source/</code>.</li>
      <li>Painel administrativo: <code>/admin/</code></li>
    </ul>
  </main>
</body>
</html>
"""


def main() -> int:
    if not DIST_INDEX.exists():
        raise FileNotFoundError(f"Arquivo nao encontrado: {DIST_INDEX}")

    index_data = json.loads(DIST_INDEX.read_text(encoding="utf-8"))

    SITE_DIR.mkdir(parents=True, exist_ok=True)
    if SITE_SRC_DIR.exists():
        for source_path in SITE_SRC_DIR.rglob("*"):
            if source_path.is_dir():
                continue
            relative_path = source_path.relative_to(SITE_SRC_DIR)
            target_path = SITE_DIR / relative_path
            target_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_path, target_path)
    if DIST_THUMBS_DIR.exists():
        if SITE_THUMBS_DIR.exists():
            shutil.rmtree(SITE_THUMBS_DIR)
        shutil.copytree(DIST_THUMBS_DIR, SITE_THUMBS_DIR)
    if DIST_CHEATS_DIR.exists():
        if SITE_CHEATS_DIR.exists():
            shutil.rmtree(SITE_CHEATS_DIR)
        shutil.copytree(DIST_CHEATS_DIR, SITE_CHEATS_DIR)
    if DIST_SAVE_PACKS_DIR.exists():
        if SITE_SAVE_PACKS_DIR.exists():
            shutil.rmtree(SITE_SAVE_PACKS_DIR)
        shutil.copytree(DIST_SAVE_PACKS_DIR, SITE_SAVE_PACKS_DIR)
    if DIST_CHEATS_PACK.exists():
        shutil.copyfile(DIST_CHEATS_PACK, SITE_CHEATS_PACK)
    if DIST_CHEATS_MANIFEST.exists():
        shutil.copyfile(DIST_CHEATS_MANIFEST, SITE_CHEATS_MANIFEST)
    if DIST_THUMBS_PACK.exists():
        shutil.copyfile(DIST_THUMBS_PACK, SITE_THUMBS_PACK)
    if DIST_THUMBS_MANIFEST.exists():
        shutil.copyfile(DIST_THUMBS_MANIFEST, SITE_THUMBS_MANIFEST)
    SITE_INDEX_JSON.write_text(DIST_INDEX.read_text(encoding="utf-8"), encoding="utf-8")
    if DIST_CHEATS_INDEX.exists():
        SITE_CHEATS_INDEX_JSON.write_text(DIST_CHEATS_INDEX.read_text(encoding="utf-8"), encoding="utf-8")
    if DIST_CHEATS_SUMMARY.exists():
        SITE_CHEATS_SUMMARY_JSON.write_text(DIST_CHEATS_SUMMARY.read_text(encoding="utf-8"), encoding="utf-8")
    if DIST_SAVES_INDEX.exists():
        SITE_SAVES_INDEX_JSON.write_text(DIST_SAVES_INDEX.read_text(encoding="utf-8"), encoding="utf-8")
    SITE_INDEX_HTML.write_text(build_html(index_data), encoding="utf-8")
    SITE_NOJEKYLL.write_text("", encoding="utf-8")

    cname = os.environ.get("MIL_PAGES_CNAME", "").strip()
    if cname:
        SITE_CNAME.write_text(cname + "\n", encoding="utf-8")
    elif SITE_CNAME.exists():
        SITE_CNAME.unlink()

    print(f"Site preparado em: {SITE_DIR}")
    print(f"Catalogo publicado em: {SITE_INDEX_JSON}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
