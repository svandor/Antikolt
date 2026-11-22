// SPDX-License-Identifier: MPL-2.0

//! End-to-end test of the core service: a real process, a real stdio link, a real file on disk.
//!
//! The unit tests in `store.rs` cover the card list; this covers the thing the shell will actually
//! talk to, because a service that only works when called as a function is not a service.

use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};

use antikolt_ipc::{decode_json_frame, encode_json_frame};
use serde_json::{json, Value};

struct Service {
    child: Child,
    buffer: Vec<u8>,
    next_id: u64,
}

impl Service {
    fn start(data_dir: &PathBuf) -> Self {
        let child = Command::new(env!("CARGO_BIN_EXE_antikolt-core-service"))
            .arg("--data-dir")
            .arg(data_dir)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn the core service");
        Self {
            child,
            buffer: Vec::new(),
            next_id: 1,
        }
    }

    fn request(&mut self, op: &str, extra: Value) -> Value {
        let id = self.next_id;
        self.next_id += 1;
        let mut request = json!({ "version": 1, "id": id, "op": op });
        if let (Some(fields), Some(target)) = (extra.as_object(), request.as_object_mut()) {
            for (key, value) in fields {
                target.insert(key.clone(), value.clone());
            }
        }
        let frame = encode_json_frame(&request).expect("encode");
        let stdin = self.child.stdin.as_mut().expect("stdin");
        stdin.write_all(&frame).expect("write the request");
        stdin.flush().expect("flush");

        // Read until one whole reply arrives; the loop may hand us partial bytes.
        loop {
            if let Ok((value, consumed)) = decode_json_frame(&self.buffer) {
                self.buffer.drain(..consumed);
                // `welcome` is its own message kind and carries no `to`; every reply must echo ours.
                if let Some(to) = value.get("to") {
                    assert_eq!(to, id, "the reply must answer our request");
                }
                return value;
            }
            let mut chunk = [0u8; 4096];
            let read = self
                .child
                .stdout
                .as_mut()
                .expect("stdout")
                .read(&mut chunk)
                .expect("read");
            assert!(read > 0, "the service closed its stdout before replying");
            self.buffer.extend_from_slice(&chunk[..read]);
        }
    }

    fn stop(mut self) {
        let reply = self.request("shutdown", json!({}));
        assert_eq!(reply["ok"], true);
        let _ = self.child.wait();
    }
}

fn temp_dir(name: &str) -> PathBuf {
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .expect("clock")
        .as_nanos();
    let dir = std::env::temp_dir().join(format!("antikolt-core-service-{name}-{stamp}"));
    std::fs::create_dir_all(&dir).expect("temp dir");
    dir
}

#[test]
fn the_service_greets_serves_cards_and_records_an_open() {
    let dir = temp_dir("lifecycle");
    let mut service = Service::start(&dir);

    let welcome = service.request("hello", json!({}));
    assert_eq!(welcome["op"], "welcome");
    assert_eq!(welcome["core"]["name"], "antikolt-core");

    let listed = service.request("cards_list", json!({}));
    assert_eq!(listed["ok"], true);
    let cards = listed["data"]["cards"].as_array().expect("cards");
    assert_eq!(cards.len(), 3, "a fresh store is seeded with examples");
    let first_id = cards[0]["id"].as_str().expect("id").to_string();

    // Opening a card records the time; the grid orders by it.
    let opened = service.request("card_open", json!({ "id_of_card": first_id }));
    assert_eq!(opened["ok"], true);
    let stamp = opened["data"]["card"]["last_opened_at_ms"]
        .as_u64()
        .expect("timestamp");
    assert!(stamp > 0, "opening a card stamps it");

    let reloaded = service.request("cards_list", json!({}));
    let cards = reloaded["data"]["cards"].as_array().expect("cards");
    assert_eq!(
        cards[0]["id"],
        first_id.as_str(),
        "the opened card comes first"
    );

    let missing = service.request("card_open", json!({ "id_of_card": "nope" }));
    assert_eq!(missing["ok"], false);
    assert_eq!(missing["error"]["code"], "not_found");

    let unknown = service.request("cannot_possibly_exist", json!({}));
    assert_eq!(unknown["error"]["code"], "not_supported");

    service.stop();
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_store_is_on_disk_and_survives_a_restart() {
    let dir = temp_dir("persist");
    let mut service = Service::start(&dir);
    let listed = service.request("cards_list", json!({}));
    let cards = listed["data"]["cards"].as_array().expect("cards");
    let id = cards[0]["id"].as_str().expect("id").to_string();
    service.request("card_open", json!({ "id_of_card": id }));
    service.stop();

    assert!(
        dir.join("antikolt.sqlite3").exists(),
        "the cards are rows in the database, not a JSON document"
    );
    assert!(
        dir.join("cards.json").exists(),
        "the export is written beside the database"
    );

    let mut again = Service::start(&dir);
    let listed = again.request("cards_list", json!({}));
    let stamps = listed["data"]["cards"]
        .as_array()
        .expect("cards")
        .iter()
        .filter_map(|card| card["last_opened_at_ms"].as_u64())
        .collect::<Vec<_>>();
    assert!(
        stamps.iter().any(|stamp| *stamp > 0),
        "the timestamp written before the restart is still there"
    );
    again.stop();
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_database_is_the_store_and_the_report_proves_the_export() {
    // ADR-069: the card writes land in SQLite, and `db_status` is the measured statement that the export beside it
    // still describes them row by row.
    let dir = temp_dir("db-status");
    let mut service = Service::start(&dir);

    let status = service.request("db_status", json!({ "sync": true }));
    assert_eq!(status["ok"], true, "db_status answered: {status}");
    assert_eq!(status["data"]["schema"], json!(9));
    assert_eq!(status["data"]["verified"], true);
    // Three cards, four containers, three workspaces and the rules of an empty ledger: every store's export is checked.
    assert_eq!(status["data"]["checked"], json!(11), "every row is checked");
    assert_eq!(status["data"]["mismatches"], json!(0));
    assert_eq!(status["data"]["cards"], json!(3));
    assert_eq!(status["data"]["containers"], json!(4));
    assert_eq!(status["data"]["workspaces"], json!(3));
    assert_eq!(status["data"]["sessions"], json!(0));
    assert!(
        status["data"]["path"]
            .as_str()
            .expect("path")
            .ends_with("antikolt.sqlite3"),
        "the reply names the database file"
    );

    // A rename goes through the product op; the database is what changes, and the export follows on shutdown.
    let listed = service.request("cards_list", json!({}));
    let mut card = listed["data"]["cards"].as_array().expect("cards")[0].clone();
    let id = card["id"].as_str().expect("id").to_string();
    card["title"] = json!("Adatbázis-átnevezés");
    let upserted = service.request("card_upsert", json!({ "card": card }));
    assert_eq!(upserted["ok"], true);
    assert_eq!(upserted["data"]["card"]["title"], "Adatbázis-átnevezés");

    // The export on disk is stale until it is written again — and `db_status` says exactly that instead of claiming
    // agreement it did not measure.
    let unsynced = service.request("db_status", json!({}));
    assert_eq!(unsynced["ok"], true);
    assert_eq!(unsynced["data"]["verified"], false);
    assert!(
        unsynced["data"]["mismatches"].as_i64().unwrap_or(0) > 0,
        "the stale export is reported as a mismatch"
    );

    let synced = service.request("db_status", json!({ "sync": true }));
    assert_eq!(synced["data"]["verified"], true, "re-export agrees again");
    service.stop();

    // And the shutdown export is the database, not the list from before the rename.
    let text = std::fs::read_to_string(dir.join("cards.json")).expect("read the export");
    let exported: Vec<serde_json::Value> = serde_json::from_str(&text).expect("parse");
    assert!(
        exported
            .iter()
            .any(|row| row["id"] == id && row["title"] == "Adatbázis-átnevezés"),
        "the export carries the rename from the database"
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_container_workspace_and_session_documents_are_adopted_once_and_then_rows() {
    // ADR-070: the three documents that used to be the writers are adopted on the first start and stay rows after it —
    // including a cleanup policy, which is part of the profile a user chose.
    let dir = temp_dir("adopt-three");
    std::fs::write(
        dir.join("containers.json"),
        serde_json::to_string_pretty(&json!([
            { "id": "c-sajat", "name": "Saját", "color": "#123456" }
        ]))
        .expect("json"),
    )
    .expect("write containers");
    std::fs::write(
        dir.join("workspaces.json"),
        serde_json::to_string_pretty(&json!([
            { "id": "w-sajat", "name": "Saját tér" }
        ]))
        .expect("json"),
    )
    .expect("write workspaces");
    std::fs::write(
        dir.join("sessions.json"),
        serde_json::to_string_pretty(&json!({
            "rules": { "container_default": "wipe", "ttl_ms": 7200000, "wipe_on_app_lock": true },
            "entries": [{
                "container_id": "c-sajat",
                "origin": "https://pelda.hu",
                "state": "expired",
                "policy": "wipe",
                "cookie_count": 3,
                "cookie_bytes": 1024,
                "storage_bytes": 2048,
                "service_worker_count": 0,
                "last_seen_ms": 1,
                "last_verified_ms": null,
                "live_ram_bytes": null,
                "keep_alive": false
            }]
        }))
        .expect("json"),
    )
    .expect("write sessions");

    let mut service = Service::start(&dir);
    let containers = service.request("containers_list", json!({}));
    assert_eq!(containers["data"]["containers"][0]["id"], "c-sajat");
    let workspaces = service.request("workspaces_list", json!({}));
    assert_eq!(workspaces["data"]["workspaces"][0]["id"], "w-sajat");
    let sessions = service.request("session_list", json!({}));
    assert_eq!(
        sessions["data"]["sessions"].as_array().expect("rows").len(),
        1
    );
    assert_eq!(
        sessions["data"]["rules"]["container_default"], "wipe",
        "the profile's cleanup policy came over with the ledger"
    );

    let status = service.request("db_status", json!({ "sync": true }));
    assert_eq!(status["data"]["verified"], true);
    assert_eq!(status["data"]["containers"], json!(1));
    assert_eq!(status["data"]["workspaces"], json!(1));
    assert_eq!(status["data"]["sessions"], json!(1));
    service.stop();

    // A restart reads the rows, and the exports it wrote on the way out agree with them.
    let mut again = Service::start(&dir);
    let sessions = again.request("session_list", json!({}));
    assert_eq!(
        sessions["data"]["sessions"].as_array().expect("rows").len(),
        1
    );
    let status = again.request("db_status", json!({}));
    assert_eq!(
        status["data"]["verified"], true,
        "the exports agree after a restart"
    );
    again.stop();
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn a_container_carries_its_proxy_and_permission_policy_through_the_wire() {
    // ADR-071: a container is a boundary, not a label. The proxy it reaches the network through and the permissions its
    // pages may ask for are columns, so they survive a restart, travel on the wire and appear in the export.
    let dir = temp_dir("container-policy");
    let mut service = Service::start(&dir);
    let upserted = service.request(
        "container_upsert",
        json!({ "container": {
            "id": "c-fixed", "name": "Fix", "color": "#123456",
            "proxy_mode": "fixed", "proxy_host": "127.0.0.1", "proxy_port": 9050,
            "proxy_bypass": "<local>",
            "permissions": { "geolocation": "allow", "notifications": "deny" }
        }}),
    );
    assert_eq!(
        upserted["ok"], true,
        "the policy is a valid container: {upserted}"
    );
    assert_eq!(upserted["data"]["container"]["proxy_mode"], json!("fixed"));
    assert_eq!(upserted["data"]["container"]["proxy_port"], json!(9050));
    assert_eq!(
        upserted["data"]["container"]["permissions"]["geolocation"],
        json!("allow")
    );

    // A mode that is not one of the three is refused, not quietly turned into "system": the user's traffic must not go
    // somewhere they did not choose.
    let refused = service.request(
        "container_upsert",
        json!({ "container": { "id": "c-bad", "name": "Rossz", "color": "", "proxy_mode": "socks" } }),
    );
    assert_eq!(
        refused["ok"], false,
        "an unknown proxy mode is refused: {refused}"
    );
    assert_eq!(refused["error"]["code"], "invalid_argument");
    service.stop();

    let mut again = Service::start(&dir);
    let listed = again.request("containers_list", json!({}));
    let entry = listed["data"]["containers"]
        .as_array()
        .expect("containers")
        .iter()
        .find(|container| container["id"] == "c-fixed")
        .cloned()
        .expect("the container survives the restart");
    assert_eq!(entry["proxy_mode"], json!("fixed"));
    assert_eq!(entry["proxy_host"], json!("127.0.0.1"));
    assert_eq!(entry["permissions"]["notifications"], json!("deny"));
    again.stop();

    let text = std::fs::read_to_string(dir.join("containers.json")).expect("read the export");
    assert!(
        text.contains("\"proxy_mode\": \"fixed\"") && text.contains("\"geolocation\": \"allow\""),
        "the export carries the policy, so a profile moved to another machine keeps it"
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn a_profile_written_before_the_switch_is_adopted_not_reseeded() {
    // A real user upgrading: only `cards.json` exists. The first start must take those cards over instead of seeding
    // the examples next to them.
    let dir = temp_dir("adopt");
    let legacy = json!([
        {
            "id": "01UPGRADE000000000000001",
            "title": "Korábbi profil kártyája",
            "url": "https://pelda.hu/regi",
            "notes": "ez a régi fájlból jött"
        }
    ]);
    std::fs::write(
        dir.join("cards.json"),
        serde_json::to_string_pretty(&legacy).expect("json"),
    )
    .expect("write the legacy profile");

    let mut service = Service::start(&dir);
    let listed = service.request("cards_list", json!({}));
    let cards = listed["data"]["cards"].as_array().expect("cards");
    assert_eq!(cards.len(), 1, "the legacy card list is adopted");
    assert_eq!(cards[0]["title"], "Korábbi profil kártyája");
    assert_eq!(cards[0]["notes"], "ez a régi fájlból jött");
    service.stop();
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn user_prefs_start_at_defaults_and_round_trip() {
    // Fresh store → defaults (5 / 600_000 / 86_400_000). A valid set persists and reads back.
    // Out-of-range values and a cold window not longer than the idle one are rejected.
    let dir = temp_dir("prefs");
    let mut service = Service::start(&dir);

    let reply = service.request("user_prefs", json!({}));
    assert_eq!(reply["ok"], true);
    assert_eq!(reply["data"]["prefs"]["max_live_cards"], json!(5));
    assert_eq!(reply["data"]["prefs"]["idle_hibernate_ms"], json!(600_000));
    assert_eq!(reply["data"]["prefs"]["cold_card_ms"], json!(86_400_000));

    // A valid set: eight live cards, one hour idle, two hours cold. The cold state is the later one (ADR-072), so its
    // window is longer than the idle one.
    let set = service.request(
        "user_prefs_set",
        json!({ "prefs": { "max_live_cards": 8, "idle_hibernate_ms": 3_600_000, "cold_card_ms": 7_200_000 } }),
    );
    assert_eq!(set["ok"], true);
    assert_eq!(set["data"]["prefs"]["max_live_cards"], json!(8));
    assert_eq!(set["data"]["prefs"]["idle_hibernate_ms"], json!(3_600_000));
    assert_eq!(set["data"]["prefs"]["cold_card_ms"], json!(7_200_000));

    let again = service.request("user_prefs", json!({}));
    assert_eq!(again["ok"], true);
    assert_eq!(again["data"]["prefs"]["max_live_cards"], json!(8));
    assert_eq!(
        again["data"]["prefs"]["idle_hibernate_ms"],
        json!(3_600_000)
    );
    assert_eq!(again["data"]["prefs"]["cold_card_ms"], json!(7_200_000));

    // max_live_cards = 0 is below 1.
    let bad = service.request(
        "user_prefs_set",
        json!({ "prefs": { "max_live_cards": 0, "idle_hibernate_ms": 3_600_000, "cold_card_ms": 7_200_000 } }),
    );
    assert_eq!(bad["ok"], false);
    assert_eq!(bad["error"]["code"], "invalid_argument");

    // A cold window equal to the idle one is refused: a card cannot go cold at the same moment it hibernates.
    let bad = service.request(
        "user_prefs_set",
        json!({ "prefs": { "max_live_cards": 5, "idle_hibernate_ms": 3_600_000, "cold_card_ms": 3_600_000 } }),
    );
    assert_eq!(bad["ok"], false);

    service.stop();
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn user_prefs_survive_a_restart() {
    let dir = temp_dir("prefs-restart");
    let mut service = Service::start(&dir);

    let set = service.request(
        "user_prefs_set",
        json!({ "prefs": { "max_live_cards": 12, "idle_hibernate_ms": 3_600_000, "cold_card_ms": 7_200_000 } }),
    );
    assert_eq!(set["ok"], true, "a valid set is accepted: {set}");
    service.stop();

    let mut again = Service::start(&dir);
    let prefs = again.request("user_prefs", json!({}));
    assert_eq!(prefs["ok"], true);
    assert_eq!(prefs["data"]["prefs"]["max_live_cards"], json!(12));
    assert_eq!(
        prefs["data"]["prefs"]["idle_hibernate_ms"],
        json!(3_600_000)
    );
    assert_eq!(prefs["data"]["prefs"]["cold_card_ms"], json!(7_200_000));
    again.stop();
    let _ = std::fs::remove_dir_all(&dir);
}
