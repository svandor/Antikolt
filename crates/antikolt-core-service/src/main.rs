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

mod sessions;
mod store;

use std::io::{Read, Write};
use std::path::PathBuf;

use antikolt_core::session::{SessionLedgerEntry, SessionRules};
use antikolt_ipc::{decode_json_frame, encode_json_frame, FrameError};
use serde_json::{json, Value};

use sessions::SessionStore;
use store::{Card, CardStore};

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
    eprintln!(
        "[core] cards={} sessions={} dir={}",
        store.list().len(),
        sessions.list().len(),
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
                    let (reply, keep_serving) = handle(&request, &mut store, &mut sessions);
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

/// Handle one request. The flag says whether to keep serving: `shutdown` answers, then stops.
fn handle(request: &Value, store: &mut CardStore, sessions: &mut SessionStore) -> (Value, bool) {
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
        "cards_list" => (ok_reply(id, json!({ "cards": store.list() })), true),
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
        // ---- session ledger -------------------------------------------------------------------
        // The policy itself lives in `antikolt-core::session`; these ops are the ledger's verbs, and
        // `session_due` is the evaluation the shell's cleanup round asks for (ADR-033).
        "session_list" => {
            // Each row carries the *measured* size of what a cleanup would remove (session::SessionSize) and
            // whether it holds a login: the panel must be able to warn before the button, not after.
            let rows: Vec<serde_json::Value> = sessions
                .list()
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
                ok_reply(id, json!({ "sessions": rows, "rules": sessions.rules() })),
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
            (
                ok_reply(id, json!({ "now_ms": at, "due": sessions.due(at, locked) })),
                true,
            )
        }
        "shutdown" => (ok_reply(id, json!({})), false),
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
