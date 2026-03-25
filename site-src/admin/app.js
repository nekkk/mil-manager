(function () {
  const STORAGE_KEY = "mil-catalog-admin-config";
  const API_ROOT = "https://api.github.com";
  const METADATA_PATH = "catalog-source/catalog-metadata.json";
  const ENTRIES_PATH = "catalog-source/entries";
  const GENERATED_INDEX_PATH = "dist/index.json";

  const state = {
    config: {
      owner: "",
      repo: "",
      branch: "main",
      token: "",
    },
    metadata: {
      catalogName: "MIL Traducoes",
      channel: "stable",
      schemaVersion: "1.0",
      catalogRevision: "",
      defaults: {
        author: "M.I.L.",
        detailsUrl: "https://miltraducoes.com/",
        language: "pt-BR",
      },
    },
    entries: [],
    fileShas: new Map(),
    fileContents: new Map(),
    originalPaths: new Map(),
    derivedEntries: new Map(),
    deletedPaths: new Set(),
    selectedId: null,
    selectedVariantId: null,
    isLoaded: false,
  };

  const els = {
    owner: document.getElementById("owner"),
    repo: document.getElementById("repo"),
    branch: document.getElementById("branch"),
    token: document.getElementById("token"),
    loadRepo: document.getElementById("loadRepo"),
    clearSession: document.getElementById("clearSession"),
    catalogName: document.getElementById("catalogName"),
    channel: document.getElementById("channel"),
    schemaVersion: document.getElementById("schemaVersion"),
    catalogRevision: document.getElementById("catalogRevision"),
    defaultAuthor: document.getElementById("defaultAuthor"),
    defaultLanguage: document.getElementById("defaultLanguage"),
    defaultDetailsUrl: document.getElementById("defaultDetailsUrl"),
    status: document.getElementById("status"),
    search: document.getElementById("search"),
    entryList: document.getElementById("entryList"),
    newEntry: document.getElementById("newEntry"),
    duplicateEntry: document.getElementById("duplicateEntry"),
    saveEntry: document.getElementById("saveEntry"),
    deleteEntry: document.getElementById("deleteEntry"),
    publishAll: document.getElementById("publishAll"),
    entryId: document.getElementById("entryId"),
    section: document.getElementById("section"),
    titleId: document.getElementById("titleId"),
    contentTypeInputs: Array.from(document.querySelectorAll('input[name="contentType"]')),
    name: document.getElementById("name"),
    introPtBr: document.getElementById("introPtBr"),
    introEnUs: document.getElementById("introEnUs"),
    summaryPtBr: document.getElementById("summaryPtBr"),
    summaryEnUs: document.getElementById("summaryEnUs"),
    author: document.getElementById("author"),
    packageVersion: document.getElementById("packageVersion"),
    contentRevision: document.getElementById("contentRevision"),
    downloadUrl: document.getElementById("downloadUrl"),
    detailsUrl: document.getElementById("detailsUrl"),
    coverUrl: document.getElementById("coverUrl"),
    thumbnailUrl: document.getElementById("thumbnailUrl"),
    tags: document.getElementById("tags"),
    minGameVersion: document.getElementById("minGameVersion"),
    maxGameVersion: document.getElementById("maxGameVersion"),
    exactGameVersions: document.getElementById("exactGameVersions"),
    variantList: document.getElementById("variantList"),
    newVariant: document.getElementById("newVariant"),
    duplicateVariant: document.getElementById("duplicateVariant"),
    deleteVariant: document.getElementById("deleteVariant"),
    variantId: document.getElementById("variantId"),
    variantLabel: document.getElementById("variantLabel"),
    variantPackageVersion: document.getElementById("variantPackageVersion"),
    variantContentRevision: document.getElementById("variantContentRevision"),
    variantDownloadUrl: document.getElementById("variantDownloadUrl"),
    variantMinGameVersion: document.getElementById("variantMinGameVersion"),
    variantMaxGameVersion: document.getElementById("variantMaxGameVersion"),
    variantExactGameVersions: document.getElementById("variantExactGameVersions"),
    featured: document.getElementById("featured"),
    entryMeta: document.getElementById("entryMeta"),
  };

  function setStatus(message, tone = "") {
    els.status.textContent = message || "";
    els.status.className = `status${tone ? ` ${tone}` : ""}`;
  }

  function loadConfigFromStorage() {
    try {
      const raw = localStorage.getItem(STORAGE_KEY);
      if (!raw) {
        return;
      }
      const parsed = JSON.parse(raw);
      state.config.owner = parsed.owner || "";
      state.config.repo = parsed.repo || "";
      state.config.branch = parsed.branch || "main";
      state.config.token = parsed.token || "";
    } catch (error) {
      console.warn(error);
    }
  }

  function saveConfigToStorage() {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(state.config));
  }

  function syncConfigFromInputs() {
    state.config.owner = els.owner.value.trim();
    state.config.repo = els.repo.value.trim();
    state.config.branch = els.branch.value.trim() || "main";
    state.config.token = els.token.value.trim();
  }

  function syncInputsFromConfig() {
    els.owner.value = state.config.owner;
    els.repo.value = state.config.repo;
    els.branch.value = state.config.branch;
    els.token.value = state.config.token;
  }

  function repoApi(path) {
    const owner = encodeURIComponent(state.config.owner);
    const repo = encodeURIComponent(state.config.repo);
    const branch = encodeURIComponent(state.config.branch);
    return `${API_ROOT}/repos/${owner}/${repo}/contents/${path}?ref=${branch}`;
  }

  function repoGitApi(path) {
    const owner = encodeURIComponent(state.config.owner);
    const repo = encodeURIComponent(state.config.repo);
    return `${API_ROOT}/repos/${owner}/${repo}/git/${path}`;
  }

  async function githubFetch(url, options = {}) {
    const headers = new Headers(options.headers || {});
    headers.set("Accept", "application/vnd.github+json");
    headers.set("Authorization", `Bearer ${state.config.token}`);
    headers.set("X-GitHub-Api-Version", "2022-11-28");

    const response = await fetch(url, { ...options, headers });
    if (!response.ok) {
      const text = await response.text();
      throw new Error(`GitHub API ${response.status}: ${text || response.statusText}`);
    }
    return response;
  }

  async function fetchBranchHead() {
    const response = await githubFetch(repoGitApi(`ref/heads/${encodeURIComponent(state.config.branch)}`));
    return response.json();
  }

  async function fetchCommitObject(sha) {
    const response = await githubFetch(repoGitApi(`commits/${sha}`));
    return response.json();
  }

  async function createBlobFromText(text) {
    const response = await githubFetch(repoGitApi("blobs"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        content: text,
        encoding: "utf-8",
      }),
    });
    return response.json();
  }

  async function createTree(baseTreeSha, entries) {
    const response = await githubFetch(repoGitApi("trees"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        base_tree: baseTreeSha,
        tree: entries,
      }),
    });
    return response.json();
  }

  async function createCommit(message, treeSha, parentSha) {
    const response = await githubFetch(repoGitApi("commits"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        message,
        tree: treeSha,
        parents: [parentSha],
      }),
    });
    return response.json();
  }

  async function updateBranchHead(commitSha) {
    const response = await githubFetch(repoGitApi(`refs/heads/${encodeURIComponent(state.config.branch)}`), {
      method: "PATCH",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        sha: commitSha,
        force: false,
      }),
    });
    return response.json();
  }

  function decodeContent(base64Value) {
    const normalized = (base64Value || "").replace(/\n/g, "");
    const binary = atob(normalized);
    const bytes = Uint8Array.from(binary, (char) => char.charCodeAt(0));
    return new TextDecoder().decode(bytes);
  }

  function encodeContent(text) {
    const bytes = new TextEncoder().encode(text);
    let binary = "";
    bytes.forEach((byte) => {
      binary += String.fromCharCode(byte);
    });
    return btoa(binary);
  }

  function serializeJson(value) {
    return JSON.stringify(value, null, 2) + "\n";
  }

  async function fetchJsonFile(path) {
    const response = await githubFetch(repoApi(path));
    const payload = await response.json();
    const content = JSON.parse(decodeContent(payload.content));
    state.fileShas.set(path, payload.sha);
    state.fileContents.set(path, serializeJson(content));
    return { content, sha: payload.sha, path };
  }

  async function fetchEntries() {
    const response = await githubFetch(repoApi(ENTRIES_PATH));
    const payload = await response.json();
    const files = payload.filter((item) => item.type === "file" && item.name.endsWith(".json"));
    const result = [];

    for (const file of files) {
      const fetched = await fetchJsonFile(file.path);
      result.push({ content: fetched.content, path: fetched.path, sha: fetched.sha });
    }

    result.sort((a, b) => (a.content.name || "").localeCompare(b.content.name || "", "pt-BR"));
    return result;
  }

  async function fetchGeneratedIndex() {
    try {
      const fetched = await fetchJsonFile(GENERATED_INDEX_PATH);
      const entries = Array.isArray(fetched.content?.entries) ? fetched.content.entries : [];
      return entries;
    } catch (error) {
      console.warn("Nao foi possivel carregar o indice gerado:", error);
      return [];
    }
  }

  function slugify(value) {
    return value
      .normalize("NFD")
      .replace(/[\u0300-\u036f]/g, "")
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "")
      .replace(/-{2,}/g, "-");
  }

  function splitList(value) {
    return value
      .split(",")
      .map((item) => item.trim())
      .filter(Boolean);
  }

  function buildCompatibilityObject(minVersion, maxVersion, exactVersionsRaw) {
    const compatibility = {};
    const exactVersions = Array.isArray(exactVersionsRaw) ? exactVersionsRaw : splitList(exactVersionsRaw || "");
    if ((minVersion || "").trim()) {
      compatibility.minGameVersion = minVersion.trim();
    }
    if ((maxVersion || "").trim()) {
      compatibility.maxGameVersion = maxVersion.trim();
    }
    if (exactVersions.length) {
      compatibility.exactGameVersions = exactVersions;
    }
    return compatibility;
  }

  function normalizeContentType(value) {
    const normalized = String(value || "").trim().toLowerCase();
    const aliases = {
      traducao: "translation",
      "tradução": "translation",
      translation: "translation",
      dublagem: "dub",
      dub: "dub",
      mod: "mod",
      mods: "mod",
      cheat: "cheat",
      cheats: "cheat",
    };
    return aliases[normalized] || normalized;
  }

  function defaultContentTypesForSection(section) {
    switch ((section || "").toLowerCase()) {
      case "translations":
        return ["translation"];
      case "mods":
        return ["mod"];
      case "cheats":
        return ["cheat"];
      default:
        return [];
    }
  }

  function findDerivedEntryByTitleId(titleId) {
    const normalized = String(titleId || "").trim().toUpperCase();
    if (!normalized) {
      return null;
    }
    for (const entry of state.derivedEntries.values()) {
      if (String(entry.titleId || "").trim().toUpperCase() === normalized) {
        return entry;
      }
    }
    return null;
  }

  function resolveDisplayName(entryLike) {
    const derived = findDerivedEntryByTitleId(entryLike?.titleId);
    const derivedName = String(derived?.name || "").trim();
    if (derivedName) {
      return derivedName;
    }
    return String(entryLike?.name || "").trim();
  }

  function syncContentTypeInputs(values) {
    const selected = new Set((values || []).map((item) => normalizeContentType(item)));
    els.contentTypeInputs.forEach((input) => {
      input.checked = selected.has(input.value);
    });
  }

  function readContentTypesFromForm() {
    const values = els.contentTypeInputs
      .filter((input) => input.checked)
      .map((input) => normalizeContentType(input.value));
    return values.length ? values : defaultContentTypesForSection(els.section.value.trim());
  }

  function applyDerivedNameFromTitleId(force = false) {
    const derived = findDerivedEntryByTitleId(els.titleId.value);
    if (!derived || !derived.name) {
      return;
    }
    if (force || !els.name.value.trim()) {
      els.name.value = derived.name;
    }
  }

  function buildEntryFromForm() {
    const compatibility = buildCompatibilityObject(
      els.minGameVersion.value,
      els.maxGameVersion.value,
      els.exactGameVersions.value
    );
    const currentEntry = findSelectedEntry();
    const variants = currentEntry && Array.isArray(currentEntry.variants) ? structuredClone(currentEntry.variants) : [];

    const entry = {
      id: els.entryId.value.trim(),
      section: els.section.value.trim(),
      titleId: els.titleId.value.trim().toUpperCase(),
      name: els.name.value.trim() || String(findDerivedEntryByTitleId(els.titleId.value)?.name || "").trim(),
      introPtBr: els.introPtBr.value.trim(),
      introEnUs: els.introEnUs.value.trim(),
      summary: els.summaryPtBr.value.trim(),
      summaryPtBr: els.summaryPtBr.value.trim(),
      summaryEnUs: els.summaryEnUs.value.trim(),
      author: els.author.value.trim(),
      packageVersion: els.packageVersion.value.trim(),
      contentRevision: els.contentRevision.value.trim(),
      contentTypes: readContentTypesFromForm(),
      downloadUrl: els.downloadUrl.value.trim(),
      detailsUrl: els.detailsUrl.value.trim(),
      coverUrl: els.coverUrl.value.trim(),
      thumbnailUrl: els.thumbnailUrl.value.trim(),
      tags: splitList(els.tags.value),
      compatibility,
      variants,
      featured: !!els.featured.checked,
    };

    Object.keys(entry).forEach((key) => {
      if (entry[key] === "" || (Array.isArray(entry[key]) && entry[key].length === 0)) {
        delete entry[key];
      }
    });
    if (Object.keys(compatibility).length === 0) {
      delete entry.compatibility;
    }
    if (!entry.downloadUrl && entry.variants && entry.variants.length > 0) {
      delete entry.downloadUrl;
    }
    return entry;
  }

  function findSelectedEntry() {
    return state.entries.find((entry) => entry.id === state.selectedId) || null;
  }

  function ensureVariantsArray(entry) {
    if (!Array.isArray(entry.variants)) {
      entry.variants = [];
    }
    return entry.variants;
  }

  function findSelectedVariant(entry) {
    if (!entry || !Array.isArray(entry.variants)) {
      return null;
    }
    return entry.variants.find((variant) => variant.id === state.selectedVariantId) || null;
  }

  function entryPath(entry) {
    return `${ENTRIES_PATH}/${entry.id}.json`;
  }

  function renderEntryMeta(entry, originalPath) {
    els.entryMeta.innerHTML = "";
    const pills = [
      `arquivo: ${originalPath || entryPath(entry)}`,
      `titleId: ${entry.titleId || "sem titleId"}`,
      entry.featured ? "destaque" : "normal",
    ];
    for (const label of pills) {
      const span = document.createElement("span");
      span.className = "pill";
      span.textContent = label;
      els.entryMeta.appendChild(span);
    }
  }

  function fillVariantForm(entry, variant) {
    if (!entry || !variant) {
      [
        "variantId",
        "variantLabel",
        "variantPackageVersion",
        "variantContentRevision",
        "variantDownloadUrl",
        "variantMinGameVersion",
        "variantMaxGameVersion",
        "variantExactGameVersions",
      ].forEach((key) => {
        els[key].value = "";
      });
      return;
    }

    els.variantId.value = variant.id || "";
    els.variantLabel.value = variant.label || "";
    els.variantPackageVersion.value = variant.packageVersion || "";
    els.variantContentRevision.value = variant.contentRevision || "";
    els.variantDownloadUrl.value = variant.downloadUrl || "";
    els.variantMinGameVersion.value = variant.compatibility?.minGameVersion || "";
    els.variantMaxGameVersion.value = variant.compatibility?.maxGameVersion || "";
    els.variantExactGameVersions.value = (variant.compatibility?.exactGameVersions || []).join(", ");
  }

  function renderVariantList() {
    els.variantList.innerHTML = "";
    const entry = findSelectedEntry();
    if (!entry) {
      return;
    }
    const variants = ensureVariantsArray(entry);
    for (const variant of variants) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `entry-item${variant.id === state.selectedVariantId ? " active" : ""}`;
      button.innerHTML = `
        <div class="title">${escapeHtml(variant.label || variant.id || "(sem label)")}</div>
        <div class="meta">${escapeHtml((variant.compatibility?.exactGameVersions || []).join(", ") || variant.compatibility?.minGameVersion || "sem versao")}</div>
      `;
      button.addEventListener("click", () => {
        try {
          saveCurrentVariant(false);
          state.selectedVariantId = variant.id;
          fillVariantForm(entry, variant);
          renderVariantList();
        } catch (error) {
          console.error(error);
          setStatus(error.message, "error");
        }
      });
      els.variantList.appendChild(button);
    }
  }

  function buildVariantFromForm() {
    return {
      id: els.variantId.value.trim(),
      label: els.variantLabel.value.trim(),
      packageVersion: els.variantPackageVersion.value.trim(),
      contentRevision: els.variantContentRevision.value.trim(),
      downloadUrl: els.variantDownloadUrl.value.trim(),
      compatibility: buildCompatibilityObject(
        els.variantMinGameVersion.value,
        els.variantMaxGameVersion.value,
        els.variantExactGameVersions.value
      ),
    };
  }

  function validateVariant(variant) {
    const missing = [];
    ["id", "downloadUrl"].forEach((field) => {
      if (!variant[field]) {
        missing.push(field);
      }
    });
    if (missing.length) {
      throw new Error(`Variant incompleta: faltam ${missing.join(", ")}`);
    }
  }

  function saveCurrentVariant(showStatus = true) {
    const entry = findSelectedEntry();
    const current = findSelectedVariant(entry);
    if (!entry || !current) {
      return;
    }

    const updated = buildVariantFromForm();
    validateVariant(updated);

    const variants = ensureVariantsArray(entry);
    const duplicate = variants.find((variant) => variant.id === updated.id && variant.id !== state.selectedVariantId);
    if (duplicate) {
      throw new Error(`Ja existe uma variant com id '${updated.id}'.`);
    }

    const index = variants.findIndex((variant) => variant.id === state.selectedVariantId);
    variants[index] = updated;
    state.selectedVariantId = updated.id;
    fillVariantForm(entry, updated);
    renderVariantList();
    if (showStatus) {
      setStatus(`Variant '${updated.id}' atualizada localmente.`, "ok");
    }
  }

  function createEmptyVariant() {
    return {
      id: "game-version",
      label: "",
      packageVersion: "",
      contentRevision: "",
      downloadUrl: "",
      compatibility: {},
    };
  }

  function addVariant(duplicate = false) {
    const entry = findSelectedEntry();
    if (!entry) {
      return;
    }
    const variants = ensureVariantsArray(entry);
    const source = duplicate ? findSelectedVariant(entry) : null;
    const variant = source ? structuredClone(source) : createEmptyVariant();
    const baseId = (variant.id || "game-version").trim() || "game-version";
    let candidate = duplicate ? `${baseId}-copy` : baseId;
    let suffix = 2;
    while (variants.some((item) => item.id === candidate)) {
      candidate = `${baseId}-${suffix++}`;
    }
    variant.id = candidate;
    if (duplicate && variant.label) {
      variant.label = `${variant.label} (copy)`;
    }
    variants.push(variant);
    state.selectedVariantId = variant.id;
    fillVariantForm(entry, variant);
    renderVariantList();
    setStatus(`Variant pronta para edicao: '${variant.id}'.`, "ok");
  }

  function deleteSelectedVariant() {
    const entry = findSelectedEntry();
    const variant = findSelectedVariant(entry);
    if (!entry || !variant) {
      return;
    }
    const confirmed = window.confirm(`Remover a variant '${variant.label || variant.id}'?`);
    if (!confirmed) {
      return;
    }
    entry.variants = ensureVariantsArray(entry).filter((item) => item.id !== variant.id);
    state.selectedVariantId = entry.variants[0]?.id || null;
    fillVariantForm(entry, findSelectedVariant(entry));
    renderVariantList();
    setStatus(`Variant '${variant.id}' removida localmente.`, "ok");
  }

  function fillEntryForm(entry) {
    if (!entry) {
      [
        "entryId",
        "titleId",
        "name",
        "introPtBr",
        "introEnUs",
        "summaryPtBr",
        "summaryEnUs",
        "author",
        "packageVersion",
        "contentRevision",
        "downloadUrl",
        "detailsUrl",
        "coverUrl",
        "thumbnailUrl",
        "tags",
        "minGameVersion",
        "maxGameVersion",
        "exactGameVersions",
      ].forEach((key) => {
        els[key].value = "";
      });
      syncContentTypeInputs(defaultContentTypesForSection("translations"));
      els.section.value = "translations";
      els.featured.checked = false;
      els.entryMeta.innerHTML = "";
      state.selectedVariantId = null;
      fillVariantForm(null, null);
      renderVariantList();
      return;
    }

    const derived = state.derivedEntries.get(entry.id) || {};
    const displayName = resolveDisplayName(entry) || String(derived.name || "").trim();

    els.entryId.value = entry.id || "";
    els.section.value = entry.section || "translations";
    els.titleId.value = entry.titleId || "";
    syncContentTypeInputs((entry.contentTypes && entry.contentTypes.length ? entry.contentTypes : defaultContentTypesForSection(entry.section || "translations")));
    els.name.value = displayName || "";
    els.introPtBr.value = entry.introPtBr || entry.intro || derived.introPtBr || derived.intro || "";
    els.introEnUs.value = entry.introEnUs || derived.introEnUs || "";
    els.summaryPtBr.value = entry.summaryPtBr || entry.summary || derived.summaryPtBr || derived.summary || "";
    els.summaryEnUs.value = entry.summaryEnUs || derived.summaryEnUs || "";
    els.author.value = entry.author || "";
    els.packageVersion.value = entry.packageVersion || "";
    els.contentRevision.value = entry.contentRevision || "";
    els.downloadUrl.value = entry.downloadUrl || "";
    els.detailsUrl.value = entry.detailsUrl || "";
    els.coverUrl.value = entry.coverUrl || derived.coverUrl || "";
    els.thumbnailUrl.value = entry.thumbnailUrl || derived.thumbnailUrl || derived.iconUrl || "";
    els.tags.value = (entry.tags || []).join(", ");
    els.minGameVersion.value = entry.compatibility?.minGameVersion || "";
    els.maxGameVersion.value = entry.compatibility?.maxGameVersion || "";
    els.exactGameVersions.value = (entry.compatibility?.exactGameVersions || []).join(", ");
    els.featured.checked = !!entry.featured;
    state.selectedVariantId = ensureVariantsArray(entry)[0]?.id || null;
    fillVariantForm(entry, findSelectedVariant(entry));
    renderVariantList();
    renderEntryMeta(entry, state.originalPaths.get(entry.id));
  }

  function renderEntryList() {
    const term = els.search.value.trim().toLowerCase();
    els.entryList.innerHTML = "";

    const filtered = state.entries.filter((entry) => {
      if (!term) {
        return true;
      }
      return [entry.id, resolveDisplayName(entry), entry.titleId]
        .filter(Boolean)
        .some((value) => value.toLowerCase().includes(term));
    });

    for (const entry of filtered) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `entry-item${entry.id === state.selectedId ? " active" : ""}`;
      button.innerHTML = `
        <div class="title">${escapeHtml(resolveDisplayName(entry) || entry.id || "(sem nome)")}</div>
        <div class="meta">${escapeHtml(entry.section || "")} • ${escapeHtml(entry.titleId || "")}</div>
      `;
      button.addEventListener("click", () => {
        try {
          saveCurrentVariant(false);
          saveCurrentEntry(false);
          state.selectedId = entry.id;
          fillEntryForm(entry);
          renderEntryList();
        } catch (error) {
          console.error(error);
          setStatus(error.message, "error");
        }
      });
      els.entryList.appendChild(button);
    }
  }

  function escapeHtml(value) {
    return String(value)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/\"/g, "&quot;");
  }

  function syncMetadataFromInputs() {
    state.metadata.catalogName = els.catalogName.value.trim();
    state.metadata.channel = els.channel.value.trim();
    state.metadata.schemaVersion = els.schemaVersion.value.trim();
    state.metadata.catalogRevision = els.catalogRevision.value.trim();
    state.metadata.defaults = {
      author: els.defaultAuthor.value.trim(),
      language: els.defaultLanguage.value.trim(),
      detailsUrl: els.defaultDetailsUrl.value.trim(),
    };
  }

  function syncInputsFromMetadata() {
    els.catalogName.value = state.metadata.catalogName || "";
    els.channel.value = state.metadata.channel || "";
    els.schemaVersion.value = state.metadata.schemaVersion || "";
    els.catalogRevision.value = state.metadata.catalogRevision || "";
    els.defaultAuthor.value = state.metadata.defaults?.author || "";
    els.defaultLanguage.value = state.metadata.defaults?.language || "";
    els.defaultDetailsUrl.value = state.metadata.defaults?.detailsUrl || "";
  }

  function validateEntry(entry) {
    const missing = [];
    ["id", "section", "titleId", "name"].forEach((field) => {
      if (!entry[field]) {
        missing.push(field);
      }
    });
    if (!entry.downloadUrl && (!Array.isArray(entry.variants) || entry.variants.length === 0)) {
      missing.push("downloadUrl ou variants");
    }
    if (missing.length) {
      throw new Error(`Entrada incompleta: faltam ${missing.join(", ")}`);
    }
  }

  function saveCurrentEntry(showStatus = true) {
    const current = findSelectedEntry();
    if (!current) {
      return;
    }
    saveCurrentVariant(false);

    const updated = buildEntryFromForm();
    validateEntry(updated);

    const newId = updated.id;
    const existingOther = state.entries.find((entry) => entry.id === newId && entry.id !== state.selectedId);
    if (existingOther) {
      throw new Error(`Ja existe uma entrada com id '${newId}'.`);
    }

    const oldPath = state.originalPaths.get(state.selectedId);
    if (oldPath && state.selectedId !== newId) {
      state.deletedPaths.add(oldPath);
      state.originalPaths.delete(state.selectedId);
      state.originalPaths.set(newId, oldPath);
    }

    const index = state.entries.findIndex((entry) => entry.id === state.selectedId);
    state.entries[index] = updated;
    state.selectedId = updated.id;
    fillEntryForm(updated);
    renderEntryList();

    if (showStatus) {
      setStatus(`Alteracoes locais aplicadas em '${updated.id}'.`, "ok");
    }
  }

  function buildMetadataPayload() {
    syncMetadataFromInputs();
    return {
      catalogName: state.metadata.catalogName,
      channel: state.metadata.channel,
      schemaVersion: state.metadata.schemaVersion,
      catalogRevision: state.metadata.catalogRevision,
      defaults: {
        author: state.metadata.defaults?.author || "M.I.L.",
        detailsUrl: state.metadata.defaults?.detailsUrl || "https://miltraducoes.com/",
        language: state.metadata.defaults?.language || "pt-BR",
      },
    };
  }

  async function putFile(path, contentObject, message) {
    const serialized = serializeJson(contentObject);
    if (state.fileContents.get(path) === serialized) {
      return { skipped: true };
    }

    const body = {
      message,
      branch: state.config.branch,
      content: encodeContent(serialized),
    };
    const existingSha = state.fileShas.get(path);
    if (existingSha) {
      body.sha = existingSha;
    }
    const response = await githubFetch(repoApi(path), {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    const payload = await response.json();
    state.fileShas.set(path, payload.content.sha);
    state.fileContents.set(path, serialized);
    return payload;
  }

  async function deleteFile(path, message) {
    const sha = state.fileShas.get(path);
    if (!sha) {
      return;
    }
    await githubFetch(repoApi(path), {
      method: "DELETE",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        message,
        branch: state.config.branch,
        sha,
      }),
    });
    state.fileShas.delete(path);
    state.fileContents.delete(path);
  }

  function buildPublishOperations(metadataPayload) {
    const operations = [];

    const metadataSerialized = serializeJson(metadataPayload);
    if (state.fileContents.get(METADATA_PATH) !== metadataSerialized) {
      operations.push({
        path: METADATA_PATH,
        content: metadataSerialized,
        kind: "update",
        label: "metadata",
      });
    }

    for (const entry of state.entries) {
      const newPath = entryPath(entry);
      const originalPath = state.originalPaths.get(entry.id);
      if (originalPath && originalPath !== newPath) {
        state.deletedPaths.add(originalPath);
      }

      const serialized = serializeJson(entry);
      if (state.fileContents.get(newPath) !== serialized) {
        operations.push({
          path: newPath,
          content: serialized,
          kind: "update",
          label: entry.id,
        });
      }
    }

    for (const path of Array.from(state.deletedPaths)) {
      if (!state.fileContents.has(path) && !state.fileShas.has(path)) {
        state.deletedPaths.delete(path);
        continue;
      }
      operations.push({
        path,
        kind: "delete",
        label: path.split("/").pop().replace(".json", ""),
      });
    }

    return operations;
  }

  async function publishAll() {
    syncConfigFromInputs();
    saveConfigToStorage();

    if (!state.config.owner || !state.config.repo || !state.config.branch || !state.config.token) {
      throw new Error("Preencha owner, reposit\u00f3rio, branch e token antes de publicar.");
    }

    saveCurrentEntry(false);
    const metadataPayload = buildMetadataPayload();

    const usedIds = new Set();
    for (const entry of state.entries) {
      validateEntry(entry);
      if (usedIds.has(entry.id)) {
        throw new Error(`ID duplicado no cat\u00e1logo: ${entry.id}`);
      }
      usedIds.add(entry.id);
    }

    setStatus("Publica\u00e7\u00e3o no GitHub em andamento...", "");

    const operations = buildPublishOperations(metadataPayload);
    if (operations.length === 0) {
      setStatus("Nada mudou no cat\u00e1logo. Nenhum arquivo foi republicado.", "ok");
      return;
    }

    const branchHead = await fetchBranchHead();
    const parentSha = branchHead.object.sha;
    const parentCommit = await fetchCommitObject(parentSha);
    const treeEntries = [];

    for (const operation of operations) {
      if (operation.kind === "delete") {
        treeEntries.push({
          path: operation.path,
          mode: "100644",
          type: "blob",
          sha: null,
        });
        continue;
      }

      const blob = await createBlobFromText(operation.content);
      treeEntries.push({
        path: operation.path,
        mode: "100644",
        type: "blob",
        sha: blob.sha,
      });
    }

    const tree = await createTree(parentCommit.tree.sha, treeEntries);
    const labels = operations
      .map((operation) => operation.label)
      .filter(Boolean)
      .slice(0, 4);
    const commitMessage =
      operations.length === 1
        ? `catalog: publish ${labels[0]}`
        : `catalog: publish ${operations.length} changes${labels.length ? ` (${labels.join(", ")})` : ""}`;

    const commit = await createCommit(commitMessage, tree.sha, parentSha);
    await updateBranchHead(commit.sha);

    await loadRepository();
    for (const entry of state.entries) {
      state.originalPaths.set(entry.id, entryPath(entry));
    }
    state.deletedPaths.clear();
    setStatus(
      `Publica\u00e7\u00e3o conclu\u00edda. ${operations.length} arquivo(s) enviados em 1 commit. O workflow do Pages deve regenerar o \u00edndice automaticamente.`,
      "ok"
    );
  }

  async function loadRepository() {
    syncConfigFromInputs();
    saveConfigToStorage();

    if (!state.config.owner || !state.config.repo || !state.config.branch || !state.config.token) {
      throw new Error("Preencha owner, reposit?rio, branch e token.");
    }

    setStatus("Carregando cat?logo do GitHub...");
    const metadataFile = await fetchJsonFile(METADATA_PATH);
    const entryFiles = await fetchEntries();
    const generatedEntries = await fetchGeneratedIndex();

    state.metadata = metadataFile.content;
    state.entries = entryFiles.map((item) => item.content);
    state.originalPaths.clear();
    state.derivedEntries.clear();
    state.deletedPaths.clear();
    for (const item of entryFiles) {
      state.originalPaths.set(item.content.id, item.path);
    }
    for (const entry of generatedEntries) {
      if (entry && entry.id) {
        state.derivedEntries.set(entry.id, entry);
      }
    }
    state.selectedId = state.entries[0]?.id || null;
    state.isLoaded = true;

    syncInputsFromMetadata();
    fillEntryForm(findSelectedEntry());
    renderEntryList();
    setStatus(`Cat\u00e1logo carregado com ${state.entries.length} entradas.`, "ok");
  }

  function createEmptyEntry() {
    const baseId = slugify(prompt("ID base do novo item:", "novo-item") || "novo-item") || "novo-item";
    let candidate = baseId;
    let suffix = 2;
    while (state.entries.some((entry) => entry.id === candidate)) {
      candidate = `${baseId}-${suffix++}`;
    }

    return {
      id: candidate,
      section: "translations",
      titleId: "",
      name: "",
      introPtBr: "",
      introEnUs: "",
      summary: "",
      summaryPtBr: "",
      summaryEnUs: "",
      author: state.metadata.defaults?.author || "M.I.L.",
      contentTypes: ["translation"],
      detailsUrl: state.metadata.defaults?.detailsUrl || "https://miltraducoes.com/",
      tags: [],
      compatibility: {},
      variants: [],
      featured: false,
    };
  }

  function addEntry(duplicate = false) {
    const current = findSelectedEntry();
    const newEntry = duplicate && current ? structuredClone(current) : createEmptyEntry();
    if (duplicate && current) {
      let candidate = `${current.id}-copy`;
      let suffix = 2;
      while (state.entries.some((entry) => entry.id === candidate)) {
        candidate = `${current.id}-copy-${suffix++}`;
      }
      newEntry.id = candidate;
      newEntry.name = `${current.name} (copy)`;
    }
    state.entries.push(newEntry);
    state.selectedId = newEntry.id;
    fillEntryForm(newEntry);
    renderEntryList();
    setStatus(`Nova entrada pronta para edi??o: '${newEntry.id}'.`);
  }

  function deleteSelectedEntry() {
    const current = findSelectedEntry();
    if (!current) {
      return;
    }
    const confirmed = window.confirm(`Remover a entrada '${current.name || current.id}'?`);
    if (!confirmed) {
      return;
    }
    const originalPath = state.originalPaths.get(current.id);
    if (originalPath) {
      state.deletedPaths.add(originalPath);
      state.originalPaths.delete(current.id);
    }
    state.entries = state.entries.filter((entry) => entry.id !== current.id);
    state.selectedId = state.entries[0]?.id || null;
    fillEntryForm(findSelectedEntry());
    renderEntryList();
    setStatus(`Entrada '${current.id}' removida localmente. Publique para concluir.`, "ok");
  }

  function wireEvents() {
    els.loadRepo.addEventListener("click", async () => {
      try {
        await loadRepository();
      } catch (error) {
        console.error(error);
        setStatus(error.message, "error");
      }
    });

    els.clearSession.addEventListener("click", () => {
      localStorage.removeItem(STORAGE_KEY);
      state.config = { owner: "", repo: "", branch: "main", token: "" };
      syncInputsFromConfig();
      setStatus("Sessão limpa.");
    });

    els.search.addEventListener("input", renderEntryList);
    els.titleId.addEventListener("input", () => applyDerivedNameFromTitleId(false));
    els.section.addEventListener("change", () => {
      const checkedCount = els.contentTypeInputs.filter((input) => input.checked).length;
      if (checkedCount === 0) {
        syncContentTypeInputs(defaultContentTypesForSection(els.section.value.trim()));
      }
    });
    els.newEntry.addEventListener("click", () => addEntry(false));
    els.duplicateEntry.addEventListener("click", () => addEntry(true));
    els.newVariant.addEventListener("click", () => addVariant(false));
    els.duplicateVariant.addEventListener("click", () => addVariant(true));
    els.deleteVariant.addEventListener("click", deleteSelectedVariant);
    els.saveEntry.addEventListener("click", () => {
      try {
        saveCurrentEntry(true);
      } catch (error) {
        console.error(error);
        setStatus(error.message, "error");
      }
    });
    els.deleteEntry.addEventListener("click", deleteSelectedEntry);
    els.publishAll.addEventListener("click", async () => {
      try {
        await publishAll();
      } catch (error) {
        console.error(error);
        setStatus(error.message, "error");
      }
    });
  }

  loadConfigFromStorage();
  syncInputsFromConfig();
  syncInputsFromMetadata();
  wireEvents();
  setStatus("Preencha os dados do repositório e carregue o catálogo.");
})();
