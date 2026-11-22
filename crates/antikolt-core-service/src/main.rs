// SPDX-License-Identifier: MPL-2.0

//! The core service: the Antikolt core running as its own process.
//!
//! The shell must not own the data and the engine must not own the policy, so the core is a third
//! process. It speaks the same length-prefixed JSON framing as the engine link (`antikolt-ipc`), but a
//! smaller message set — the engine's `Message` enum is about surfaces, this is about cards.
//!
//! Wire shape (one JSON object per frame, `op` names the request):
//!
//! ```text
//! {"version":1,"id":7,"op":"cards_list"}
//! {"version":1,"id":7,"op":"reply","to":7,"ok":true,"data":{"cards":[…]}}
//! {"version":1,"id":8,"op":"reply","to":8,"ok":false,"error":{"code":"not_found",…}}
//! ```
//!
//! Diagnostics go to stderr; stdout carries frames only, exactly like the engine host.

mod bundle;
mod containers;
mod persist;
mod sessions;
mod sqlite_store;
mod store;
mod workspaces;

use std::io::{Read, Write};
use std::path::PathBuf;

use antikolt_core::session::{SessionLedgerEntry, SessionRules};
use antikolt_ipc::{decode_json_frame, encode_json_frame, FrameError};
use serde_json::{json, Value};

use bundle::Bundle;
use containers::{Container, ContainerStore};
use sessions::SessionStore;
use sqlite_store::UserPrefs;
use store::{Card, CardStore};
use workspaces::{Workspace, WorkspaceStore};

/// Version of this small protocol, independent of the engine link's version.
const CORE_PROTOCOL_VERSION: u16 = 1;

fn main() -> std::process::ExitCode {
    match run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("[core] {message}");
            std::process::ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let data_dir = data_directory()?;
    let mut store = CardStore::open(&data_dir).map_err(|error| {
        format!(
            "cannot open the card store in {}: {error}",
            data_dir.display()
        )
    })?;
    // The ledger is a second file on purpose: a broken session row must not cost the card list.
    let mut sessions = SessionStore::open(&data_dir).map_err(|error| {
        format!(
            "cannot open the session ledger in {}: {error}",
            data_dir.display()
        )
    })?;
    // The container and workspace tables are further files: they are shared by every card, and a broken one must
    // not cost the cards.
    let mut containers = ContainerStore::open(&data_dir).map_err(|error| {
        format!(
            "cannot open the container store in {}: {error}",
            data_dir.display()
        )
    })?;
    let mut workspaces = WorkspaceStore::open(&data_dir).map_err(|error| {
        format!(
            "cannot open the workspace store in {}: {error}",
            data_dir.display()
        )
    })?;
    // Every store adopted its pre-switch JSON document (if there was one) and recorded that in the database, so a failed
    // adoption anywhere above leaves the question open and the next start retries it (ADR-069/070).
    let card_count = store
        .list()
        .map_err(|error| format!("cannot read the card store: {error}"))?
        .len();
    let session_count = sessions
        .list()
        .map_err(|error| format!("cannot read the session ledger: {error}"))?
        .len();
    let container_count = containers
        .list()
        .map_err(|error| format!("cannot read the container store: {error}"))?
        .len();
    let workspace_count = workspaces
        .list()
        .map_err(|error| format!("cannot read the workspace store: {error}"))?
        .len();
    eprintln!(
        "[core] cards={} sessions={} containers={} workspaces={} dir={}",
        card_count,
        session_count,
        container_count,
        workspace_count,
        data_dir.display()
    );

    let stdin = std::io::stdin();
    let mut input = stdin.lock();
    let stdout = std::io::stdout();
    let mut output = stdout.lock();
    let mut buffer: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 8192];

    loop {
        // Serve every complete frame already buffered before asking for more bytes.
        loop {
            match decode_json_frame(&buffer) {
                Ok((request, consumed)) => {
                    buffer.drain(..consumed);
                    let (reply, keep_serving) = handle(
                        &request,
                        &mut store,
                        &mut sessions,
                        &mut containers,
                        &mut workspaces,
                    );
                    write_frame(&mut output, &reply)?;
                    if !keep_serving {
                        return Ok(());
                    }
                }
                Err(FrameError::Incomplete { .. }) => break,
                Err(error) => return Err(format!("malformed frame: {error}")),
            }
        }

        match input.read(&mut chunk) {
            Ok(0) => return Ok(()), // the shell closed the link
            Ok(read) => buffer.extend_from_slice(&chunk[..read]),
            Err(error) => return Err(format!("stdin read failed: {error}")),
        }
    }
}

fn write_frame(output: &mut impl Write, value: &Value) -> Result<(), String> {
    let bytes = encode_json_frame(value).map_err(|error| format!("cannot encode: {error}"))?;
    output
        .write_all(&bytes)
        .and_then(|()| output.flush())
        .map_err(|error| format!("stdout write failed: {error}"))
}

/// Write the four derived JSON exports beside the database, best effort.
///
/// Since ADR-069/070 the database is the only truth and every `*.json` is a derived document: a failure to write one
/// must not fail the user's write, but it must not be silent either.
fn export_all(
    store: &CardStore,
    sessions: &SessionStore,
    containers: &ContainerStore,
    workspaces: &WorkspaceStore,
) {
    let results: [Result<PathBuf, String>; 4] = [
        store.export_json().map_err(|error| error.to_string()),
        containers.export_json().map_err(|error| error.to_string()),
        workspaces.export_json().map_err(|error| error.to_string()),
        sessions.export_json().map_err(|error| error.to_string()),
    ];
    for result in results {
        if let Err(error) = result {
            eprintln!("[core] store_export_failed: {error}");
        }
    }
}

/// The cards, or a loud error reply: an unreadable store must never look like an empty grid.
fn cards_or_error(id: u64, store: &CardStore) -> Result<Vec<Card>, Value> {
    store
        .list()
        .map_err(|error| error_reply(id, "internal", &format!("cannot read the cards: {error}")))
}

/// Handle one request. The flag says whether to keep serving: `shutdown` answers, then stops.
fn handle(
    request: &Value,
    store: &mut CardStore,
    sessions: &mut SessionStore,
    containers: &mut ContainerStore,
    workspaces: &mut WorkspaceStore,
) -> (Value, bool) {
    let id = request.get("id").and_then(Value::as_u64).unwrap_or(0);
    let version = request
        .get("version")
        .and_then(Value::as_u64)
        .unwrap_or(u64::from(CORE_PROTOCOL_VERSION));
    if version != u64::from(CORE_PROTOCOL_VERSION) {
        return (
            error_reply(
                id,
                "version_mismatch",
                &format!("this core speaks version {CORE_PROTOCOL_VERSION}"),
            ),
            true,
        );
    }

    let op = request
        .get("op")
        .and_then(Value::as_str)
        .unwrap_or_default();
    match op {
        "hello" => (
            json!({
                "version": CORE_PROTOCOL_VERSION,
                "id": id,
                "op": "welcome",
                // `welcome` is its own message kind, so it echoes the request id instead of a `to`.
                "to": id,
                "core": { "name": "antikolt-core", "version": env!("CARGO_PKG_VERSION"),
                          "protocol": CORE_PROTOCOL_VERSION },
            }),
            true,
        ),
        // ---- the store switch (ADR-069) -------------------------------------------------------------
        // The grid's list is a database read, full stop. Until this slice the JSON store was the writer and the list
        // had to mirror it forward (ADR-065/066) and heal a stale copy; now there is one writer, so a list request is
        // a query and a disagreement cannot exist to heal.
        "cards_list" => match cards_or_error(id, store) {
            Ok(cards) => (ok_reply(id, json!({ "cards": cards })), true),
            Err(reply) => (reply, true),
        },
        "card_open" => {
            let Some(card_id) = request.get("id_of_card").and_then(Value::as_str) else {
                return (
                    error_reply(id, "invalid_argument", "card_open needs id_of_card"),
                    true,
                );
            };
            match store.mark_opened(card_id, now_ms()) {
                Ok(Some(card)) => (ok_reply(id, json!({ "card": card })), true),
                Ok(None) => (error_reply(id, "not_found", "no card with that id"), true),
                Err(error) => (error_reply(id, "internal", &error.to_string()), true),
            }
        }
        "card_upsert" => {
            let Some(card) = request.get("card") else {
                return (
                    error_reply(id, "invalid_argument", "card_upsert needs card"),
                    true,
                );
            };
            match serde_json::from_value::<Card>(card.clone()) {
                Ok(card) => match store.upsert(card) {
                    Ok(card) => (ok_reply(id, json!({ "card": card })), true),
                    Err(error) => (error_reply(id, "internal", &error.to_string()), true),
                },
                Err(error) => (
                    error_reply(id, "invalid_argument", &error.to_string()),
                    true,
                ),
            }
        }
        "card_delete" => {
            let Some(card_id) = request.get("id_of_card").and_then(Value::as_str) else {
                return (
                    error_reply(id, "invalid_argument", "card_delete needs id_of_card"),
                    true,
                );
            };
            match store.remove(card_id) {
                Ok(true) => (ok_reply(id, json!({ "removed": true })), true),
                Ok(false) => (error_reply(id, "not_found", "no card with that id"), true),
                Err(error) => (error_reply(id, "internal", &error.to_string()), true),
            }
        }
        // The page's text is stored on its own instead of a whole `card_upsert` because the shell does not own the
        // rest of the record: sending a card back would race with a pin the user just made (ADR-049).
        "card_page_text" => {
            let (Some(card_id), Some(text)) = (
                request.get("id_of_card").and_then(Value::as_str),
                request.get("text").and_then(Value::as_str),
            ) else {
                return (
                    error_reply(
                        id,
                        "invalid_argument",
                        "card_page_text needs id_of_card and text",
                    ),
                    true,
                );
            };
            match store.set_page_text(card_id, text) {
                Ok(Some(card)) => (ok_reply(id, json!({ "card": card })), true),
                Ok(None) => (error_reply(id, "not_found", "no card with that id"), true),
                Err(error) => (error_reply(id, "internal", &error.to_string()), true),
            }
        }
        // The working set is stored on its own for the same reason the text is: a single-column write, so a
        // capture never races a pin or note the user just made (F6, per-card breakdown).
        "card_memory" => {
            let (Some(card_id), Some(bytes)) = (
                request.get("id_of_card").and_then(Value::as_str),
                request.get("memory_bytes").and_then(Value::as_i64),
            ) else {
                return (
                    error_reply(
                        id,
                        "invalid_argument",
                        "card_memory needs id_of_card and memory_bytes",
                    ),
                    true,
                );
            };
            match store.set_card_memory(card_id, bytes) {
                Ok(Some(card)) => (ok_reply(id, json!({ "card": card })), true),
                Ok(None) => (error_reply(id, "not_found", "no card with that id"), true),
                Err(error) => (error_reply(id, "internal", &error.to_string()), true),
            }
        }
        // ---- workspace table ------------------------------------------------------------------
        // The tabs the grid shows live here (ADR-056), in the database since ADR-070. The page used to invent the
        // assignment, which meant the thirteenth card silently claimed to be in "Olvasnivaló".
        "workspaces_list" => match workspaces.list() {
            Ok(list) => (ok_reply(id, json!({ "workspaces": list })), true),
            Err(error) => (error_reply(id, "internal", &error.to_string()), true),
        },
        "workspace_upsert" => {
            let Some(workspace) = request.get("workspace") else {
                return (
                    error_reply(id, "invalid_argument", "workspace_upsert needs workspace"),
                    true,
                );
            };
            match serde_json::from_value::<Workspace>(workspace.clone()) {
                Ok(workspace) => match workspaces.upsert(workspace) {
                    Ok(workspace) => (ok_reply(id, json!({ "workspace": workspace })), true),
                    Err(error) => (
                        error_reply(id, "invalid_argument", &error.to_string()),
                        true,
                    ),
                },
                Err(error) => (
                    error_reply(id, "invalid_argument", &error.to_string()),
                    true,
                ),
            }
        }
        "workspace_delete" => {
            let Some(workspace_id) = request.get("id_of_workspace").and_then(Value::as_str) else {
                return (
                    error_reply(
                        id,
                        "invalid_argument",
                        "workspace_delete needs id_of_workspace",
                    ),
                    true,
                );
            };
            // A workspace that still holds cards is not deleted: the same rule as the containers (ADR-052), for the
            // same reason — a silent move is how "my cards disappeared" starts.
            let cards = match cards_or_error(id, store) {
                Ok(cards) => cards,
                Err(reply) => return (reply, true),
            };
            let used_by = cards
                .into_iter()
                .filter(|card| card.workspace == workspace_id)
                .count();
            if used_by > 0 {
                return (
                    error_reply(
                        id,
                        "in_use",
                        &format!("{used_by} kártya még ebben a munkaterületben van"),
                    ),
                    true,
                );
            }
            match workspaces.delete(workspace_id) {
                Ok(true) => (ok_reply(id, json!({ "removed": true })), true),
                Ok(false) => (
                    error_reply(id, "not_found", "no workspace with that id"),
                    true,
                ),
                Err(error) => (error_reply(id, "internal", &error.to_string()), true),
            }
        }
        // ---- container table ------------------------------------------------------------------
        // The colours and names the grid draws live here, not in a JSON constant in the page (ADR-052), in the database
        // since ADR-070: a rename must survive a reload, and the "same" container must not mean two different things in
        // two layers.
        "containers_list" => match containers.list() {
            Ok(list) => (ok_reply(id, json!({ "containers": list })), true),
            Err(error) => (error_reply(id, "internal", &error.to_string()), true),
        },
        "container_upsert" => {
            let Some(container) = request.get("container") else {
                return (
                    error_reply(id, "invalid_argument", "container_upsert needs container"),
                    true,
                );
            };
            match serde_json::from_value::<Container>(container.clone()) {
                Ok(container) => match containers.upsert(container) {
                    Ok(container) => (ok_reply(id, json!({ "container": container })), true),
                    Err(error) => (
                        error_reply(id, "invalid_argument", &error.to_string()),
                        true,
                    ),
                },
                Err(error) => (
                    error_reply(id, "invalid_argument", &error.to_string()),
                    true,
                ),
            }
        }
        "container_delete" => {
            let Some(container_id) = request.get("id_of_container").and_then(Value::as_str) else {
                return (
                    error_reply(
                        id,
                        "invalid_argument",
                        "container_delete needs id_of_container",
                    ),
                    true,
                );
            };
            // A container that still holds cards is not deleted silently: a silent move is how "my cards
            // disappeared" starts. The caller may name where they should go (`move_to`, ADR-054) — then the move
            // and the delete happen in one answer, and the reply says how many cards travelled.
            let move_to = request
                .get("move_to")
                .and_then(Value::as_str)
                .unwrap_or_default();
            let cards = match cards_or_error(id, store) {
                Ok(cards) => cards,
                Err(reply) => return (reply, true),
            };
            let used_by = cards
                .into_iter()
                .filter(|card| card.container == container_id)
                .count();
            if used_by > 0 {
                if move_to.is_empty() {
                    return (
                        error_reply(
                            id,
                            "in_use",
                            &format!("{used_by} kártya még ebben a konténerben van"),
                        ),
                        true,
                    );
                }
                if move_to == container_id {
                    return (
                        error_reply(
                            id,
                            "invalid_argument",
                            "move_to nem lehet a törölt konténer",
                        ),
                        true,
                    );
                }
                let target_known = match containers.list() {
                    Ok(list) => list.iter().any(|entry| entry.id == move_to),
                    Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
                };
                if !target_known {
                    return (
                        error_reply(id, "not_found", "a move_to konténer nem létezik"),
                        true,
                    );
                }
                // The move is written before the delete: if the delete fails, the cards are still somewhere.
                match store.move_container(container_id, move_to) {
                    Ok(moved) => match containers.delete(container_id) {
                        Ok(true) => (
                            ok_reply(id, json!({ "removed": true, "moved": moved })),
                            true,
                        ),
                        Ok(false) => (
                            error_reply(id, "not_found", "no container with that id"),
                            true,
                        ),
                        Err(error) => (error_reply(id, "internal", &error.to_string()), true),
                    },
                    Err(error) => (error_reply(id, "internal", &error.to_string()), true),
                }
            } else {
                match containers.delete(container_id) {
                    Ok(true) => (ok_reply(id, json!({ "removed": true, "moved": 0 })), true),
                    Ok(false) => (
                        error_reply(id, "not_found", "no container with that id"),
                        true,
                    ),
                    Err(error) => (error_reply(id, "internal", &error.to_string()), true),
                }
            }
        }
        // ---- session ledger -------------------------------------------------------------------
        // The policy itself lives in `antikolt-core::session`; these ops are the ledger's verbs, and
        // `session_due` is the evaluation the shell's cleanup round asks for (ADR-033).
        "session_list" => {
            // Each row carries the *measured* size of what a cleanup would remove (session::SessionSize) and
            // whether it holds a login: the panel must be able to warn before the button, not after.
            let (entry_list, rules) = match (sessions.list(), sessions.rules()) {
                (Ok(entries), Ok(rules)) => (entries, rules),
                (Err(error), _) | (_, Err(error)) => {
                    return (error_reply(id, "internal", &error.to_string()), true)
                }
            };
            let rows: Vec<serde_json::Value> = entry_list
                .into_iter()
                .map(|entry| {
                    let size = entry.size();
                    let mut row = serde_json::to_value(&entry).unwrap_or_default();
                    if let Some(object) = row.as_object_mut() {
                        object.insert(
                            "size".into(),
                            json!({
                                "cookies": size.cookies,
                                "bytes": size.bytes,
                                "service_workers": size.service_workers,
                            }),
                        );
                        object.insert("loses_login".into(), json!(entry.loses_login()));
                    }
                    row
                })
                .collect();
            (
                ok_reply(id, json!({ "sessions": rows, "rules": rules })),
                true,
            )
        }
        "session_rules" => {
            let Some(rules) = request.get("rules") else {
                return (
                    error_reply(id, "invalid_argument", "session_rules needs rules"),
                    true,
                );
            };
            match serde_json::from_value::<SessionRules>(rules.clone()) {
                Ok(rules) => match sessions.set_rules(rules) {
                    Ok(rules) => (ok_reply(id, json!({ "rules": rules })), true),
                    Err(error) => (
                        error_reply(id, session_error_code(&error), &error.to_string()),
                        true,
                    ),
                },
                Err(error) => (
                    error_reply(id, "invalid_argument", &error.to_string()),
                    true,
                ),
            }
        }
        "session_upsert" => {
            let Some(session) = request.get("session") else {
                return (
                    error_reply(id, "invalid_argument", "session_upsert needs session"),
                    true,
                );
            };
            match serde_json::from_value::<SessionLedgerEntry>(session.clone()) {
                Ok(entry) => match sessions.upsert(entry) {
                    Ok(entry) => (ok_reply(id, json!({ "session": entry })), true),
                    Err(error) => (
                        error_reply(id, session_error_code(&error), &error.to_string()),
                        true,
                    ),
                },
                Err(error) => (
                    error_reply(id, "invalid_argument", &error.to_string()),
                    true,
                ),
            }
        }
        "session_close" => {
            let (Some(container_id), Some(origin)) = (
                request.get("container_id").and_then(Value::as_str),
                request.get("origin").and_then(Value::as_str),
            ) else {
                return (
                    error_reply(
                        id,
                        "invalid_argument",
                        "session_close needs container_id and origin",
                    ),
                    true,
                );
            };
            match sessions.close(container_id, origin) {
                Ok(true) => (ok_reply(id, json!({ "closed": true })), true),
                Ok(false) => (
                    error_reply(
                        id,
                        "not_found",
                        "no session row with that container and origin",
                    ),
                    true,
                ),
                Err(error) => (
                    error_reply(id, session_error_code(&error), &error.to_string()),
                    true,
                ),
            }
        }
        "session_due" => {
            let at = request
                .get("now_ms")
                .and_then(Value::as_u64)
                .unwrap_or_else(now_ms);
            let locked = request
                .get("app_locked")
                .and_then(Value::as_bool)
                .unwrap_or(false);
            match sessions.due(at, locked) {
                Ok(due) => (ok_reply(id, json!({ "now_ms": at, "due": due })), true),
                Err(error) => (error_reply(id, "internal", &error.to_string()), true),
            }
        }
        // ---- user preferences -----------------------------------------------------------------
        // One row of live-card cap and hibernation timings (ADR-072). Read without validation, write after it.
        "user_prefs" => match sessions.user_prefs() {
            Ok(prefs) => (ok_reply(id, json!({ "prefs": prefs })), true),
            Err(error) => (error_reply(id, "internal", &error.to_string()), true),
        },
        "user_prefs_set" => {
            let Some(prefs) = request.get("prefs") else {
                return (
                    error_reply(id, "invalid_argument", "user_prefs_set needs prefs"),
                    true,
                );
            };
            match serde_json::from_value::<UserPrefs>(prefs.clone()) {
                Ok(prefs) => match sessions.set_user_prefs(prefs) {
                    Ok(prefs) => (ok_reply(id, json!({ "prefs": prefs })), true),
                    Err(error) => (
                        error_reply(id, session_error_code(&error), &error.to_string()),
                        true,
                    ),
                },
                Err(error) => (
                    error_reply(id, "invalid_argument", &error.to_string()),
                    true,
                ),
            }
        }
        // ---- the portable bundle -----------------------------------------------------------------
        // The user's data in one file, so a profile can be carried to another machine (ADR-059). Export reads the
        // stores; import validates the whole file *before* it writes anything, and reports what it moved.
        "profile_export" => {
            let Some(path) = request.get("path").and_then(Value::as_str) else {
                return (
                    error_reply(id, "invalid_argument", "profile_export needs path"),
                    true,
                );
            };
            let cards = match cards_or_error(id, store) {
                Ok(cards) => cards,
                Err(reply) => return (reply, true),
            };
            let container_list = match containers.list() {
                Ok(list) => list,
                Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
            };
            let workspace_list = match workspaces.list() {
                Ok(list) => list,
                Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
            };
            let session_list = match sessions.list() {
                Ok(list) => list,
                Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
            };
            let rules = match sessions.rules() {
                Ok(rules) => rules,
                Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
            };
            let prefs = match sessions.user_prefs() {
                Ok(prefs) => prefs,
                Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
            };
            let bundle = Bundle::new(
                now_ms(),
                cards,
                container_list,
                workspace_list,
                session_list,
                rules,
                prefs,
            );
            let (cards, containers_count, workspaces_count, sessions_count) = bundle.counts();
            match bundle.write(std::path::Path::new(path)) {
                Ok(()) => (
                    ok_reply(
                        id,
                        json!({
                            "path": path,
                            "schema": bundle::BUNDLE_SCHEMA,
                            "cards": cards,
                            "containers": containers_count,
                            "workspaces": workspaces_count,
                            "sessions": sessions_count,
                        }),
                    ),
                    true,
                ),
                Err(error) => (error_reply(id, "internal", &error.to_string()), true),
            }
        }
        "profile_import" => {
            let Some(path) = request.get("path").and_then(Value::as_str) else {
                return (
                    error_reply(id, "invalid_argument", "profile_import needs path"),
                    true,
                );
            };
            let read = match Bundle::read(std::path::Path::new(path)) {
                Ok(bundle) => bundle,
                Err(error) => {
                    let code = match error {
                        bundle::BundleError::Schema(_) => "version_mismatch",
                        bundle::BundleError::Io(_) => "not_found",
                        _ => "invalid_argument",
                    };
                    return (error_reply(id, code, &error.to_string()), true);
                }
            };
            let (cards, containers_count, workspaces_count, sessions_count) = read.counts();
            // The stores are written one after another and each write is atomic; a failure part-way leaves a
            // profile that is readable and consistent, and the reply says which step failed.
            let steps: [(&str, Result<usize, String>); 4] = [
                (
                    "cards",
                    store
                        .replace_all(read.cards.clone())
                        .map_err(|error| error.to_string()),
                ),
                (
                    "containers",
                    containers
                        .replace_all(read.containers.clone())
                        .map_err(|error| error.to_string()),
                ),
                (
                    "workspaces",
                    workspaces
                        .replace_all(read.workspaces.clone())
                        .map_err(|error| error.to_string()),
                ),
                (
                    "sessions",
                    sessions
                        .replace_all(read.sessions.clone(), read.rules)
                        .map_err(|error| error.to_string()),
                ),
            ];
            for (name, step) in steps.iter() {
                if let Err(message) = step {
                    return (
                        error_reply(
                            id,
                            "internal",
                            &format!("az import a(z) {name} táblánál megállt: {message}"),
                        ),
                        true,
                    );
                }
            }
            // Preferences are optional in the bundle: an older bundle without them keeps the profile's current prefs.
            if let Some(prefs) = read.prefs {
                if let Err(error) = sessions.set_user_prefs(prefs) {
                    return (
                        error_reply(
                            id,
                            "internal",
                            &format!("user preferences import failed: {error}"),
                        ),
                        true,
                    );
                }
            }
            // Every store is a row store now, so each of the four exports must describe what the import actually wrote
            // (ADR-069/070). A failed export is logged, not fatal: the database is the truth and it was written.
            export_all(store, sessions, containers, workspaces);
            (
                ok_reply(
                    id,
                    json!({
                        "path": path,
                        "schema": read.schema,
                        "cards": cards,
                        "containers": containers_count,
                        "workspaces": workspaces_count,
                        "sessions": sessions_count,
                    }),
                ),
                true,
            )
        }
        // ---- the storage report (ADR-064 → ADR-070) -----------------------------------------------
        // All four stores are SQLite-first now; this op is how the shell and the gate see that the switch really
        // happened and that the exports beside the database still describe the rows. `sync` writes the four exports
        // from the rows, then every export is read back and compared row by row. Without `sync` it reports what is
        // already on disk.
        "db_status" => {
            let sync = request
                .get("sync")
                .and_then(Value::as_bool)
                .unwrap_or(false);
            if sync {
                export_all(store, sessions, containers, workspaces);
            }
            let (cards, containers_count, workspaces_count, sessions_count) = match store.counts() {
                Ok(counts) => counts,
                Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
            };
            let schema = match store.schema_version() {
                Ok(version) => version,
                Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
            };
            // The check is the *export* direction: the rows are the truth, so "verified" means "the files this core
            // writes can be parsed back into exactly these rows". A store's unreadable export makes the whole answer
            // `not measured` (`mismatches: -1`) instead of a fake agreement.
            let mut checked = 0i64;
            let mut mismatches = 0i64;
            let mut unreadable = false;
            let checks: [Result<(i64, i64), String>; 4] = [
                store.verify_export().map_err(|error| error.to_string()),
                containers
                    .verify_export()
                    .map_err(|error| error.to_string()),
                workspaces
                    .verify_export()
                    .map_err(|error| error.to_string()),
                sessions.verify_export().map_err(|error| error.to_string()),
            ];
            for check in checks {
                match check {
                    Ok((rows, bad)) => {
                        checked += rows;
                        if bad < 0 {
                            unreadable = true;
                        } else {
                            mismatches += bad;
                        }
                    }
                    Err(error) => {
                        return (error_reply(id, "internal", &error), true);
                    }
                }
            }
            if unreadable {
                mismatches = -1;
            }
            let cards_in_workspace = match store.list() {
                Ok(list) => match list.first() {
                    Some(card) => store.cards_in_workspace(&card.workspace).unwrap_or(0),
                    None => 0,
                },
                Err(error) => return (error_reply(id, "internal", &error.to_string()), true),
            };
            let exports: Vec<String> = [
                store.path(),
                containers.path(),
                workspaces.path(),
                sessions.path(),
            ]
            .into_iter()
            .map(|path| path.display().to_string())
            .collect();
            (
                ok_reply(
                    id,
                    json!({
                        "path": store.db_path().display().to_string(),
                        "exports": exports,
                        "schema": schema,
                        "synced": sync,
                        "checked": checked,
                        "verified": mismatches == 0,
                        "mismatches": mismatches,
                        "cards_in_workspace": cards_in_workspace,
                        "cards": cards,
                        "containers": containers_count,
                        "workspaces": workspaces_count,
                        "sessions": sessions_count,
                    }),
                ),
                true,
            )
        }
        "shutdown" => {
            // One last export of all four documents, so the readable copies next to the database are never older than
            // the last write. A failure is logged rather than refusing the shutdown: the rows are already on disk.
            export_all(store, sessions, containers, workspaces);
            (ok_reply(id, json!({})), false)
        }
        other => (
            error_reply(id, "not_supported", &format!("unknown op `{other}`")),
            true,
        ),
    }
}

/// The wire error code for a ledger failure: a refused row is the caller's fault, the rest is storage.
fn session_error_code(error: &sessions::SessionError) -> &'static str {
    if error.is_invalid_argument() {
        "invalid_argument"
    } else {
        "internal"
    }
}

fn ok_reply(id: u64, data: Value) -> Value {
    json!({ "version": CORE_PROTOCOL_VERSION, "id": id, "op": "reply", "to": id,
            "ok": true, "data": data })
}

fn error_reply(id: u64, code: &str, message: &str) -> Value {
    json!({ "version": CORE_PROTOCOL_VERSION, "id": id, "op": "reply", "to": id,
            "ok": false, "error": { "code": code, "message": message } })
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|elapsed| u64::try_from(elapsed.as_millis()).unwrap_or(0))
        .unwrap_or(0)
}

/// Where the card store lives. `--data-dir` wins, then `ANTIKOLT_DATA_DIR`, then the platform's
/// local application data directory.
fn data_directory() -> Result<PathBuf, String> {
    let mut args = std::env::args().skip(1);
    if let Some(arg) = args.next() {
        match arg.as_str() {
            "--data-dir" => {
                let Some(value) = args.next() else {
                    return Err("--data-dir needs a value".to_string());
                };
                return Ok(PathBuf::from(value));
            }
            "--help" | "-h" => {
                println!("antikolt-core-service [--data-dir <path>]");
                std::process::exit(0);
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }

    if let Ok(from_env) = std::env::var("ANTIKOLT_DATA_DIR") {
        if !from_env.is_empty() {
            return Ok(PathBuf::from(from_env));
        }
    }
    let base = std::env::var("LOCALAPPDATA")
        .or_else(|_| std::env::var("XDG_DATA_HOME"))
        .map_err(|_| "no data directory available; pass --data-dir".to_string())?;
    Ok(PathBuf::from(base).join("Antikolt"))
}
