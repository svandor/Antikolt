// SPDX-License-Identifier: MPL-2.0
//
// `<antikolt-card>` as a standalone package. The grid (app.js) and a user-authored home page both load
// this one file, so the card is defined once. It is deliberately self-contained: its own styles and
// status table, and the workspace/container selectors read from `setOptions` (empty by default), so an
// author's HTML cannot break the cards by accident.

(function installAntikoltCard(global) {
  "use strict";

  // The card's management selects read these; the grid supplies the live tables, a user page may leave them empty.
  let cardContainers = [];
  let cardWorkspaces = [];

  // The card's own strings (i18n, ADR-079). Hungarian is authoritative; `t()` maps to English when the shell
  // pushes `data-language=en`. A missing entry falls back to the Hungarian source.
  const EN = {
    "bejelentkezve": "signed in",
    "hamarosan lejár": "expiring soon",
    "kijelentkezve": "signed out",
    "ismeretlen állapot": "unknown state",
    "A kártya címe": "Card title",
    "A kártya webcíme": "Card URL",
    "Mentés": "Save",
    "Mégse": "Cancel",
    "Biztos?": "Sure?",
    "címke törlése": "tag removal",
    "＋címke": "＋tag",
    "Új címke": "New tag",
    "címke": "tag",
    "Kitűzés levétele": "Unpin",
    "Kitűzés": "Pin",
    "Jegyzet szerkesztése": "Edit note",
    "Jegyzet hozzáadása": "Add note",
    "Jegyzet": "Note",
    "Kattints újra a törléshez (5 másodpercig)": "Click again to delete (5 seconds)",
    "Kártya törlése (két kattintás)": "Delete card (two clicks)",
    "a lap utolsó munkakészlete fagyasztáskor": "the page's last working set at freeze",
    "Címke hozzáadása": "Add tag",
  };
  const t = (hu) => (document.documentElement.dataset.language === "en" && EN[hu] ? EN[hu] : hu);

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
      if (!this.dataset.dragWired) {
        this.dataset.dragWired = "1";
        // A card can be dragged onto a workspace tab (ADR-058): the `<select>` stays the reachable way, the drag
        // is the fast one. The drag must not hijack text selection or a dropdown from the card's own controls.
        this.draggable = true;
        this.addEventListener("dragstart", (event) => {
          const id = this.#card?.id;
          if (!id) return;
          const path = event.composedPath?.() ?? [];
          if (
            path.some((el) =>
              el?.matches?.("input, select, textarea, button, [contenteditable]"),
            )
          ) {
            event.preventDefault();
            return;
          }
          event.dataTransfer.setData("text/plain", id);
          event.dataTransfer.setData("application/x-antikolt-card", id);
          event.dataTransfer.effectAllowed = "move";
        });
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
      article.setAttribute("aria-label", `${card.title} — ${card.domain} — ${t(status.label)}`);
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
        input.setAttribute("aria-label", t("A kártya címe"));
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
      for (const entry of cardContainers) {
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
      for (const entry of cardWorkspaces) {
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
        input.setAttribute("aria-label", t("A kártya webcíme"));
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
      badge.title = t(card.pinned ? "Kitűzés levétele" : "Kitűzés");
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
      noteButton.title = t(card.notes ? "Jegyzet szerkesztése" : "Jegyzet hozzáadása");

      const editor = document.createElement("div");
      editor.className = "note-editor";
      editor.hidden = true;
      const textarea = document.createElement("textarea");
      textarea.rows = 2;
      textarea.value = card.notes || "";
      textarea.setAttribute("aria-label", t("Jegyzet"));
      const save = document.createElement("button");
      save.type = "button";
      save.className = "note-save";
      save.textContent = t("Mentés");
      const cancel = document.createElement("button");
      cancel.type = "button";
      cancel.className = "note-cancel";
      cancel.textContent = t("Mégse");
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
      removeButton.textContent = this.armedDelete?.id === card.id ? t("Biztos?") : "×";
      removeButton.dataset.armed = this.armedDelete?.id === card.id ? "1" : "0";
      removeButton.title = t(
        this.armedDelete?.id === card.id
          ? "Kattints újra a törléshez (5 másodpercig)"
          : "Kártya törlése (két kattintás)",
      );
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

      meta.append(dot, containerSelect, workspaceSelect, domain);
      // The working set the shell measured at freeze (F6): a hint, not a live number — the card is on disk now.
      if (card.memoryBytes > 0) {
        const memory = document.createElement("span");
        memory.className = "card-memory";
        memory.textContent = `≈${Math.max(1, Math.round(card.memoryBytes / 1048576))} MB`;
        memory.title = t("a lap utolsó munkakészlete fagyasztáskor");
        meta.append(memory);
      }
      meta.append(noteButton, badge, removeButton);
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
        remove.setAttribute("aria-label", `${tag} ${t("címke törlése")}`);
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
      addTag.textContent = t("＋címke");
      addTag.title = t("Címke hozzáadása");
      addTag.addEventListener("click", (event) => {
        event.preventDefault();
        event.stopPropagation();
        const input = document.createElement("input");
        input.type = "text";
        input.className = "tag-input";
        input.setAttribute("aria-label", t("Új címke"));
        input.placeholder = t("címke");
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
    .card-memory { font-variant-numeric: tabular-nums; color: var(--text-muted); flex: none; }
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

  if (!global.customElements.get("antikolt-card")) {
    global.customElements.define("antikolt-card", AntikoltCard);
  }

  global.antikoltCard = {
    AntikoltCard,
    STATUS,
    hashHue,
    cardStyles,
    setOptions: function (options) {
      if (options && Array.isArray(options.containers)) { cardContainers = options.containers; }
      if (options && Array.isArray(options.workspaces)) { cardWorkspaces = options.workspaces; }
    },
    getPendingFocusId: function () { return pendingFocusId; }
  };
})(window);