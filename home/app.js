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

/** The bridge contract. Every method here must exist in the real shell's injection. */
function createMockBridge() {
  const now = Date.now();
  const day = 24 * 60 * 60 * 1000;

  const containers = [
    { id: "c-work", name: "Munka", color: "#2F6FEB" },
    { id: "c-private", name: "Magán", color: "#2E9E5B" },
    { id: "c-money", name: "Pénz", color: "#E3A008" },
    { id: "c-dev", name: "Fejlesztés", color: "#8E44AD" },
  ];

  const workspaces = [
    { id: "w-home", name: "Otthon" },
    { id: "w-work", name: "Munka" },
    { id: "w-read", name: "Olvasnivaló" },
  ];

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

const STATUS = {
  signed_in: { icon: "🔒", label: "bejelentkezve" },
  expiring: { icon: "⏳", label: "hamarosan lejár" },
  signed_out: { icon: "🔓", label: "kijelentkezve" },
  unknown: { icon: "·", label: "ismeretlen állapot" },
};

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
    // this stays cheap.
    const signature = `${card.id}|${card.title}|${card.size}|${card.status}|${card.pinned}|${themeHost}`;
    if (root.dataset.signature === signature) return;
    root.dataset.signature = signature;

    const style = document.createElement("style");
    style.textContent = cardStyles();

    const article = document.createElement("article");
    article.className = "card";
    article.dataset.size = card.size || "m";
    article.tabIndex = -1;
    article.setAttribute("role", "listitem");
    article.setAttribute("aria-label", `${card.title} — ${card.domain} — ${status.label}`);
    article.dataset.cardId = card.id;

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

    const badge = document.createElement("span");
    badge.className = "badge";
    badge.textContent = `${card.pinned ? "📌 " : ""}${card.archived ? "💤 " : ""}${status.icon}`;
    badge.title = status.label;

    meta.append(dot, domain, badge);
    body.append(title, meta);

    if (card.notes) {
      const notes = document.createElement("div");
      notes.className = "card-meta";
      notes.style.opacity = "0.75";
      notes.textContent = card.notes;
      body.append(notes);
    }

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

    this.cards = await this.bridge.cards.list();
    this.applyFilter();

    this.bindEvents();
    this.restoreTheme();
    await this.refreshSystem();
  }

  bindEvents() {
    window.addEventListener("resize", () => this.onResize(), { passive: true });
    this.viewport.addEventListener("scroll", () => this.renderWindow(), { passive: true });

    let timer = 0;
    this.searchEl.addEventListener("input", () => {
      window.clearTimeout(timer);
      timer = window.setTimeout(async () => {
        this.searchTerm = this.searchEl.value.trim();
        this.cards = this.searchTerm
          ? await this.bridge.search(this.searchTerm)
          : await this.bridge.cards.list();
        this.applyFilter();
      }, 80);
    });

    document.querySelectorAll("[data-theme-set]").forEach((button) => {
      button.addEventListener("click", () => this.setTheme(button.dataset.themeSet));
    });

    document.addEventListener("keydown", (event) => this.onKeydown(event));

    this.viewport.addEventListener("click", (event) => {
      const card = event.target.closest(".card, antikolt-card");
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
        this.memEl.textContent = `${used} / ${budget} MB`;
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

function withNativeOpen(mock) {
  return {
    ...mock,
    cards: {
      ...mock.cards,
      open: async (id, options) => {
        // The card's own URL and title travel with the call: the shell may not know this card yet,
        // and the page is the only place the list currently lives.
        const cards = await mock.cards.list();
        const card = cards.find((entry) => entry.id === id) || {};
        try {
          await window.antikoltNative.openCard({
            ...(options || {}),
            id,
            url: card.url,
            title: card.title,
          });
        } catch (error) {
          console.warn("openCard failed", error);
        }
      },
    },
    system: {
      ...mock.system,
      engine: () => Promise.resolve("CEF (beágyazott kártya)"),
    },
  };
}

const bridge = window.antikolt ?? (window.antikoltNative
  ? withNativeOpen(mockBridge)
  : mockBridge);
const page = new HomePage(bridge);
page.start().catch((error) => {
  console.error("home page failed to start", error);
});
