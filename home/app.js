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

// The containers the product ships with. The core seeds the same four (ADR-052) and its table replaces this one
// at start-up; the constant stays as the fallback for a page opened without a core (a plain browser).
const PAGE_CONTAINERS = [
  { id: "c-work", name: "Munka", color: "#2F6FEB" },
  { id: "c-private", name: "Magán", color: "#2E9E5B" },
  { id: "c-money", name: "Pénz", color: "#E3A008" },
  { id: "c-dev", name: "Fejlesztés", color: "#8E44AD" },
];

// The table the page draws from. It starts as the shipped default and is replaced by the core's table at
// start-up (ADR-052): a rename or a new container lives in the core, and the page only draws what it is told.
let pageContainers = PAGE_CONTAINERS;
// The workspaces the product ships with. Like the containers, the core seeds the same three and its table
// replaces this one at start-up (ADR-056); the constant is the fallback for a page opened without a core.
const PAGE_WORKSPACES = [
  { id: "w-home", name: "Otthon" },
  { id: "w-work", name: "Munka" },
  { id: "w-read", name: "Olvasnivaló" },
];

// The table the tab row draws from. It starts as the shipped default and is replaced by the core's table, or by
// the shell's push — whichever arrives first.
let pageWorkspaces = PAGE_WORKSPACES;

/** A new card's id: a 26-character ULID-shaped string, so the core's ordering stays lexicographic by creation. */
function newCardId() {
  const alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  const stamp = Date.now();
  let out = "";
  // 10 characters of time (base32 of milliseconds), 16 of randomness: unique enough for a personal card list, and a
  // shape the store already uses for its seed cards.
  for (let shift = 45; shift >= 0; shift -= 5) {
    out += alphabet[(Math.floor(stamp / 2 ** shift) & 31) % 32];
  }
  for (let index = 0; index < 16; index += 1) {
    out += alphabet[Math.floor(Math.random() * 32)];
  }
  return out.slice(0, 26);
}

/** The bridge contract. Every method here must exist in the real shell's injection. */
function createMockBridge() {
  const now = Date.now();
  const day = 24 * 60 * 60 * 1000;

  const containers = pageContainers;
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
      // The sample cards a browser-only page shows: the workspace is part of the sample data, stated as such,
      // instead of being derived from the card's position in the list.
      workspaceId: workspaces[index % workspaces.length].id,
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
    pageContainers.find((entry) => entry.id === containerId) ??
    pageContainers.find((entry) => entry.name === containerId) ??
    pageContainers[1];
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
    // The page's own text, captured when the card was frozen (ADR-049). Empty for a card that was never frozen;
    // the search treats it as one more field to look at, never as a preview of the article.
    pageText: String(row.page_text ?? ""),
    // Free-form tags the user typed (ADR-055). The search has always promised "címke"; this is the field that
    // makes the promise true, and it travels to the core with every write.
    tags: Array.isArray(row.tags) ? row.tags.map((tag) => String(tag)).filter(Boolean) : [],
    // The workspace is a stored field (ADR-056), not an index the page invents: `index > 12 ? "w-read" : "w-home"`
    // put every thirteenth card into "Olvasnivaló" whether the user asked for it or not. An empty field means the
    // first workspace — what every card written before the table existed means.
    workspaceId: String(row.workspace ?? "") || (PAGE_WORKSPACES[0]?.id ?? "w-home"),
  };
}

/** The fold a Hungarian search is expected to do: `Kávé` is found by typing `kave`.
 *
 * Two steps, both standard: lowercase, then strip the diacritics through Unicode decomposition (`ő` becomes
 * `o` + a combining double acute, which is removed with the rest of the combining marks). The handful of letters
 * that do *not* decompose (`ł`, `ø`, `đ`, `ß`) get an explicit map, because a search box that works for Hungarian
 * and fails for a Polish author's name is a bug waiting for the first issue report.
 */
function foldSearch(text) {
  return String(text || "")
    .toLowerCase()
    .normalize("NFD")
    .replace(/[\u0300-\u036f]/g, "")
    .replace(/ł/g, "l")
    .replace(/ø/g, "o")
    .replace(/đ/g, "d")
    .replace(/ß/g, "ss");
}

/** The page's own search rule (title, URL, note, tag, and the captured page text) — accent-insensitive, because
 * typing `kave` is what a Hungarian user actually does. The page text comes from the freeze (`card_page_text`,
 * ADR-049): a card that was never frozen has none, so this is a bonus field, not a promise about every card.
 */
function matchesTerm(card, term) {
  const query = foldSearch(term).trim();
  if (!query) return true;
  return foldSearch(
    [card.title, card.url, card.notes, card.pageText, ...(card.tags || [])].join(" "),
  ).includes(query);
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
      // A note save re-renders the list — and the core's push re-renders it *again* — so the focus is re-applied on
      // every rebuild until the user does something else (the save handler releases the id on the next pointer or
      // key event). A timeout here was the bug: it could expire before the push, and the focus then fell to `body`.
      requestAnimationFrame(() => {
        if (article.isConnected) article.focus();
      });
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
    // Renaming in place (ADR-062): a double click turns the title into an input, Enter saves, Esc cancels — the same
    // gesture the container legend and the workspace tabs already use, and no dialog (ADR-053).
    title.title = "Dupla kattintás: átnevezés";
    title.addEventListener("dblclick", (event) => {
      event.preventDefault();
      event.stopPropagation();
      const input = document.createElement("input");
      input.type = "text";
      input.className = "card-title-input";
      input.value = card.title || "";
      input.setAttribute("aria-label", "A kártya címe");
      const finish = (value, save) => {
        const next = String(value ?? "").trim();
        const wanted = save && next && next !== card.title;
        // The write happens *before* the DOM is put back: restoring the title first made the browser fire `blur`,
        // whose own `finish("", false)` ran re-entrantly and could throw before the save — the editor then looked
        // like it silently did nothing (ADR-062, measured by `--addcardtest`'s `wrote_by`/`editor` pair).
        if (wanted) {
          const detail = { id: this.card?.id, title: next };
          window.__lastRename = JSON.stringify(detail);
          if (typeof window.antikoltRenameCard === "function") {
            window.antikoltRenameCard(detail.id, detail.title);
          } else {
            this.dispatchEvent(
              new CustomEvent("antikolt-card-title", { detail, bubbles: true, composed: true }),
            );
          }
        }
        try {
          if (input.parentNode) {
            input.replaceWith(title);
          }
        } catch (error) {
          console.warn("a címszerkesztő nem tudta visszaállítani a címet", error);
        }
      };
      input.addEventListener("keydown", (keyEvent) => {
        if (keyEvent.key === "Enter") {
          keyEvent.preventDefault();
          finish(input.value, true);
        } else if (keyEvent.key === "Escape") {
          keyEvent.preventDefault();
          finish("", false);
        }
      });
      input.addEventListener("blur", () => finish("", false));
      title.replaceWith(input);
      input.focus();
      input.select();
    });

    const meta = document.createElement("div");
    meta.className = "card-meta";

    const dot = document.createElement("span");
    dot.className = "dot";
    dot.style.background = card.container?.color ?? "var(--text-muted)";
    dot.title = card.container?.name ?? "";

    // The card↔container binding is switchable from the card itself (roadmap M2). A native `<select>` keeps it
    // keyboard- and screen-reader-friendly, and it shows the container's name, not just a colour — a dot alone
    // cannot be read out. The write goes through the shell like every other write, so the grid shows only what
    // the core accepted.
    const containerSelect = document.createElement("select");
    containerSelect.className = "container-select";
    containerSelect.setAttribute("aria-label", "Konténer");
    containerSelect.title = card.container?.name ?? "";
    for (const entry of pageContainers) {
      const option = document.createElement("option");
      option.value = entry.id;
      option.textContent = entry.name;
      if (card.container && entry.id === card.container.id) {
        option.selected = true;
      }
      containerSelect.append(option);
    }
    containerSelect.addEventListener("change", (event) => {
      event.stopPropagation();
      this.dispatchEvent(
        new CustomEvent("antikolt-container", {
          detail: { id: this.card?.id, container: containerSelect.value },
          bubbles: true,
          composed: true,
        }),
      );
    });

    // The workspace is a second, narrower selector next to the container (ADR-058). Two selects look busy, but the
    // alternative — a card that can only change workspace through a drag gesture that does not exist yet — would
    // be a setting the user cannot reach. Both are plain `<select>`s, so keyboard and screen readers already work.
    const workspaceSelect = document.createElement("select");
    workspaceSelect.className = "workspace-select";
    workspaceSelect.setAttribute("aria-label", "Munkaterület");
    workspaceSelect.title = "Munkaterület";
    for (const entry of pageWorkspaces) {
      const option = document.createElement("option");
      option.value = entry.id;
      option.textContent = entry.name;
      if (entry.id === card.workspaceId) {
        option.selected = true;
      }
      workspaceSelect.append(option);
    }
    workspaceSelect.addEventListener("change", (event) => {
      event.stopPropagation();
      this.dispatchEvent(
        new CustomEvent("antikolt-workspace", {
          detail: { id: this.card?.id, workspace: workspaceSelect.value },
          bubbles: true,
          composed: true,
        }),
      );
    });

    const domain = document.createElement("span");
    domain.className = "card-domain";
    domain.textContent = card.domain;
    // The URL is editable the same way the title is (ADR-063): a double click opens the address, Enter saves it. The
    // card's *target* is the one field where a typo is silent — the card keeps opening the old page — so it is worth
    // the same in-place editor.
    domain.title = "Dupla kattintás: webcím szerkesztése";
    domain.addEventListener("dblclick", (event) => {
      event.preventDefault();
      event.stopPropagation();
      const input = document.createElement("input");
      input.type = "text";
      input.className = "card-domain-input";
      input.value = card.url || "";
      input.setAttribute("aria-label", "A kártya webcíme");
      const restore = () => {
        try {
          if (input.parentNode) {
            input.replaceWith(domain);
          }
        } catch (error) {
          console.warn("a webcím-szerkesztő nem tudta visszaállítani a mezőt", error);
        }
      };
      const finish = (value, save) => {
        const typed = String(value ?? "").trim();
        const wanted = save && typed && typed !== card.url;
        // Write first, restore second — the same order the title editor had to learn (ADR-062): restoring the node
        // fires `blur`, and a re-entrant cancel must not swallow the save the user asked for.
        if (wanted) {
          const next = /^https?:\/\//i.test(typed) ? typed : `https://${typed}`;
          const detail = { id: this.card?.id, url: next };
          window.__lastUrlEdit = JSON.stringify(detail);
          if (typeof window.antikoltCardUrl === "function") {
            window.antikoltCardUrl(detail.id, detail.url);
          }
        }
        restore();
      };
      input.addEventListener("keydown", (keyEvent) => {
        if (keyEvent.key === "Enter") {
          keyEvent.preventDefault();
          finish(input.value, true);
        } else if (keyEvent.key === "Escape") {
          keyEvent.preventDefault();
          finish("", false);
        }
      });
      input.addEventListener("blur", () => finish("", false));
      domain.replaceWith(input);
      input.focus();
      input.select();
    });

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
      // The id is kept until the *user* does something else, not until a timeout: the list is rebuilt twice on a
      // save (the page's own render, then the core's push), and the push can arrive later than any guessed delay.
      // A timeout that expired first left the focus on `body` (measured: `note_focus=not-host:BODY`), which is
      // exactly the accessibility promise this feature is about.
      const release = () => {
        pendingFocusId = null;
        document.removeEventListener("pointerdown", release, true);
        document.removeEventListener("keydown", release, true);
      };
      document.addEventListener("pointerdown", release, true);
      document.addEventListener("keydown", release, true);
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

    // Deleting is two-step on purpose (ADR-068): a single click on a `×` inside a card the user clicks to *open* is
    // how a page loses a card by accident. The first click arms the card (the button says "Biztos?"), the second
    // within five seconds deletes it; anything else disarms.
    const removeButton = document.createElement("button");
    removeButton.type = "button";
    removeButton.className = "badge remove";
    removeButton.textContent = this.armedDelete?.id === card.id ? "Biztos?" : "×";
    removeButton.dataset.armed = this.armedDelete?.id === card.id ? "1" : "0";
    removeButton.title =
      this.armedDelete?.id === card.id
        ? "Kattints újra a törléshez (5 másodpercig)"
        : "Kártya törlése (két kattintás)";
    removeButton.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      this.dispatchEvent(
        new CustomEvent("antikolt-card-delete", {
          detail: { id: this.card?.id },
          bubbles: true,
          composed: true,
        }),
      );
    });

    meta.append(dot, containerSelect, workspaceSelect, domain, noteButton, badge, removeButton);
    body.append(title, meta);
    // Tags (ADR-055): the chips a user typed, plus the inline way to add one. No dialog and no separate screen:
    // a tag is a word, and typing it where it belongs is the whole interaction.
    const tagsRow = document.createElement("div");
    tagsRow.className = "card-tags";
    for (const tag of card.tags ?? []) {
      const chip = document.createElement("span");
      chip.className = "tag";
      chip.dataset.tag = tag;
      const text = document.createElement("span");
      text.textContent = tag;
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "tag-remove";
      remove.setAttribute("aria-label", `${tag} címke törlése`);
      remove.textContent = "×";
      remove.addEventListener("click", (event) => {
        event.preventDefault();
        event.stopPropagation();
        this.dispatchEvent(
          new CustomEvent("antikolt-tags", {
            detail: { id: this.card?.id, tags: (this.card?.tags ?? []).filter((entry) => entry !== tag) },
            bubbles: true,
            composed: true,
          }),
        );
      });
      chip.append(text, remove);
      tagsRow.append(chip);
    }
    const addTag = document.createElement("button");
    addTag.type = "button";
    addTag.className = "tag-add";
    addTag.textContent = "＋címke";
    addTag.title = "Címke hozzáadása";
    addTag.addEventListener("click", (event) => {
      event.preventDefault();
      event.stopPropagation();
      const input = document.createElement("input");
      input.type = "text";
      input.className = "tag-input";
      input.setAttribute("aria-label", "Új címke");
      input.placeholder = "címke";
      const finish = (value, keep) => {
        const tags = keep
          ? [...(this.card?.tags ?? []), String(value ?? "").trim()].filter(Boolean)
          : this.card?.tags ?? [];
        this.dispatchEvent(
          new CustomEvent("antikolt-tags", {
            detail: { id: this.card?.id, tags },
            bubbles: true,
            composed: true,
          }),
        );
      };
      input.addEventListener("keydown", (keyEvent) => {
        if (keyEvent.key === "Enter") {
          keyEvent.preventDefault();
          finish(input.value, true);
        } else if (keyEvent.key === "Escape") {
          keyEvent.preventDefault();
          finish("", false);
        }
      });
      input.addEventListener("blur", () => finish("", false));
      addTag.replaceWith(input);
      input.focus();
    });
    tagsRow.append(addTag);
    body.append(tagsRow);


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
    this.legendEl = document.getElementById("container-legend");
    // The container filter is a *view* state: it is not stored, because a filter the user forgot about is a
    // trap ("where are my cards?"). A fresh start shows everything.
    this.containerFilter = null;
    // Set when the core refuses a delete because cards still live in the container: the legend then offers the
    // move instead of a dead end (ADR-054).
    this.containerMove = null;
    this.lastContainerError = "";
    // The last tag write, for the witness a self test reads (ADR-055).
    this.lastTags = null;
    // The last workspace write (ADR-056), same reason.
    this.lastWorkspace = null;
    // The workspace editor and its error: an inline form on the tab row, never a dialog (ADR-053/ADR-057).
    this.workspaceEdit = null;
    this.lastWorkspaceError = "";
    // The add-card editor (ADR-061): the same shape — an inline form, no dialog, and an error place of its own.
    this.cardAdd = null;
    this.lastCardError = "";
    // The armed `×` of the two-step delete (ADR-068): `{id, at}` while a card waits for its second click.
    this.armedDelete = null;
  }


  async start() {
    // The tab row comes from the core too (ADR-056). A core that does not answer leaves the shipped table, but a
    // core that answers with nothing is a lie to act on: the first workspace is then the fallback, not "no tabs".
    try {
      const table = await this.bridge.workspaces.list();
      if (Array.isArray(table) && table.length) {
        pageWorkspaces = table;
      }
    } catch (error) {
      console.warn("a munkaterület-tábla nem jött át", error);
    }
    this.workspaces = pageWorkspaces;
    this.activeWorkspace = this.workspaces[0]?.id ?? null;
    this.renderWorkspaces();

    // The container table comes from the core (ADR-052), before the cards are mapped: a card's colour and name
    // are looked up through it. A core that does not answer leaves the shipped default in place, because a grid
    // without colours is worse than a grid with the wrong palette.
    try {
      const table = await this.bridge.containers.list();
      if (Array.isArray(table) && table.length) {
        pageContainers = table;
      }
    } catch (error) {
      console.warn("a konténer-tábla nem jött át", error);
    }
    this.containers = pageContainers;

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
   * The core's container table arrived (ADR-052): re-map the cards through it and redraw.
   *
   * The mapping is not idempotent by accident: a card's container becomes an *object* (colour + name), so a new
   * table must rebuild the models, not just the legend. The core's own records are kept untouched, because they
   * only carry the id.
   */
  acceptContainers() {
    if (this.coreCards) {
      this.allCards = this.coreCards.map(pageCardFromCore);
      this.cards = this.allCards.slice();
      cardLookup.rows = this.allCards;
    }
    this.applyFilter();
  }

  /**
   * The core's workspace table arrived (ADR-056): redraw the tabs and re-map the cards through it.
   *
   * The card's workspace comes from its record, so a new table does not change an assignment — but the tab labels
   * and the active tab must follow the table, or a renamed workspace would keep its old name in the row.
   */
  acceptWorkspaces() {
    this.workspaces = pageWorkspaces;
    this.renderWorkspaces();
    if (this.coreCards) {
      this.allCards = this.coreCards.map(pageCardFromCore);
      this.cards = this.allCards.slice();
      cardLookup.rows = this.allCards;
    }
    this.applyFilter();
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
   * Move one card to another workspace through the shell (`cards.upsert`), then re-render from the answer
   * (ADR-056). Same rule as the container switch: the grid shows what the core accepted, and the view's
   * `workspaceId` follows the answer, not the request.
   */
  async setWorkspace(id, workspaceId) {
    if (!id || !workspaceId) return;
    const model = this.allCards.find((card) => card.id === id);
    const record = this.coreRowById?.get(id);
    if (!model || !record) {
      this.lastWorkspace = { id, workspace: "", error: "nincs mag-rekord ehhez a kártyához" };
      return;
    }
    if (typeof this.bridge?.cards?.upsert !== "function") {
      this.lastWorkspace = { id, workspace: "", error: "a híd nem tud írni (nincs motor)" };
      return;
    }
    const next = { ...record, workspace: String(workspaceId) };
    try {
      const answer = await this.bridge.cards.upsert(next);
      const saved = answer?.card ?? next;
      const stored = this.coreRowById.get(id);
      if (stored) stored.workspace = String(saved.workspace ?? "");
      model.workspaceId = String(saved.workspace ?? "") || model.workspaceId;
      this.lastWorkspace = { id, workspace: model.workspaceId, error: "" };
      this.windowEl.dataset.signature = "";
      this.applyFilter();
    } catch (error) {
      this.lastWorkspace = { id, workspace: "", error: String(error?.message ?? error) };
      throw error;
    }
  }

  /**
   * Move one card to another container through the shell (`cards.upsert`), then re-render from the answer.
   *
   * Same rule as the pin and the note: the grid shows what the core accepted. The *view* keeps a container
   * object (colour and name for the dot), while the record carries the id, so the answer is translated back
   * through the same table the cards were read with.
   */
  async setContainer(id, containerId) {
    if (!id || !containerId) return;
    const model = this.allCards.find((card) => card.id === id);
    const record = this.coreRowById?.get(id);
    if (!model || !record) {
      this.lastContainer = { id, container: "", error: "nincs mag-rekord ehhez a kártyához" };
      return;
    }
    if (typeof this.bridge?.cards?.upsert !== "function") {
      this.lastContainer = { id, container: "", error: "a híd nem tud írni (nincs motor)" };
      return;
    }
    const next = { ...record, container: String(containerId) };
    try {
      const answer = await this.bridge.cards.upsert(next);
      const saved = answer?.card ?? next;
      const stored = this.coreRowById.get(id);
      if (stored) stored.container = String(saved.container ?? "");
      model.container =
        pageContainers.find((entry) => entry.id === String(saved.container ?? "")) ??
        model.container;
      this.lastContainer = { id, container: String(saved.container ?? ""), error: "" };
      this.windowEl.dataset.signature = "";
      this.applyFilter();
    } catch (error) {
      this.lastContainer = { id, container: "", error: String(error?.message ?? error) };
    }
  }

  /**
   * Save one card's note through the shell (`cards.upsert`), then re-render from the answer.
   *
   * Same rule as the pin: the grid shows what the core accepted, so a failed write cannot leave the card
   * claiming a note the store does not hold.
   */
  /**
   * Save one card's tags through the shell (`cards.upsert`), then re-render from the answer (ADR-055).
   *
   * Same rule as the pin and the note: the grid shows what the core accepted. The core normalises the list
   * (trim, de-duplicate, cap), so the answer — not the typed text — is what the chip row shows.
   */
  async setTags(id, tags) {
    if (!id) return;
    const model = this.allCards.find((card) => card.id === id);
    const record = this.coreRowById?.get(id);
    if (!model || !record) {
      this.lastTags = { id, tags: [], error: "nincs mag-rekord ehhez a kártyához" };
      return;
    }
    if (typeof this.bridge?.cards?.upsert !== "function") {
      this.lastTags = { id, tags: [], error: "a híd nem tud írni (nincs motor)" };
      return;
    }
    const next = { ...record, tags: Array.isArray(tags) ? tags : [] };
    try {
      const answer = await this.bridge.cards.upsert(next);
      const saved = answer?.card ?? next;
      const stored = this.coreRowById.get(id);
      if (stored) stored.tags = Array.isArray(saved.tags) ? saved.tags : [];
      model.tags = Array.isArray(saved.tags) ? saved.tags : [];
      this.lastTags = { id, tags: model.tags, error: "" };
      this.windowEl.dataset.signature = "";
      this.applyFilter();
    } catch (error) {
      this.lastTags = { id, tags: [], error: String(error?.message ?? error) };
    }
  }

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
    document.addEventListener("antikolt-container", (event) =>
      this.setContainer(event.detail?.id, event.detail?.container),
    );
    document.addEventListener("antikolt-tags", (event) =>
      this.setTags(event.detail?.id, event.detail?.tags),
    );
    document.addEventListener("antikolt-card-title", (event) =>
      this.setCardTitle(event.detail?.id, event.detail?.title),
    );
    document.addEventListener("antikolt-card-delete", (event) =>
      this.requestDelete(event.detail?.id),
    );
    window.addEventListener("antikolt-card-title", (event) =>
      this.setCardTitle(event.detail?.id, event.detail?.title),
    );
    document.addEventListener("antikolt-workspace", (event) =>
      this.setWorkspace(event.detail?.id, event.detail?.workspace),
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

  /**
   * The add-card editor (ADR-061): a URL and an optional title, in an inline form above the grid.
   *
   * The card is written through the same `cards.upsert` every other change uses, into the active workspace and the
   * first container — the two facts a new card has to have, and the two a form cannot ask without a third field.
   */
  beginCardAdd() {
    this.cardAdd = { url: "", title: "" };
    this.lastCardError = "";
    this.renderCardAdd();
  }

  renderCardAdd() {
    const host = document.getElementById("card-add-form");
    if (!host) return;
    host.replaceChildren();
    if (!this.cardAdd) {
      host.hidden = true;
      return;
    }
    host.hidden = false;
    const form = document.createElement("form");
    form.className = "card-add-form";
    const url = document.createElement("input");
    url.type = "text";
    url.className = "card-add-url";
    url.placeholder = "https://…";
    url.setAttribute("aria-label", "A kártya webcíme");
    const title = document.createElement("input");
    title.type = "text";
    title.className = "card-add-title";
    title.placeholder = "Cím (elhagyható — a webcím lesz a cím)";
    title.setAttribute("aria-label", "A kártya címe");
    const hint = document.createElement("span");
    hint.className = "card-add-hint";
    hint.textContent = "Enter: mentés · Esc: mégse";
    form.append(url, title, hint);
    if (this.lastCardError) {
      const problem = document.createElement("span");
      problem.className = "card-add-error";
      problem.setAttribute("role", "status");
      problem.textContent = this.lastCardError;
      form.append(problem);
    }
    form.addEventListener("submit", (event) => {
      event.preventDefault();
      this.commitCardAdd(url.value, title.value);
    });
    form.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        this.cardAdd = null;
        this.renderCardAdd();
      }
    });
    host.append(form);
    requestAnimationFrame(() => url.focus());
  }

  /** Save the typed card. The URL is the only required field; a title falls back to the host. */
  async commitCardAdd(rawUrl, rawTitle) {
    const url = String(rawUrl ?? "").trim();
    const title = String(rawTitle ?? "").trim();
    if (!url) {
      this.cardAdd = null;
      this.renderCardAdd();
      return;
    }
    const normalized = /^https?:\/\//i.test(url) ? url : `https://${url}`;
    const domain = normalized.replace(/^https?:\/\//i, "").split("/")[0];
    const model = {
      id: newCardId(),
      title: title || domain,
      url: normalized,
      domain,
      containerId: pageContainers[0]?.id ?? "",
      workspaceId: this.activeWorkspace ?? pageWorkspaces[0]?.id ?? "",
      tags: [],
      pinned: false,
      notes: "",
    };
    const record = {
      id: model.id,
      title: model.title,
      url: model.url,
      container: model.containerId,
      status: "unknown",
      pinned: false,
      notes: "",
      preview_ref: "",
      last_opened_at_ms: 0,
      page_text: "",
      tags: [],
      workspace: model.workspaceId,
    };
    if (typeof this.bridge?.cards?.upsert !== "function") {
      this.lastCardError = "a híd nem tud írni (nincs motor)";
      this.renderCardAdd();
      return;
    }
    try {
      const answer = await this.bridge.cards.upsert(record);
      const saved = answer?.card ?? record;
      this.cardAdd = null;
      this.lastCardError = "";
      this.coreRowById.set(saved.id, saved);
      this.coreCards.push(saved);
      this.allCards = this.coreCards.map(pageCardFromCore);
      this.cards = this.allCards.slice();
      this.windowEl.dataset.signature = "";
      this.applyFilter();
      this.renderCardAdd();
    } catch (error) {
      this.lastCardError = String(error?.message ?? error);
      this.renderCardAdd();
    }
  }

  /**
   * Rename one card through the shell (`cards.upsert`), then re-render from the answer (ADR-062).
   *
   * The record travels whole — the title is one field of it — so the core keeps the container, the tags, the note and
   * the capture, and the grid draws what came back.
   */
  async setCardTitle(id, title) {
    const wanted = String(title ?? "").trim();
    const model = this.allCards.find((card) => card.id === id);
    const record = this.coreRowById?.get(id);
    if (!model || !record || !wanted) {
      this.lastCardError = "nincs mit átnevezni";
      return;
    }
    if (typeof this.bridge?.cards?.upsert !== "function") {
      this.lastCardError = "a híd nem tud írni (nincs motor)";
      return;
    }
    try {
      const answer = await this.bridge.cards.upsert({ ...record, title: wanted });
      const saved = answer?.card ?? record;
      this.coreRowById.set(id, saved);
      this.coreCards = this.coreCards.map((row) => (row.id === id ? saved : row));
      this.allCards = this.coreCards.map(pageCardFromCore);
      this.cards = this.allCards.slice();
      this.lastCardError = "";
      this.windowEl.dataset.signature = "";
      this.applyFilter();
    } catch (error) {
      this.lastCardError = String(error?.message ?? error);
    }
  }

  /**
   * Change one card's URL through the shell (`cards.upsert`), then re-render from the answer (ADR-063).
   *
   * The domain the card shows is derived from the URL everywhere else (the add form does the same), so this only
   * writes the URL and lets `pageCardFromCore` recompute the rest from the core's answer.
   */
  async setCardUrl(id, url) {
    const wanted = String(url ?? "").trim();
    const record = this.coreRowById?.get(id);
    if (!record || !wanted) {
      this.lastCardError = "nincs mit átírni";
      return;
    }
    if (typeof this.bridge?.cards?.upsert !== "function") {
      this.lastCardError = "a híd nem tud írni (nincs motor)";
      return;
    }
    try {
      const answer = await this.bridge.cards.upsert({ ...record, url: wanted });
      const saved = answer?.card ?? record;
      this.coreRowById.set(id, saved);
      this.coreCards = this.coreCards.map((row) => (row.id === id ? saved : row));
      this.allCards = this.coreCards.map(pageCardFromCore);
      this.cards = this.allCards.slice();
      this.lastCardError = "";
      this.windowEl.dataset.signature = "";
      this.applyFilter();
    } catch (error) {
      this.lastCardError = String(error?.message ?? error);
    }
  }

  /**
   * The two-step delete (ADR-068): the first `×` arms the card, the second deletes it.
   *
   * A card is a big click target that *opens* a page, and the `×` sits inside it. One stray click must not destroy
   * anything, so the first click only arms — and the arm expires by itself, because a card left armed for a minute
   * is a trap for the next stray click.
   */
  requestDelete(id) {
    if (!id) {
      return;
    }
    const at = Date.now();
    if (this.armedDelete?.id === id && at - this.armedDelete.at < 5000) {
      this.armedDelete = null;
      this.deleteCard(id);
      return;
    }
    this.armedDelete = { id, at };
    this.lastCardError = "";
    this.renderWorkspaces();
    // The grid draws the armed state from the model, so re-render the visible window.
    this.windowEl.dataset.signature = "";
    this.applyFilter();
    window.setTimeout(() => {
      if (this.armedDelete?.id === id) {
        this.armedDelete = null;
        this.windowEl.dataset.signature = "";
        this.applyFilter();
      }
    }, 5000);
  }

  /** Remove one card (`cards.delete`), after the core accepted it (ADR-061). */
  async deleteCard(id) {
    if (!id || typeof this.bridge?.cards?.delete !== "function") {
      this.lastCardError = "a híd nem tud írni (nincs motor)";
      return;
    }
    try {
      await this.bridge.cards.delete(id);
      this.coreRowById.delete(id);
      this.coreCards = this.coreCards.filter((row) => row.id !== id);
      this.allCards = this.coreCards.map(pageCardFromCore);
      this.cards = this.allCards.slice();
      this.windowEl.dataset.signature = "";
      this.applyFilter();
    } catch (error) {
      this.lastCardError = String(error?.message ?? error);
    }
  }

  renderWorkspaces() {
    const nav = document.getElementById("workspaces");
    this.workspacesEl = nav;
    nav.replaceChildren();
    // The add-card button lives outside the tab row (it is not a tab), but it is wired here because the render is
    // the one place that knows the page is ready (ADR-061).
    const addCard = document.getElementById("card-add");
    if (addCard && !addCard.dataset.wired) {
      addCard.dataset.wired = "1";
      addCard.addEventListener("click", () => this.beginCardAdd());
    }
    for (const workspace of this.workspaces) {
      const tab = document.createElement("span");
      tab.className = "ws-tab";
      tab.dataset.workspace = workspace.id;
      const button = document.createElement("button");
      button.type = "button";
      button.className = "ws-pick";
      button.dataset.workspace = workspace.id;
      button.setAttribute("role", "tab");
      button.setAttribute("aria-selected", String(workspace.id === this.activeWorkspace));
      button.textContent = workspace.name;
      button.addEventListener("click", () => {
        this.activeWorkspace = workspace.id;
        this.renderWorkspaces();
        this.applyFilter();
      });
      // Rename: double click on the tab, the same way the container legend works (ADR-053/ADR-057).
      button.addEventListener("dblclick", (event) => {
        event.preventDefault();
        this.beginWorkspaceEdit({ mode: "rename", id: workspace.id, name: workspace.name });
      });
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "ws-remove";
      remove.dataset.workspace = workspace.id;
      remove.setAttribute("aria-label", `${workspace.name} törlése`);
      remove.title = "Munkaterület törlése (csak ha üres)";
      remove.textContent = "×";
      remove.addEventListener("click", (event) => {
        event.stopPropagation();
        this.deleteWorkspace(workspace.id);
      });
      tab.append(button, remove);
      nav.append(tab);
    }
    // The editor is one element for both jobs (add and rename): two inputs would be two places to keep in sync.
    if (this.workspaceEdit) {
      const form = document.createElement("form");
      form.className = "ws-editor";
      form.noValidate = true;
      const input = document.createElement("input");
      input.type = "text";
      input.className = "ws-input";
      input.value = this.workspaceEdit.name ?? "";
      input.placeholder = this.workspaceEdit.mode === "add" ? "Új munkaterület" : "Munkaterület neve";
      input.setAttribute("aria-label", input.placeholder);
      const confirm = document.createElement("span");
      confirm.className = "ws-editor-hint";
      confirm.textContent = "Enter: mentés · Esc: mégse";
      form.append(input, confirm);
      form.addEventListener("submit", (event) => {
        event.preventDefault();
        this.commitWorkspaceEdit(input.value);
      });
      input.addEventListener("keydown", (event) => {
        if (event.key === "Escape") {
          event.preventDefault();
          this.workspaceEdit = null;
          this.renderWorkspaces();
        }
      });
      nav.append(form);
      requestAnimationFrame(() => input.focus());
    } else {
      const add = document.createElement("button");
      add.type = "button";
      add.className = "ws-add";
      add.setAttribute("aria-label", "Új munkaterület");
      add.title = "Új munkaterület";
      add.textContent = "＋";
      add.addEventListener("click", () => this.beginWorkspaceEdit({ mode: "add", name: "" }));
      nav.append(add);
    }
    if (this.lastWorkspaceError) {
      const problem = document.createElement("span");
      problem.className = "ws-error";
      problem.setAttribute("role", "status");
      problem.textContent = this.lastWorkspaceError;
      nav.append(problem);
    }
  }

  beginWorkspaceEdit(edit) {
    this.workspaceEdit = edit;
    this.lastWorkspaceError = "";
    this.renderWorkspaces();
  }

  /// `w-` + a slug of the name, so a new workspace gets an id a person could type. A clash gets a suffix.
  newWorkspaceId(name) {
    const base =
      "w-" +
      (name
        .toLowerCase()
        .normalize("NFD")
        .replace(/[\u0300-\u036f]/g, "")
        .replace(/[^a-z0-9]+/g, "-")
        .replace(/^-+|-+$/g, "")
        .slice(0, 24) || "munkaterulet");
    let candidate = base;
    let suffix = 2;
    while (pageWorkspaces.some((entry) => entry.id === candidate)) {
      candidate = `${base}-${suffix}`;
      suffix += 1;
    }
    return candidate;
  }

  /// Write the table change through the shell, then redraw what the core answered (ADR-056/ADR-057).
  async commitWorkspaceEdit(rawName) {
    const name = String(rawName ?? "").trim();
    const edit = this.workspaceEdit;
    if (!edit || !name) {
      this.workspaceEdit = null;
      this.renderWorkspaces();
      return;
    }
    if (typeof this.bridge?.workspaces?.upsert !== "function") {
      this.lastWorkspaceError = "a híd nem tud írni (nincs motor)";
      this.workspaceEdit = null;
      this.renderWorkspaces();
      return;
    }
    const id = edit.mode === "rename" ? edit.id : this.newWorkspaceId(name);
    try {
      await this.bridge.workspaces.upsert({ id, name });
      this.workspaceEdit = null;
      this.lastWorkspaceError = "";
      await this.reloadWorkspaces();
    } catch (error) {
      this.lastWorkspaceError = String(error?.message ?? error);
      this.workspaceEdit = null;
      this.renderWorkspaces();
    }
  }

  async deleteWorkspace(id) {
    if (typeof this.bridge?.workspaces?.delete !== "function") {
      this.lastWorkspaceError = "a híd nem tud írni (nincs motor)";
      this.renderWorkspaces();
      return;
    }
    try {
      await this.bridge.workspaces.delete(id);
      this.lastWorkspaceError = "";
      await this.reloadWorkspaces();
    } catch (error) {
      // The core refuses a workspace that still holds cards with `in_use`: the message is the product's words.
      this.lastWorkspaceError = String(error?.message ?? error);
      this.renderWorkspaces();
    }
  }

  /// Re-read the table from the core after a write, so the tabs are the core's answer and not an assumption.
  async reloadWorkspaces() {
    try {
      const table = await this.bridge.workspaces.list();
      if (Array.isArray(table) && table.length) {
        pageWorkspaces = table;
      }
    } catch (error) {
      console.warn("a munkaterület-tábla újratöltése nem sikerült", error);
    }
    if (!pageWorkspaces.some((entry) => entry.id === this.activeWorkspace)) {
      this.activeWorkspace = pageWorkspaces[0]?.id ?? null;
    }
    this.acceptWorkspaces();
  }

  /** The container legend: a coloured chip per container with the number of cards the current search shows.
   *
   *  The chips are also the filter. A chip is a button with `aria-pressed`, so the state is announced, and only
   *  one container can be active at a time — a second click on the same chip clears it. The counts follow the
   *  search, not the whole store: a legend that claims "3 cards" while the search hides two of them is a legend
   *  that lies.
   */
  renderContainerLegend() {
    if (!this.legendEl) return;
    const visible = this.cards.filter((card) => !this.activeWorkspace || card.workspaceId === this.activeWorkspace);
    this.legendEl.replaceChildren();
    for (const container of pageContainers) {
      const count = visible.filter(
        (card) => card.container?.id === container.id && matchesTerm(card, this.searchTerm),
      ).length;
      const chip = document.createElement("span");
      chip.className = "legend-chip";
      chip.dataset.container = container.id;
      chip.setAttribute("role", "group");
      chip.title = `${container.name} — ${count} kártya`;
      const pick = document.createElement("button");
      pick.type = "button";
      pick.className = "legend-pick";
      pick.dataset.container = container.id;
      pick.setAttribute("aria-pressed", this.containerFilter === container.id ? "true" : "false");
      pick.title = this.containerFilter === container.id ? "Szűrő törlése" : `${container.name} szűrése`;
      const dot = document.createElement("span");
      dot.className = "dot";
      dot.style.background = container.color;
      const label = document.createElement("span");
      label.textContent = `${container.name} ${count}`;
      pick.append(dot, label);
      pick.addEventListener("click", () => {
        this.containerFilter = this.containerFilter === container.id ? null : container.id;
        this.applyFilter();
      });
      // Rename: double click on the label (or Enter on the pick while holding Alt is too obscure — a double click
      // is the desktop gesture people try). The input that appears is the same one the add button uses.
      pick.addEventListener("dblclick", (event) => {
        event.preventDefault();
        this.beginContainerEdit({ mode: "rename", id: container.id, name: container.name });
      });
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "legend-remove";
      remove.dataset.container = container.id;
      remove.setAttribute("aria-label", `${container.name} törlése`);
      remove.title = "Konténer törlése (csak ha üres)";
      remove.textContent = "×";
      remove.addEventListener("click", (event) => {
        event.stopPropagation();
        this.deleteContainer(container.id);
      });
      chip.append(pick, remove);
      this.legendEl.append(chip);
    }
    // The editor is one element for both jobs (add and rename): two inputs would be two places to keep in sync.
    if (this.containerEdit) {
      const form = document.createElement("form");
      form.className = "legend-editor";
      form.noValidate = true;
      const input = document.createElement("input");
      input.type = "text";
      input.className = "legend-input";
      input.dataset.containerEdit = this.containerEdit.mode;
      input.value = this.containerEdit.name ?? "";
      input.placeholder = this.containerEdit.mode === "add" ? "Új konténer neve" : "Konténer neve";
      input.setAttribute("aria-label", input.placeholder);
      const confirm = document.createElement("span");
      confirm.className = "legend-editor-hint";
      confirm.textContent = "Enter: mentés · Esc: mégse";
      form.append(input, confirm);
      form.addEventListener("submit", (event) => {
        event.preventDefault();
        this.commitContainerEdit(input.value);
      });
      input.addEventListener("keydown", (event) => {
        if (event.key === "Escape") {
          event.preventDefault();
          this.containerEdit = null;
          this.renderContainerLegend();
        }
      });
      this.legendEl.append(form);
      // Focus without stealing it from the grid's keyboard handler: the input is the only place the user can type
      // while it is open.
      requestAnimationFrame(() => input.focus());
    } else {
      const add = document.createElement("button");
      add.type = "button";
      add.className = "legend-add";
      add.setAttribute("aria-label", "Új konténer");
      add.title = "Új konténer";
      add.textContent = "＋ konténer";
      add.addEventListener("click", () => this.beginContainerEdit({ mode: "add", name: "" }));
      this.legendEl.append(add);
    }
    if (this.containerMove) {
      // The refusal is not a dead end: the row says where the cards can go, and one click moves them and deletes
      // the container (ADR-054). The options are the *other* containers, because moving a group into itself is
      // not a move.
      const move = document.createElement("span");
      move.className = "legend-move";
      const text = document.createElement("span");
      text.className = "legend-error";
      text.textContent = this.containerMove.error;
      const select = document.createElement("select");
      select.className = "legend-move-target";
      select.setAttribute("aria-label", "Kártyák új konténere");
      for (const container of pageContainers) {
        if (container.id === this.containerMove.id) continue;
        const option = document.createElement("option");
        option.value = container.id;
        option.textContent = container.name;
        select.append(option);
      }
      const confirm = document.createElement("button");
      confirm.type = "button";
      confirm.className = "legend-move-confirm";
      confirm.textContent = "Áthelyezés és törlés";
      confirm.addEventListener("click", () =>
        this.moveAndDeleteContainer(this.containerMove.id, select.value),
      );
      move.append(text, select, confirm);
      this.legendEl.append(move);
    }
    if (this.lastContainerError) {
      const problem = document.createElement("span");
      problem.className = "legend-error";
      problem.setAttribute("role", "status");
      problem.textContent = this.lastContainerError;
      this.legendEl.append(problem);
    }
  }

  beginContainerEdit(edit) {
    this.containerEdit = edit;
    this.lastContainerError = "";
    this.renderContainerLegend();
  }

  /// Write the container table change through the shell (ADR-052), then draw what the core answered.
  async commitContainerEdit(rawName) {
    const name = String(rawName ?? "").trim();
    const edit = this.containerEdit;
    if (!edit || !name) {
      this.containerEdit = null;
      this.renderContainerLegend();
      return;
    }
    if (typeof this.bridge?.containers?.upsert !== "function") {
      this.lastContainerError = "a híd nem tud írni (nincs motor)";
      this.containerEdit = null;
      this.renderContainerLegend();
      return;
    }
    const id = edit.mode === "rename" ? edit.id : this.newContainerId(name);
    try {
      const saved = await this.bridge.containers.upsert({ id, name, color: "" });
      this.containerEdit = null;
      this.lastContainerError = "";
      await this.reloadContainers(saved);
    } catch (error) {
      this.lastContainerError = String(error?.message ?? error);
      this.containerEdit = null;
      this.renderContainerLegend();
    }
  }

  /// `c-` + a slug of the name, so a new container gets an id a person could type. A clash gets a suffix.
  newContainerId(name) {
    const base =
      "c-" +
      (name
        .toLowerCase()
        .normalize("NFD")
        .replace(/[\u0300-\u036f]/g, "")
        .replace(/[^a-z0-9]+/g, "-")
        .replace(/^-+|-+$/g, "")
        .slice(0, 24) || "kontener");
    let candidate = base;
    let suffix = 2;
    while (pageContainers.some((entry) => entry.id === candidate)) {
      candidate = `${base}-${suffix}`;
      suffix += 1;
    }
    return candidate;
  }

  async deleteContainer(id) {
    if (typeof this.bridge?.containers?.delete !== "function") {
      this.lastContainerError = "a híd nem tud írni (nincs motor)";
      this.renderContainerLegend();
      return;
    }
    try {
      await this.bridge.containers.delete(id);
      this.lastContainerError = "";
      this.containerMove = null;
      if (this.containerFilter === id) {
        this.containerFilter = null;
      }
      await this.reloadContainers();
    } catch (error) {
      const message = String(error?.message ?? error);
      // A container that still holds cards cannot be deleted (ADR-052/054). Instead of only showing the refusal,
      // the legend offers the way out: pick where the cards go, and the same click completes the delete.
      if (message.includes("kártya")) {
        this.containerMove = { id, error: message };
        this.lastContainerError = "";
      } else {
        this.lastContainerError = message;
      }
      this.renderContainerLegend();
    }
  }

  /// Move this container's cards into another one and delete it (ADR-054), through the shell.
  async moveAndDeleteContainer(id, moveTo) {
    if (typeof this.bridge?.containers?.delete !== "function" || !moveTo) {
      return;
    }
    try {
      await this.bridge.containers.delete(id, moveTo);
      this.containerMove = null;
      this.lastContainerError = "";
      if (this.containerFilter === id) {
        this.containerFilter = null;
      }
      await this.reloadContainers();
    } catch (error) {
      this.lastContainerError = String(error?.message ?? error);
      this.containerMove = null;
      this.renderContainerLegend();
    }
  }

  /// Re-read the table from the core after a write, so the legend is the core's answer and not an assumption.
  async reloadContainers(preferred) {
    try {
      const table = await this.bridge.containers.list();
      if (Array.isArray(table) && table.length) {
        pageContainers = table;
        this.containers = table;
      }
    } catch (error) {
      console.warn("a konténer-tábla újratöltése nem sikerült", error);
    }
    if (preferred && preferred.id && pageContainers.some((entry) => entry.id === preferred.id)) {
      this.containers = pageContainers;
    }
    this.acceptContainers();
  }

  applyFilter() {
    const byWorkspace = this.cards.filter(
      (card) => !this.activeWorkspace || card.workspaceId === this.activeWorkspace,
    );
    // The container legend filters too, and it is applied *after* the search so the legend's own counts can be
    // computed from the same pair of rules (search first, then the chosen container).
    this.filtered = this.containerFilter
      ? byWorkspace.filter((card) => card.container?.id === this.containerFilter)
      : byWorkspace;
    this.renderContainerLegend();

    const pinned = this.filtered.filter((card) => card.pinned);
    this.renderPinned(pinned);
    this.pinnedSection.hidden = pinned.length === 0;

    this.layout();
    this.viewport.scrollTop = 0;
    this.renderWindow();

    const total = this.filtered.length;
    this.countEl.textContent = total ? `${total} kártya` : "";
    this.emptyEl.hidden = total !== 0;
    // The empty state names what was searched for: "nothing" is a statement about the *query*, and the user is
    // about to change it.
    this.emptyEl.textContent = this.searchTerm
      ? `Nincs találat erre: „${this.searchTerm}”. Próbálj kevesebb szót, vagy töröld a keresést.`
      : "Nincs a keresésnek megfelelő kártya.";
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
    // A note save asks for the focus back on the card it happened on. The per-element hook (`pendingFocusId`)
    // waits for an animation frame, which a background or occluded window may never deliver — so the grid also
    // restores the focus *synchronously* here, right after the rebuild, where the element is already connected.
    if (pendingFocusId) {
      const host = [...this.windowEl.querySelectorAll("antikolt-card")].find(
        (element) => element.card?.id === pendingFocusId,
      );
      host?.shadowRoot?.querySelector(".card")?.focus({ preventScroll: true });
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
      delete: async (id) => {
        const native = nativeBridge();
        if (!native?.cards?.delete) {
          throw new Error("a híd nem tud törölni (nincs motor)");
        }
        return native.cards.delete(id);
      },
    },
    workspaces: {
      ...mock.workspaces,
      // The tab row lives in the core (ADR-056); a plain browser gets the shipped default from the mock.
      list: async () => {
        const native = nativeBridge();
        if (!native?.request) {
          return mock.workspaces.list();
        }
        try {
          const answer = await native.request("workspaces.list", {});
          const table = Array.isArray(answer?.workspaces) ? answer.workspaces : [];
          return table.length ? table : mock.workspaces.list();
        } catch (error) {
          console.warn("workspaces.list failed", error);
          return mock.workspaces.list();
        }
      },
      upsert: async (workspace) => {
        const native = nativeBridge();
        if (!native?.request) {
          throw new Error("a híd nem tud írni (nincs motor)");
        }
        const answer = await native.request("workspaces.upsert", { workspace });
        return answer?.workspace;
      },
      delete: async (id) => {
        const native = nativeBridge();
        if (!native?.request) {
          throw new Error("a híd nem tud írni (nincs motor)");
        }
        return native.request("workspaces.delete", { id });
      },
    },
    // The database mirror (ADR-064): the page asks the shell, the shell asks the core.
    db: {
      status: async (sync) => {
        const native = nativeBridge();
        if (!native?.request) {
          throw new Error("a híd nem tud kérdezni (nincs motor)");
        }
        return native.request("db.status", { sync: Boolean(sync) });
      },
    },
    // The portable bundle (ADR-059). The path is the caller's: the page's button will own the dialog, so this stays
    // a plain request and the shell never blocks a page on native UI.
    profile: {
      export: async (path) => {
        const native = nativeBridge();
        if (!native?.request) {
          throw new Error("a híd nem tud írni (nincs motor)");
        }
        return native.request("profile.export", { path: String(path) });
      },
      import: async (path) => {
        const native = nativeBridge();
        if (!native?.request) {
          throw new Error("a híd nem tud írni (nincs motor)");
        }
        return native.request("profile.import", { path: String(path) });
      },
    },
    containers: {
      ...mock.containers,
      // The table lives in the core (ADR-052); a plain browser gets the shipped default from the mock.
      list: async () => {
        const native = nativeBridge();
        if (!native?.request) {
          return mock.containers.list();
        }
        try {
          const answer = await native.request("containers.list", {});
          const list = Array.isArray(answer?.containers) ? answer.containers : [];
          return list.length ? list : mock.containers.list();
        } catch (error) {
          console.warn("containers.list failed", error);
          return mock.containers.list();
        }
      },
      upsert: async (container) => {
        const native = nativeBridge();
        if (!native?.request) {
          throw new Error("a híd nem tud írni (nincs motor)");
        }
        const answer = await native.request("containers.upsert", { container });
        return answer?.container;
      },
      delete: async (id, moveTo) => {
        const native = nativeBridge();
        if (!native?.request) {
          throw new Error("a híd nem tud írni (nincs motor)");
        }
        return native.request("containers.delete", moveTo ? { id, moveTo } : { id });
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

// The core's container table, pushed with the cards (ADR-052). It is a push, not a request, because the bridge
// script is installed on load *end* — which can be later than this deferred script — and a legend drawn from the
// fallback table while the core had a different one is exactly the bug this push removes.
window.antikoltAcceptContainers = (payload) => {
  const parsed = typeof payload === "string" ? JSON.parse(payload) : payload;
  const list = Array.isArray(parsed?.containers) ? parsed.containers : [];
  if (list.length) {
    pageContainers = list;
    page.containers = list;
    // The cards were mapped through the old table: map them again, so the dot colours and the selector options
    // are the ones the core just described.
    page.acceptContainers();
  }
  return list.length;
};

// The core's workspace table, pushed with the cards (ADR-056). Like the containers it is a push, not a request:
// the bridge script is installed on load *end*, which can be later than this deferred script.
window.antikoltAcceptWorkspaces = (payload) => {
  const parsed = typeof payload === "string" ? JSON.parse(payload) : payload;
  const list = Array.isArray(parsed?.workspaces) ? parsed.workspaces : [];
  if (list.length) {
    pageWorkspaces = list;
    page.workspaces = list;
    // The active tab may have vanished with the old table: fall back to the first one instead of showing an empty
    // grid for a tab that no longer exists.
    if (!list.some((entry) => entry.id === page.activeWorkspace)) {
      page.activeWorkspace = list[0].id;
    }
    page.acceptWorkspaces();
  }
  return list.length;
};

// The product's own path for a workspace change, for the self test: the same call the (not yet built) drag or
// menu will make. No dialog, no separate screen (ADR-053/ADR-056).
window.antikoltMoveToWorkspace = (cardId, workspaceId) => page.setWorkspace(cardId, workspaceId);

// The card's remove path, the same one a menu item will call (ADR-061).
window.antikoltDeleteCard = (cardId) => page.deleteCard(cardId);

// The two-step delete's own witness (ADR-068): what is armed and what the grid holds.
window.antikoltArmedDelete = () => page.armedDelete ?? null;

// The rename path direct (ADR-062), so a caller (a test, or a future menu item) does not have to fake a double click.
window.antikoltRenameCard = (cardId, title) => page.setCardTitle(cardId, title);

// The URL's own door (ADR-063), the same shape.
window.antikoltCardUrl = (cardId, url) => page.setCardUrl(cardId, url);

// The database mirror's door (ADR-064), for the self test and for a future "where is my data" panel.
window.antikoltDbStatus = (sync) => page.bridge.db.status(sync);

// The add-card form's own witness (ADR-061): what the last add/delete did, and what the grid now holds.
window.antikoltCardState = () => ({
  adding: Boolean(page.cardAdd),
  error: page.lastCardError ?? "",
  count: page.cards.length,
  cards: page.cards.map((card) => ({ id: card.id, title: card.title, workspace: card.workspaceId })),
});

// The bundle's page-side door (ADR-059), for the self test and for the button that will call it: the path is the
// caller's, so no native dialog is involved here.
window.antikoltProfileWrite = (path) => page.bridge.profile.export(path);
window.antikoltProfileRead = (path) => page.bridge.profile.import(path);

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

// The search witness: what the box contains, what the grid shows for it, and what the empty state says. The fold
// is exposed too, so a self test can prove the accent rule directly instead of inferring it from a card list.
window.antikoltSearchFold = (text) => foldSearch(text);
window.antikoltSearchState = () => ({
  term: page.searchTerm,
  count: page.cards.length,
  filtered: page.filtered.length,
  titles: page.cards.map((card) => card.title),
  countText: page.countEl.textContent,
  emptyHidden: page.emptyEl.hidden,
  emptyText: page.emptyEl.textContent,
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

// The core's container table as the page draws it (ADR-052): a self test must be able to see a reload.
// The tag witness (ADR-055): what the last tag write did, and the tags the grid now shows per card.
window.antikoltTagState = () => ({
  lastId: page.lastTags?.id ?? "",
  lastTags: page.lastTags?.tags ?? [],
  lastError: page.lastTags?.error ?? "",
  cards: page.cards.map((card) => ({ id: card.id, tags: card.tags ?? [] })),
});

window.antikoltContainerTable = () => pageContainers.map((entry) => `${entry.id}:${entry.name}`);

// The workspace witness (ADR-056): the table the tab row draws, the active tab, and the workspace of each card.
window.antikoltWorkspaceState = () => ({
  table: pageWorkspaces.map((entry) => `${entry.id}:${entry.name}`),
  active: page.activeWorkspace ?? "",
  lastId: page.lastWorkspace?.id ?? "",
  lastWorkspace: page.lastWorkspace?.workspace ?? "",
  lastError: page.lastWorkspace?.error ?? "",
  editing: page.workspaceEdit ? page.workspaceEdit.mode : "",
  uiError: page.lastWorkspaceError ?? "",
  tabs: [...(page.workspacesEl?.querySelectorAll("[role='tab']") ?? [])].map((tab) => ({
    id: tab.dataset.workspace ?? "",
    label: (tab.querySelector(".ws-pick")?.textContent ?? tab.textContent ?? "").trim(),
    selected: tab.getAttribute("aria-selected") === "true",
    removable: Boolean(tab.querySelector(".ws-remove")),
  })),
  cards: page.cards.map((card) => `${card.id}:${card.workspaceId}`),
  filtered: page.filtered.length,
});

// The container witness: what the last switch did, and the container each shown card now belongs to (id map).
window.antikoltContainerState = () => ({
  lastId: page.lastContainer?.id ?? "",
  lastContainer: page.lastContainer?.container ?? "",
  lastError: page.lastContainer?.error ?? "",
  cards: page.cards.map((card) => `${card.id}:${card.container?.id ?? ""}`),
  // The legend is a filter (ADR-051): what is pressed, and what the grid shows because of it.
  filter: page.containerFilter ?? "",
  filtered: page.filtered.length,
  total: page.cards.length,
  legend: [...(page.legendEl?.querySelectorAll(".legend-chip") ?? [])].map((chip) => ({
    id: chip.dataset.container ?? "",
    pressed: chip.querySelector(".legend-pick")?.getAttribute("aria-pressed") === "true",
    label: (chip.querySelector(".legend-pick")?.textContent ?? chip.textContent ?? "").trim(),
    removable: Boolean(chip.querySelector(".legend-remove")),
  })),
  // The last failed table write (a container that still holds cards, for instance): the user must see the core's
  // own words, and a test must be able to read them.
  lastError: page.lastContainerError ?? "",
  move: page.containerMove ? { id: page.containerMove.id ?? "", error: page.containerMove.error ?? "" } : null,
  editing: page.containerEdit ? page.containerEdit.mode : "",
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
