// SPDX-License-Identifier: MPL-2.0
//
// The default Antikolt home page.
//
// Two rules from the architecture are visible here and must not be broken by future edits:
//
//  1. **Only the visible rows exist in the DOM** (docs/HU/03-MEMORIA.md, rule 1). A grid of 5 000
//     cards must cost the same as a grid of 50. That is why the list is virtualised rather than
//     "just rendered and hidden".
//  2. **Nothing here talks to the file system or the network.** The page reaches the browser only
//     through the narrow bridge below (`docs/HU/02-ARCHITEKTURA.md` §11.2), which the real shell
//     injects as `window.antikolt`. Without it, a mock keeps the page usable as a static preview.

const GAP = 16;
const CARD_MIN_WIDTH = 220;
const OVERSCAN_ROWS = 2;

// Container and workspace groupings belong to the page, not to the mock: the shell sends the core's
// cards with a container id, and both data sources must render with the same colours and tabs.
const PAGE_CONTAINERS = [
  { id: "c-work", name: "Munka", color: "#2F6FEB" },
  { id: "c-private", name: "Magán", color: "#2E9E5B" },
  { id: "c-money", name: "Pénz", color: "#E3A008" },
  { id: "c-dev", name: "Fejlesztés", color: "#8E44AD" },
];

const PAGE_WORKSPACES = [
  { id: "w-home", name: "Otthon" },
  { id: "w-work", name: "Munka" },
  { id: "w-read", name: "Olvasnivaló" },
];

/** The bridge contract. Every method here must exist in the real shell's injection. */
function createMockBridge() {
  const now = Date.now();
  const day = 24 * 60 * 60 * 1000;

  const containers = PAGE_CONTAINERS;
  const workspaces = PAGE_WORKSPACES;

  const raw = [
    ["Google — munka", "https://mail.google.com/", "Munka", "signed_in", true, "l", 12],
    ["Antikolt projekt", "https://github.com/antikolt/antikolt", "Fejlesztés", "signed_in", true, "l", 4],
    ["YouTube", "https://www.youtube.com/", "Magán", "signed_in", false, "m", 3],
    ["Facebook", "https://www.facebook.com/", "Magán", "expiring", false, "m", 2],
    ["Saját weboldal", "https://sajatoldal.hu/admin", "Magán", "signed_in", true, "s", 1],
    ["GitHub", "https://github.com/", "Fejlesztés", "signed_in", false, "m", 0.5],
    ["Hacker News", "https://news.ycombinator.com/", "Fejlesztés", "signed_in", false, "m", 0.2],
    ["MDN Web Docs", "https://developer.mozilla.org/", "Fejlesztés", "unknown", false, "m", 0.4],
    ["Gmail — magán", "https://mail.google.com/mail/u/1/", "Magán", "signed_in", false, "m", 0.3],
    ["Bank", "https://netbank.example.hu/", "Pénz", "signed_out", false, "m", 0.6],
    ["Telex", "https://telex.hu/", "Otthon", "unknown", false, "s", 0.1],
    ["444", "https://444.hu/", "Otthon", "unknown", false, "s", 0.1],
    ["Portfolio", "https://www.portfolio.hu/", "Pénz", "unknown", false, "s", 0.2],
    ["Stack Overflow", "https://stackoverflow.com/", "Fejlesztés", "signed_in", false, "m", 0.3],
    ["crates.io", "https://crates.io/", "Fejlesztés", "signed_in", false, "s", 0.2],
    ["Chromium Embedded Framework", "https://github.com/chromiumembedded/cef", "Fejlesztés", "signed_in", false, "m", 0.1],
    ["Discord", "https://discord.com/app", "Magán", "signed_in", false, "m", 0.4],
    ["Netflix", "https://www.netflix.com/browse", "Magán", "signed_in", false, "m", 0.05],
    ["Wikipédia", "https://hu.wikipedia.org/", "Otthon", "unknown", false, "m", 0.9],
    ["Időjárás", "https://www.idokep.hu/", "Otthon", "unknown", false, "s", 0.7],
    ["BKK menetrend", "https://bkk.hu/", "Otthon", "unknown", false, "s", 0.8],
    ["Figma", "https://www.figma.com/files", "Munka", "signed_in", false, "m", 0.3],
    ["Notion", "https://www.notion.so/", "Munka", "signed_in", false, "m", 0.2],
    ["Szerver napló", "http://192.168.1.10:8080/", "Fejlesztés", "signed_out", false, "m", 0.4],
    ["Rust docs", "https://doc.rust-lang.org/std/", "Fejlesztés", "unknown", false, "s", 0.3],
    ["KeePassXC letöltés", "https://keepassxc.org/download/", "Pénz", "unknown", false, "s", 0.2],
    ["Főnix fórum", "https://example.org/forum/", "Otthon", "unknown", false, "s", 0.6],
    ["Régi blog", "https://example.net/blog", "Otthon", "unknown", true, "s", 0.5],
  ];

  const cards = raw.map((row, index) => {
    const [title, url, containerName, status, pinned, size, luminance] = row;
    const container = containers.find((c) => c.name === containerName) ?? containers[1];
    return {
      id: `01MOCK${String(index).padStart(20, "0")}`,
      title,
      url,
      domain: safeDomain(url),
      notes: index % 5 === 0 ? "Megnézni a hétvégén" : "",
      tags: index % 3 === 0 ? ["olvasnivaló"] : [],
      status,
      pinned,
      archived: false,
      size,
      container,
      previewRef: null,
      luminance,
      lastOpenedAt: now - index * day * 0.4,
      workspaceId: index > 20 ? "w-work" : index > 12 ? "w-read" : "w-home",
    };
  });

  let layout = null;

  return {
    cards: {
      list: () => Promise.resolve(cards.slice()),
      open: (id) => {
        const card = cards.find((c) => c.id === id);
        console.info("[mock] open card", id, card ? card.url : "");
        return Promise.resolve();
      },
      setLayout: (id, next) => {
        layout = { id, ...next };
        return Promise.resolve();
      },
    },
    workspaces: {
      list: () => Promise.resolve(workspaces.slice()),
      switch: (id) => Promise.resolve(id),
    },
    containers: { list: () => Promise.resolve(containers.slice()) },
    search: (query) => {
      const q = String(query || "").toLowerCase();
      if (!q) return Promise.resolve(cards.slice());
      return Promise.resolve(
        cards.filter((c) =>
          [c.title, c.url, c.notes, ...(c.tags || [])]
            .join(" ")
            .toLowerCase()
            .includes(q),
        ),
      );
    },
    events: { on: () => () => {} },
    system: {
      memory: () =>
        Promise.resolve({ usedBytes: 0, budgetBytes: 1024 * 1024 * 1024, engineState: "mock" }),
      engine: () => Promise.resolve("mock bridge (statikus előnézet)"),
      getLayout: () => Promise.resolve(layout),
    },
  };
}

function safeDomain(url) {
  try {
    return new URL(url).host.replace(/^www\./, "");
  } catch {
    return "ismeretlen";
  }
}

/** Deterministic hue from a string, used for placeholder tiles and container dots. */
function hashHue(text) {
  let hash = 0;
  for (let i = 0; i < text.length; i += 1) {
    hash = (hash * 31 + text.charCodeAt(i)) | 0;
  }
  return Math.abs(hash) % 360;
}

/** The card the keyboard should return to after the list re-renders (note save/cancel). */
let pendingFocusId = null;

const STATUS = {
  signed_in: { icon: "🔒", label: "bejelentkezve" },
  expiring: { icon: "⏳", label: "hamarosan lejár" },
  signed_out: { icon: "🔓", label: "kijelentkezve" },
  unknown: { icon: "·", label: "ismeretlen állapot" },
};

/**
 * The core's card record (`crates/antikolt-core-service/src/store.rs`) as the grid shows it.
 *
 * The two shapes differ on purpose: the core stores facts (URL, container id, when it was opened),
 * the page needs what it draws (domain, container colour, size class). One adapter, one place to
 * change when either side moves.
 */
function pageCardFromCore(row, index) {
  const containerId = String(row.container ?? "");
  const container =
    PAGE_CONTAINERS.find((entry) => entry.id === containerId) ??
    PAGE_CONTAINERS.find((entry) => entry.name === containerId) ??
    PAGE_CONTAINERS[1];
  const statuses = ["signed_in", "expiring", "signed_out", "unknown"];
  const pinned = Boolean(row.pinned);
  return {
    id: String(row.id ?? ""),
    title: String(row.title ?? "") || String(row.url ?? "") || "cím nélküli kártya",
    url: String(row.url ?? ""),
    domain: safeDomain(String(row.url ?? "")),
    notes: String(row.notes ?? ""),
    tags: [],
    status: statuses.includes(row.status) ? row.status : "unknown",
    pinned,
    archived: false,
    size: pinned ? "l" : "m",
    container,
    // The preview is produced by the shell and served by the engine from the profile (ADR-035): the
    // record only carries its name, and an empty one means the card has no picture yet.
    previewRef: String(row.preview_ref ?? "") || null,
    luminance: 0,
    lastOpenedAt: Number(row.last_opened_at_ms) || 0,
    workspaceId: index > 12 ? "w-read" : "w-home",
  };
}

/** The page's own search rule (title, URL, note, tag). */
function matchesTerm(card, term) {
  const query = String(term || "").toLowerCase();
  if (!query) return true;
  return [card.title, card.url, card.notes, ...(card.tags || [])]
    .join(" ")
    .toLowerCase()
    .includes(query);
}

/// Where `cards.open` finds the URL and title it hands to the shell. The list the page shows is the
/// truth — the shell may not know this card yet — so the newest full list is kept here instead of
/// being re-read from the mock.
const cardLookup = { rows: [] };

/** `<antikolt-card>`: the one element a user-authored home page inserts. */
class AntikoltCard extends HTMLElement {
  static get observedAttributes() {
    return ["size"];
  }

  #card = null;

  set card(value) {
    this.#card = value;
    this.render();
  }

  get card() {
    return this.#card;
  }

  connectedCallback() {
    if (!this.shadowRoot) {
      this.attachShadow({ mode: "open" });
    }
    this.render();
  }

  render() {
    if (!this.isConnected) return;
    const card = this.#card;
    const root = this.shadowRoot;
    if (!root) return;

    if (!card) {
      root.replaceChildren();
      return;
    }

    const status = STATUS[card.status] ?? STATUS.unknown;
    const themeHost = document.documentElement.getAttribute("data-theme") ?? "grey";

    // Rebuild only when something visible changed; the grid re-creates elements aggressively, so
    // this stays cheap. The signature lives on the *host*: a `ShadowRoot` is a `DocumentFragment`,
    // not an `HTMLElement`, so it has no `dataset` — reading it threw on every single card, which
    // left the grid full of empty elements and only looked like "a page that renders nothing".
    // The note is part of the visible card, and leaving it out was a real defect: an edited note stayed
    // stale on screen until something else changed the card. It is also what lets render() run after a
    // save, which is where the focus is restored.
    const signature = `${card.id}|${card.title}|${card.size}|${card.status}|${card.pinned}|${card.notes}|${themeHost}`;
    if (this.dataset.signature === signature) return;
    this.dataset.signature = signature;

    const style = document.createElement("style");
    style.textContent = cardStyles();

    const article = document.createElement("article");
    article.className = "card";
    article.dataset.size = card.size || "m";
    article.tabIndex = -1;
    article.setAttribute("role", "listitem");
    article.setAttribute("aria-label", `${card.title} — ${card.domain} — ${status.label}`);
    article.dataset.cardId = card.id;
    if (pendingFocusId && card.id === pendingFocusId) {
      // A note save re-renders the list — and the core's push re-renders it *again* — so the focus must be
      // re-applied on every rebuild until the list has settled; clearing it on the first one lost the race
      // (measured: activeElement was null after the push).
      requestAnimationFrame(() => {
        if (article.isConnected) article.focus();
      });
      const wanted = pendingFocusId;
      setTimeout(() => {
        if (pendingFocusId === wanted) pendingFocusId = null;
      }, 2000);
    }

    const preview = document.createElement("div");
    preview.className = "card-preview";

    if (card.previewRef) {
      const img = document.createElement("img");
      img.src = `antikolt://blob/${card.previewRef}`;
      img.alt = "";
      img.loading = "lazy";
      img.decoding = "async";
      preview.append(img);
    } else {
      const tile = document.createElement("span");
      tile.className = "placeholder";
      tile.textContent = (card.domain || "?").charAt(0).toUpperCase();
      tile.style.background = `linear-gradient(135deg, hsl(${hashHue(card.domain)} 24% 34%), hsl(
        ${(hashHue(card.domain) + 40) % 360} 22% 26%
      ))`;
      preview.append(tile);
    }

    const body = document.createElement("div");
    body.className = "card-body";

    const title = document.createElement("div");
    title.className = "card-title";
    title.textContent = card.title || card.domain;

    const meta = document.createElement("div");
    meta.className = "card-meta";

    const dot = document.createElement("span");
    dot.className = "dot";
    dot.style.background = card.container?.color ?? "var(--text-muted)";
    dot.title = card.container?.name ?? "";

    const domain = document.createElement("span");
    domain.className = "card-domain";
    domain.textContent = card.domain;

    const badge = document.createElement("button");
    badge.type = "button";
    badge.className = "badge pin";
    badge.textContent = `${card.pinned ? "📌 " : ""}${card.archived ? "💤 " : ""}${status.icon}`;
    badge.title = card.pinned ? "Kitűzés levétele" : "Kitűzés";
    // The card itself opens on click, so the pin must not travel further: it is a control inside a
    // link-shaped surface, and a click on it would otherwise also open the card.
    badge.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      this.dispatchEvent(
        new CustomEvent("antikolt-pin", {
          detail: { id: this.card?.id },
          bubbles: true,
          composed: true,
        }),
      );
    });

    // Notes are edited in place, not in a dialog: the editor is part of the card, so the card's own
    // click (which opens it) must not fire from anything inside the editor.
    const noteButton = document.createElement("button");
    noteButton.type = "button";
    noteButton.className = "note";
    noteButton.textContent = card.notes ? "✎" : "＋";
    noteButton.title = card.notes ? "Jegyzet szerkesztése" : "Jegyzet hozzáadása";

    const editor = document.createElement("div");
    editor.className = "note-editor";
    editor.hidden = true;
    const textarea = document.createElement("textarea");
    textarea.rows = 2;
    textarea.value = card.notes || "";
    textarea.setAttribute("aria-label", "Jegyzet");
    const save = document.createElement("button");
    save.type = "button";
    save.className = "note-save";
    save.textContent = "Mentés";
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.className = "note-cancel";
    cancel.textContent = "Mégse";
    // Typing must not reach the grid's keyboard navigation, and selecting text must not open the card.
    for (const eventName of ["click", "keydown", "keyup", "pointerdown"]) {
      textarea.addEventListener(eventName, (event) => event.stopPropagation());
    }
    save.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      editor.hidden = true;
      pendingFocusId = this.card?.id ?? null;
      this.dispatchEvent(
        new CustomEvent("antikolt-note", {
          detail: { id: this.card?.id, notes: textarea.value },
          bubbles: true,
          composed: true,
        }),
      );
    });
    cancel.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      textarea.value = this.card?.notes || "";
      editor.hidden = true;
      // Nothing re-renders on cancel, so the card is still there to take the focus back.
      this.querySelector(".card")?.focus();
    });
    editor.append(textarea, save, cancel);
    noteButton.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      // Open in place: a re-render would throw the typed text away.
      editor.hidden = !editor.hidden;
      if (!editor.hidden) textarea.focus();
    });

    meta.append(dot, domain, noteButton, badge);
    body.append(title, meta);

    if (card.notes) {
      const notes = document.createElement("div");
      notes.className = "card-meta card-notes";
      notes.style.opacity = "0.75";
      notes.textContent = card.notes;
      body.append(notes);
    }
    body.append(editor);

    article.append(preview, body);
    root.replaceChildren(style, article);
  }
}

function cardStyles() {
  // The component brings its own styles so a user-authored home page cannot break the cards by
  // accident, and so the same markup renders identically in the pinned row and in the grid.
  return `
    :host { display: block; }
    .card {
      display: flex; flex-direction: column;
      border: 1px solid var(--border); border-radius: var(--radius-card);
      background: var(--surface); box-shadow: var(--shadow-1);
      overflow: hidden; cursor: pointer; outline: none;
      transition: box-shadow var(--motion-fast) var(--ease-out);
    }
    .card:hover, .card:focus-visible { box-shadow: var(--shadow-2); }
    .card:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
    .card-preview { position: relative; aspect-ratio: 16 / 10; display: grid; place-items: center; overflow: hidden; background: var(--surface-raised); }
    .card[data-size="s"] .card-preview { aspect-ratio: 16 / 9; }
    .card-preview img { width: 100%; height: 100%; object-fit: cover; object-position: top center; display: block; }
    .placeholder { width: 100%; height: 100%; display: grid; place-items: center; font-size: 28px; font-weight: 600; color: #fff; opacity: .85; }
    .card-preview::after { content: ""; position: absolute; inset: auto 0 0 0; height: 42%; background: linear-gradient(to top, rgb(0 0 0 / 45%), transparent); }
    .card-body { display: flex; flex-direction: column; gap: 2px; padding: 0 12px 12px; }
    .card-title { font: 500 13px/1.45 Inter, "Segoe UI", system-ui, sans-serif; color: var(--text); display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; overflow-wrap: anywhere; }
    .card[data-size="s"] .card-title { -webkit-line-clamp: 1; }
    .card-meta { display: flex; align-items: center; gap: 4px; font: 11px/1.45 Inter, "Segoe UI", system-ui, sans-serif; color: var(--text-muted); min-width: 0; }
    .card-domain { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .dot { width: 6px; height: 6px; border-radius: 50%; flex: none; }
    .badge { margin-left: auto; flex: none; }
    .pin { background: none; border: 0; padding: 0 2px; font: inherit; color: inherit; cursor: pointer; border-radius: 4px; }
    .pin:hover, .pin:focus-visible { outline: 2px solid var(--accent); outline-offset: 1px; }
    .note { background: none; border: 0; padding: 0 2px; margin-left: auto; font: inherit; color: inherit; cursor: pointer; border-radius: 4px; }
    .note:hover, .note:focus-visible { outline: 2px solid var(--accent); outline-offset: 1px; }
    .badge { margin-left: 0; }
    .note-editor { display: flex; flex-direction: column; gap: 4px; padding: 6px 0 0; }
    .note-editor[hidden] { display: none; }
    .note-editor textarea { width: 100%; resize: vertical; font: inherit; color: var(--text); background: var(--surface-raised); border: 1px solid var(--border); border-radius: 6px; padding: 4px 6px; }
    .note-editor button { font: inherit; cursor: pointer; align-self: flex-start; }
    @media (prefers-reduced-motion: reduce) { .card { transition: none; } }
  `;
}

if (!customElements.get("antikolt-card")) {
  customElements.define("antikolt-card", AntikoltCard);
}

/** The page itself. */
class HomePage {
  constructor(bridge) {
    this.bridge = bridge;
    this.cards = [];
    this.allCards = [];
    /** Set when the shell pushes the core's list; `null` keeps the bridge's own (sample) cards. */
    this.coreCards = null;
    /** `sample` until the shell pushes the core's list, then `core` — the self test reads this. */
    this.cardSource = "sample";
    this.filtered = [];
    this.workspaces = [];
    this.activeWorkspace = null;
    this.searchTerm = "";
    this.cols = 1;
    this.rowHeight = 190;
    this.focusedIndex = -1;

    this.viewport = document.getElementById("viewport");
    this.windowEl = document.getElementById("window");
    this.spacer = document.getElementById("spacer");
    this.countEl = document.getElementById("count");
    this.emptyEl = document.getElementById("empty");
    this.memEl = document.getElementById("mem");
    this.engineEl = document.getElementById("engine");
    this.searchEl = document.getElementById("search");
    this.pinnedSection = document.getElementById("pinned-section");
    this.pinnedRow = document.getElementById("pinned");
  }

  async start() {
    this.workspaces = await this.bridge.workspaces.list();
    this.activeWorkspace = this.workspaces[0]?.id ?? null;
    this.renderWorkspaces();

    // A push from the shell may already have arrived (it is sent when the page finishes loading), so
    // the pushed list wins over whatever the bridge would hand out.
    this.allCards = this.coreCards
      ? this.coreCards.map(pageCardFromCore)
      : await this.bridge.cards.list();
    if (this.coreCards) this.indexCoreRows();
    this.cards = this.allCards.slice();
    cardLookup.rows = this.allCards;
    this.applyFilter();

    this.bindEvents();
    this.restoreTheme();
    await this.refreshSystem();
  }

  /**
   * Replace the card list with the one the core owns.
   *
   * Called from `window.antikoltAcceptCards` when the shell pushes the list it got from the core. If
   * no push ever arrives (no core running), nothing here runs and the page keeps its sample cards —
   * a grid with samples is better than an empty grid, and the shell's status bar says why.
   */
  acceptCards(rows) {
    this.coreCards = Array.isArray(rows) ? rows : [];
    this.indexCoreRows();
    this.allCards = this.coreCards.map(pageCardFromCore);
    this.cards = this.allCards.slice();
    cardLookup.rows = this.allCards;
    this.cardSource = "core";
    this.applyFilter();
    this.refreshSystem().catch(() => {});
  }

  /**
   * Remember the core's own records by id.
   *
   * The grid's model is a *view* (a domain, a container object, a preview reference); the core's record
   * is what `cards.upsert` validates. Writing must send the record, not the view — sending the view was
   * the kind of mistake that fails as a type error somewhere else entirely.
   */
  indexCoreRows() {
    this.coreRowById = new Map((this.coreCards || []).map((row) => [row.id, row]));
  }

  /**
   * Pin or unpin one card, through the shell (`cards.upsert`), which asks the core.
   *
   * The grid re-renders from the answer: the page shows a change only after the core has it, so a failed
   * write cannot leave the grid claiming something the store does not hold. In sample mode (no engine)
   * there is no channel and nothing is pretended.
   */
  async togglePin(id) {
    if (!id) return;
    const model = this.allCards.find((card) => card.id === id);
    const record = this.coreRowById?.get(id);
    if (!model || !record) {
      this.lastPin = { id, error: "nincs mag-rekord ehhez a kártyához" };
      return;
    }
    if (typeof this.bridge?.cards?.upsert !== "function") {
      this.lastPin = { id, error: "a híd nem tud írni (nincs motor)" };
      return;
    }
    const next = { ...record, pinned: !record.pinned };
    try {
      const answer = await this.bridge.cards.upsert(next);
      const saved = answer?.card ?? next;
      const stored = this.coreRowById.get(id);
      if (stored) stored.pinned = Boolean(saved.pinned);
      model.pinned = Boolean(saved.pinned);
      model.size = model.pinned ? "l" : "m";
      this.lastPin = { id, pinned: model.pinned, error: "" };
      this.windowEl.dataset.signature = "";
      this.applyFilter();
    } catch (error) {
      this.lastPin = { id, error: String(error?.message ?? error) };
    }
  }

  /**
   * Save one card's note through the shell (`cards.upsert`), then re-render from the answer.
   *
   * Same rule as the pin: the grid shows what the core accepted, so a failed write cannot leave the card
   * claiming a note the store does not hold.
   */
  async setNote(id, notes) {
    if (!id) return;
    const model = this.allCards.find((card) => card.id === id);
    const record = this.coreRowById?.get(id);
    if (!model || !record) {
      this.lastNote = { id, notes: "", error: "nincs mag-rekord ehhez a kártyához" };
      return;
    }
    if (typeof this.bridge?.cards?.upsert !== "function") {
      this.lastNote = { id, notes: "", error: "a híd nem tud írni (nincs motor)" };
      return;
    }
    const next = { ...record, notes: String(notes ?? "") };
    try {
      const answer = await this.bridge.cards.upsert(next);
      const saved = answer?.card ?? next;
      const stored = this.coreRowById.get(id);
      if (stored) stored.notes = String(saved.notes ?? "");
      model.notes = String(saved.notes ?? "");
      this.lastNote = { id, notes: model.notes, error: "" };
      this.windowEl.dataset.signature = "";
      this.applyFilter();
    } catch (error) {
      this.lastNote = { id, notes: "", error: String(error?.message ?? error) };
    }
  }

  bindEvents() {
    window.addEventListener("resize", () => this.onResize(), { passive: true });
    this.viewport.addEventListener("scroll", () => this.renderWindow(), { passive: true });

    let timer = 0;
    this.searchEl.addEventListener("input", () => {
      window.clearTimeout(timer);
      timer = window.setTimeout(async () => {
        this.searchTerm = this.searchEl.value.trim();
        if (this.coreCards) {
          // The core's list is searched in the page: a round trip per keystroke is not worth it, and
          // the set is small by design.
          this.cards = this.allCards.filter((card) => matchesTerm(card, this.searchTerm));
        } else {
          this.cards = this.searchTerm
            ? await this.bridge.search(this.searchTerm)
            : await this.bridge.cards.list();
        }
        this.applyFilter();
      }, 80);
    });

    document.querySelectorAll("[data-theme-set]").forEach((button) => {
      button.addEventListener("click", () => this.setTheme(button.dataset.themeSet));
    });

    document.addEventListener("keydown", (event) => this.onKeydown(event));

    // The pin badge lives inside a card's shadow root, so its click arrives as a composed event from
    // the host. It is handled here, before anything else, because a pin is not an "open".
    document.addEventListener("antikolt-pin", (event) => this.togglePin(event.detail?.id));
    document.addEventListener("antikolt-note", (event) =>
      this.setNote(event.detail?.id, event.detail?.notes),
    );

    // Delegated on the document on purpose: the pinned row lives outside `#viewport`, so a listener
    // bound there never saw a click on a pinned card — the most prominent cards were inert. One
    // listener for both places, and the `closest` check decides what was hit.
    document.addEventListener("click", (event) => {
      const card = event.target.closest?.(".card, antikolt-card");
      if (!card) return;
      const host = card.closest("antikolt-card");
      const id = host?.card?.id;
      if (id) this.bridge.cards.open(id, { mode: "live" });
    });
  }

  onResize() {
    this.layout();
    this.renderWindow();
  }

  renderWorkspaces() {
    const nav = document.getElementById("workspaces");
    nav.replaceChildren();
    for (const workspace of this.workspaces) {
      const button = document.createElement("button");
      button.type = "button";
      button.setAttribute("role", "tab");
      button.setAttribute("aria-selected", String(workspace.id === this.activeWorkspace));
      button.textContent = workspace.name;
      button.addEventListener("click", () => {
        this.activeWorkspace = workspace.id;
        this.renderWorkspaces();
        this.applyFilter();
      });
      nav.append(button);
    }
  }

  applyFilter() {
    const byWorkspace = this.cards.filter(
      (card) => !this.activeWorkspace || card.workspaceId === this.activeWorkspace,
    );
    this.filtered = byWorkspace;

    const pinned = this.filtered.filter((card) => card.pinned);
    this.renderPinned(pinned);
    this.pinnedSection.hidden = pinned.length === 0;

    this.layout();
    this.viewport.scrollTop = 0;
    this.renderWindow();

    const total = this.filtered.length;
    this.countEl.textContent = total ? `${total} kártya` : "";
    this.emptyEl.hidden = total !== 0;
    this.windowEl.hidden = total === 0;
  }

  renderPinned(cards) {
    this.pinnedRow.replaceChildren();
    for (const card of cards.slice(0, 8)) {
      const element = document.createElement("antikolt-card");
      element.card = card;
      element.style.width = "260px";
      this.pinnedRow.append(element);
    }
  }

  layout() {
    const width = this.viewport.clientWidth || 1;
    this.cols = Math.max(1, Math.floor((width + GAP) / (CARD_MIN_WIDTH + GAP)));
    this.windowEl.style.gridTemplateColumns = `repeat(${this.cols}, minmax(0, 1fr))`;
    this.windowEl.style.gap = `${GAP}px`;

    // Measure one card so the virtualiser knows a row's height exactly instead of guessing.
    const probe = document.createElement("antikolt-card");
    probe.card = this.filtered[0] ?? {
      id: "probe",
      title: "minta",
      domain: "pelda.hu",
      status: "unknown",
      size: "m",
      container: { color: "#666" },
    };
    probe.style.visibility = "hidden";
    this.windowEl.append(probe);
    const measured = probe.getBoundingClientRect().height;
    probe.remove();
    if (measured > 40) {
      this.rowHeight = Math.ceil(measured) + GAP;
    }

    const rows = Math.ceil(this.filtered.length / this.cols);
    this.spacer.style.height = `${rows * this.rowHeight}px`;

    // Column count or metrics changed: the rendered window is no longer valid.
    this.windowEl.dataset.signature = "";
  }

  rowCount() {
    return Math.ceil(this.filtered.length / this.cols);
  }

  renderWindow() {
    const totalRows = this.rowCount();
    if (totalRows === 0) {
      this.windowEl.replaceChildren();
      return;
    }
    const top = this.viewport.scrollTop;
    const height = this.viewport.clientHeight || 600;
    const first = Math.max(0, Math.floor(top / this.rowHeight) - OVERSCAN_ROWS);
    const last = Math.min(totalRows - 1, Math.ceil((top + height) / this.rowHeight) + OVERSCAN_ROWS);

    const signature = `${first}:${last}:${this.cols}:${this.filtered.length}:${this.themeSignature()}`;
    if (this.windowEl.dataset.signature === signature) return;
    this.windowEl.dataset.signature = signature;

    this.windowEl.style.transform = `translateY(${first * this.rowHeight}px)`;

    const fragment = document.createDocumentFragment();
    const startIndex = first * this.cols;
    const endIndex = Math.min(this.filtered.length, (last + 1) * this.cols);
    for (let index = startIndex; index < endIndex; index += 1) {
      const card = this.filtered[index];
      if (!card) continue;
      const element = document.createElement("antikolt-card");
      element.card = card;
      element.dataset.index = String(index);
      if (index === this.focusedIndex) {
        element.querySelector(".card")?.setAttribute("tabindex", "0");
      }
      fragment.append(element);
    }
    this.windowEl.replaceChildren(fragment);

    if (this.focusedIndex >= startIndex && this.focusedIndex < endIndex) {
      const focused = this.windowEl.querySelector(`antikolt-card[data-index="${this.focusedIndex}"]`);
      focused?.shadowRoot?.querySelector(".card")?.focus({ preventScroll: true });
    }
  }

  themeSignature() {
    return document.documentElement.getAttribute("data-theme") ?? "grey";
  }

  onKeydown(event) {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      this.searchEl.focus();
      this.searchEl.select();
      return;
    }
    if (event.key === "Escape" && document.activeElement === this.searchEl) {
      this.searchEl.value = "";
      this.searchEl.dispatchEvent(new Event("input"));
      return;
    }
    if (document.activeElement === this.searchEl) return;

    const total = this.filtered.length;
    if (total === 0) return;

    const current = this.focusedIndex < 0 ? 0 : this.focusedIndex;
    let next = current;
    switch (event.key) {
      case "ArrowRight":
        next = Math.min(total - 1, current + 1);
        break;
      case "ArrowLeft":
        next = Math.max(0, current - 1);
        break;
      case "ArrowDown":
        next = Math.min(total - 1, current + this.cols);
        break;
      case "ArrowUp":
        next = Math.max(0, current - this.cols);
        break;
      case "Home":
        next = 0;
        break;
      case "End":
        next = total - 1;
        break;
      case "Enter":
        if (this.focusedIndex >= 0) {
          this.bridge.cards.open(this.filtered[this.focusedIndex].id, { mode: "live" });
        }
        return;
      default:
        return;
    }

    event.preventDefault();
    this.focusedIndex = next;
    this.scrollIndexIntoView(next);
    this.windowEl.dataset.signature = "";
    this.renderWindow();
  }

  scrollIndexIntoView(index) {
    const row = Math.floor(index / this.cols);
    const rowTop = row * this.rowHeight;
    const rowBottom = rowTop + this.rowHeight;
    const viewTop = this.viewport.scrollTop;
    const viewBottom = viewTop + this.viewport.clientHeight;
    if (rowTop < viewTop) {
      this.viewport.scrollTop = rowTop;
    } else if (rowBottom > viewBottom) {
      this.viewport.scrollTop = rowBottom - this.viewport.clientHeight;
    }
  }

  setTheme(theme) {
    if (!["light", "grey", "dark", "amoled"].includes(theme)) return;
    document.documentElement.setAttribute("data-theme", theme);
    try {
      localStorage.setItem("antikolt.theme", theme);
    } catch {
      // A locked-down profile may forbid storage; the theme still applies for this session.
    }
    document.querySelectorAll("[data-theme-set]").forEach((button) => {
      button.setAttribute("aria-pressed", String(button.dataset.themeSet === theme));
    });
    // Cards carry a theme-dependent signature, so force them to re-render.
    this.windowEl.dataset.signature = "";
    this.renderPinned(this.filtered.filter((card) => card.pinned));
    this.renderWindow();
  }

  restoreTheme() {
    let theme = "grey";
    try {
      theme = localStorage.getItem("antikolt.theme") || "grey";
    } catch {
      // ignore
    }
    this.setTheme(theme);
  }

  async refreshSystem() {
    try {
      const memory = await this.bridge.system.memory();
      if (memory && typeof memory.usedBytes === "number" && memory.usedBytes > 0) {
        const used = Math.round(memory.usedBytes / (1024 * 1024));
        const budget = Math.round((memory.budgetBytes || 0) / (1024 * 1024));
        // A budget of zero means "no budget is known yet" (F6), not "zero megabytes".
        this.memEl.textContent = budget > 0 ? `${used} / ${budget} MB` : `${used} MB`;
      } else {
        this.memEl.textContent = "motor: hideg";
      }
      const engine = await this.bridge.system.engine();
      this.engineEl.textContent = `motor: ${engine}`;
    } catch {
      this.memEl.textContent = "—";
    }
  }
}

// The bridge: a full `window.antikolt` when the shell injects one, otherwise the mock. The native
// bridge shipped today (`window.antikoltNative`) covers the one action that must be real — opening a
// card — while the mock still supplies the card list until the core's card API lands. The two are
// composed here instead of being faked apart, so a click on a card really opens it.
const mockBridge = createMockBridge();

/**
 * The mock bridge, plus the one thing that must be real when the shell is around: opening a card.
 *
 * The native bridge is looked up **at call time**, not at start-up. The engine injects
 * `window.antikoltNative`, but that injection happens when the browser-side load handler runs, which
 * is *after* the document's own scripts start — so a page that decided at start-up found nothing and
 * silently kept the sample behaviour (measured: a click opened the sample card instead of asking the
 * shell). A call-time check also keeps the page honest when it is opened in a plain browser.
 */
function withNativeOpen(mock) {
  const nativeBridge = () =>
    typeof window.antikoltNative !== "undefined" ? window.antikoltNative : null;
  return {
    ...mock,
    cards: {
      ...mock.cards,
      open: async (id, options) => {
        const native = nativeBridge();
        if (!native) {
          return mock.cards.open(id, options);
        }
        // The card's own URL and title travel with the call: the shell may not know this card yet,
        // and the page is the only place the list currently lives. `cardLookup` holds the list the
        // page is actually showing (the core's, once it arrived), not the mock's samples.
        const card =
          cardLookup.rows.find((entry) => entry.id === id) ||
          (await mock.cards.list()).find((entry) => entry.id === id) ||
          {};
        try {
          await native.openCard({
            ...(options || {}),
            id,
            url: card.url,
            title: card.title,
            // The shell cannot see the list, so the pinned flag has to travel with the request: a
            // pinned card is the documented opt-out from automatic hibernation.
            pinned: Boolean(card.pinned),
          });
        } catch (error) {
          console.warn("openCard failed", error);
        }
      },
      // The write half of the channel (protocol 2). In a plain browser there is no engine, so the
      // answer is an honest rejection instead of a write that never happened.
      upsert: async (card) => {
        const native = nativeBridge();
        if (!native?.cards?.upsert) {
          throw new Error("a híd nem tud írni (nincs motor)");
        }
        return native.cards.upsert(card);
      },
    },
    system: {
      ...mock.system,
      engine: () =>
        Promise.resolve(
          nativeBridge() ? "CEF (beágyazott kártya)" : "mock bridge (statikus előnézet)",
        ),
      // The memory figure is measured by the engine (ADR-036), not estimated here. `budgetBytes` is 0
      // until profiles carry a budget, and the renderer then shows the used figure alone.
      memory: async () => {
        const native = nativeBridge();
        if (!native?.request) {
          return mock.system.memory();
        }
        const answer = await native.request("system.memory", {});
        return {
          usedBytes: Number(answer?.usedBytes) || 0,
          budgetBytes: Number(answer?.budgetBytes) || 0,
        };
      },
    },
  };
}

const bridge = window.antikolt ?? withNativeOpen(mockBridge);
const page = new HomePage(bridge);
page.start().catch((error) => {
  console.error("home page failed to start", error);
});

// The shell pushes the cards the core owns. The push stays because it is how a fresh grid is filled
// without asking; the two-way channel (protocol 2) is what the page uses to *write*.
window.antikoltAcceptCards = (payload) => {
  const parsed = typeof payload === "string" ? JSON.parse(payload) : payload;
  const rows = Array.isArray(parsed?.cards) ? parsed.cards : [];
  page.acceptCards(rows);
  return rows.length;
};

// What the grid actually shows: the page-side witness a self test reads instead of guessing from a
// screenshot or from the shell's own bookkeeping.
window.antikoltGridState = () => ({
  source: page.cardSource,
  count: page.cards.length,
  filtered: page.filtered.length,
  firstTitle: page.cards[0]?.title ?? "",
  firstId: page.cards[0]?.id ?? "",
  firstUrl: page.cards[0]?.url ?? "",
  titles: page.cards.map((card) => card.title),
});

// The pin witness: what the last pin did, and the pinned titles as the grid now shows them.
window.antikoltPinState = () => ({
  source: page.cardSource,
  pinnedCount: page.cards.filter((card) => card.pinned).length,
  pinnedTitles: page.cards.filter((card) => card.pinned).map((card) => card.title),
  lastId: page.lastPin?.id ?? "",
  lastPinned: page.lastPin?.pinned ?? null,
  lastError: page.lastPin?.error ?? "",
});

// The note witness: the last saved note, and the grid's own copy of it (they must agree).
window.antikoltNoteState = () => ({
  source: page.cardSource,
  lastId: page.lastNote?.id ?? "",
  notes: page.lastNote?.notes ?? "",
  error: page.lastNote?.error ?? "",
  cardNotes: page.cards.find((card) => card.id === page.lastNote?.id)?.notes ?? "",
  cardsWithNotes: page.cards.filter((card) => card.notes).length,
});
