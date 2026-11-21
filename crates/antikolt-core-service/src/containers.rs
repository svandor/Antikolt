// SPDX-License-Identifier: MPL-2.0

//! The container table the core serves.
//!
//! A container is the colour-coded grouping a card belongs to (docs/HU/02 §7, table `container`). Until now the
//! shell kept this table in the *page* (`PAGE_CONTAINERS` in `home/app.js`), which meant a rename lived in a
//! JSON constant and the colour a card showed was decided by whichever layer drew it. This store moves the table
//! where every other fact lives: the core.
//!
//! Since ADR-070 the table is **SQLite-first**: the rows are the truth, and `containers.json` is a derived export —
//! written on adoption, on shutdown and when `db_status` asks for a sync, and read only once, to adopt a profile
//! whose JSON document was still the writer of record.

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use std::collections::BTreeMap;

use crate::persist::write_json_atomic;
use crate::sqlite_store::{Db, DbError};

/// The export beside the database: the legacy document, and the readable copy this build keeps.
pub const FILE_NAME: &str = "containers.json";

/// How many per-permission overrides one container may carry. The set of Chromium permissions is small; a cap keeps a
/// pasted blob from becoming a policy (the same reasoning as the tag cap, ADR-055).
pub const MAX_PERMISSION_RULES: usize = 32;

/// The three ways a container can reach the network (ADR-071).
pub const PROXY_MODES: [&str; 3] = ["system", "direct", "fixed"];

fn default_proxy_mode() -> String {
    "system".to_string()
}

/// One container: the colour-coded grouping a card belongs to, and — since ADR-071 — a real boundary rather than a
/// label. The fields after `color` say how *this* container reaches the network and which permissions it grants, which
/// is what a container is for: separating identities is meaningless if they share one cookie jar and one proxy.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Container {
    pub id: String,
    pub name: String,
    /// `#RRGGBB`; the page uses it for the dot and the legend chip.
    pub color: String,
    /// `system` (Chromium's own setting), `direct` (no proxy) or `fixed` (`proxy_host:proxy_port`).
    #[serde(default = "default_proxy_mode")]
    pub proxy_mode: String,
    #[serde(default)]
    pub proxy_host: String,
    #[serde(default)]
    pub proxy_port: u16,
    /// Chromium's bypass list syntax, `;`-separated (`*.local;<local>`). Only the `fixed` mode uses it.
    #[serde(default)]
    pub proxy_bypass: String,
    /// Per-permission policy: a Chromium permission id (`geolocation`, `notifications`, `media`,
    /// `clipboard_read_write`, …) mapped to `allow` or `deny`. Anything not listed is **never granted**: this host has no
    /// prompt surface yet, so the honest default is denial, and the user opts a container in instead of out.
    #[serde(default)]
    pub permissions: BTreeMap<String, String>,
}

#[derive(Debug, thiserror::Error)]
pub enum ContainerError {
    #[error("cannot read or write the container store: {0}")]
    Io(#[from] std::io::Error),
    #[error("the container store is not valid JSON: {0}")]
    Json(#[from] serde_json::Error),
    /// The pre-switch document could not be read: it was the only copy of the containers at that moment, so the core
    /// stops instead of replacing it with the seeds.
    #[error("cannot read the container list written by an earlier version ({path}): {source}")]
    Legacy {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("invalid container: {0}")]
    Invalid(String),
    #[error("the database refused the container table: {0}")]
    Db(#[from] DbError),
}

/// One container with the defaults the product ships: no proxy of its own, no permission granted.
fn seed(id: &str, name: &str, color: &str) -> Container {
    Container {
        id: id.to_string(),
        name: name.to_string(),
        color: color.to_string(),
        proxy_mode: default_proxy_mode(),
        proxy_host: String::new(),
        proxy_port: 0,
        proxy_bypass: String::new(),
        permissions: BTreeMap::new(),
    }
}

/// The containers the product ships with. Ids are stable and referenced by the cards that already exist.
fn seed_containers() -> Vec<Container> {
    vec![
        seed("c-work", "Munka", "#2F6FEB"),
        seed("c-private", "Magán", "#2E9E5B"),
        seed("c-money", "Pénz", "#E3A008"),
        seed("c-dev", "Fejlesztés", "#8E44AD"),
    ]
}

/// The container table: rows in SQLite, with `containers.json` as a derived export.
#[derive(Debug)]
pub struct ContainerStore {
    data_dir: PathBuf,
    db: Db,
}

impl ContainerStore {
    /// Open the store, adopting the pre-switch `containers.json` once (ADR-070).
    ///
    /// A missing document is seeded with the four containers the product shipped with; a document that exists — even
    /// an empty one — is adopted as it is, because the user may have deleted every container they had.
    pub fn open(directory: &Path) -> Result<Self, ContainerError> {
        fs::create_dir_all(directory)?;
        let db = Db::open(directory)?;
        let mut store = Self {
            data_dir: directory.to_path_buf(),
            db,
        };
        if !store.db.was_adopted("containers")? {
            store.adopt_legacy()?;
            store.db.mark_adopted("containers")?;
        }
        Ok(store)
    }

    /// The export's path inside the profile.
    pub fn path(&self) -> PathBuf {
        self.data_dir.join(FILE_NAME)
    }

    pub fn list(&self) -> Result<Vec<Container>, ContainerError> {
        Ok(self.db.all_containers()?)
    }

    /// Create or replace one container. The id must look like a container id (`c-…`) and the name must not be
    /// empty; a colour that is not `#RRGGBB` is replaced instead of rejected, because a wrong colour is a
    /// cosmetic bug and a rejected save is a lost rename.
    ///
    /// The proxy and permission fields are the ones that must *not* be guessed at (ADR-071): a proxy mode that is not
    /// one of the three, or a half-typed host, is refused, because silently falling back to "system" would send a
    /// container's traffic somewhere the user did not choose. A permission value that is not `allow`/`deny` is refused
    /// for the same reason.
    pub fn upsert(&mut self, mut container: Container) -> Result<Container, ContainerError> {
        container.id = container.id.trim().to_string();
        if !container.id.starts_with("c-") || container.id.len() < 3 {
            return Err(ContainerError::Invalid(
                "a konténer id `c-` előtaggal kezdődik".to_string(),
            ));
        }
        container.name = container.name.trim().to_string();
        if container.name.is_empty() {
            return Err(ContainerError::Invalid(
                "a konténer neve nem lehet üres".to_string(),
            ));
        }
        if !is_color(&container.color) {
            container.color = color_for(&container.id);
        }
        container.proxy_mode = container.proxy_mode.trim().to_ascii_lowercase();
        if !PROXY_MODES.contains(&container.proxy_mode.as_str()) {
            return Err(ContainerError::Invalid(format!(
                "ismeretlen proxy-mód: `{}` (system, direct vagy fixed)",
                container.proxy_mode
            )));
        }
        container.proxy_host = container.proxy_host.trim().to_string();
        container.proxy_bypass = container.proxy_bypass.trim().to_string();
        if container.proxy_mode == "fixed" {
            if container.proxy_host.is_empty() || container.proxy_host.contains(char::is_whitespace)
            {
                return Err(ContainerError::Invalid(
                    "fix proxyhoz kell egy hoszt (szóköz nélkül)".to_string(),
                ));
            }
            if container.proxy_port == 0 {
                return Err(ContainerError::Invalid(
                    "fix proxyhoz kell egy port (1–65535)".to_string(),
                ));
            }
        } else {
            // The host and port only mean something in `fixed` mode; carrying them in another mode would be a value
            // that is displayed but not used.
            container.proxy_host.clear();
            container.proxy_port = 0;
            container.proxy_bypass.clear();
        }
        container.permissions = Self::normalise_permissions(container.permissions)?;
        self.db.upsert_container(&container)?;
        Ok(container)
    }

    /// Trim, lowercase and cap the permission rules; refuse a value that is not a decision.
    ///
    /// Kept next to the write, not in the caller: a policy that says `maybe` would be stored and then interpreted as
    /// "not allow" by the engine, which is a rule the user never chose.
    fn normalise_permissions(
        permissions: BTreeMap<String, String>,
    ) -> Result<BTreeMap<String, String>, ContainerError> {
        let mut normalised = BTreeMap::new();
        for (key, value) in permissions {
            let key = key.trim().to_ascii_lowercase();
            if key.is_empty()
                || key.len() > 48
                || !key
                    .chars()
                    .all(|character| character.is_ascii_alphanumeric() || "_-.".contains(character))
            {
                return Err(ContainerError::Invalid(format!(
                    "érvénytelen engedély-név: `{key}`"
                )));
            }
            let value = value.trim().to_ascii_lowercase();
            if value != "allow" && value != "deny" {
                return Err(ContainerError::Invalid(format!(
                    "az engedély értéke `allow` vagy `deny` lehet (`{key}` = `{value}`)"
                )));
            }
            if normalised.len() >= MAX_PERMISSION_RULES && !normalised.contains_key(&key) {
                return Err(ContainerError::Invalid(format!(
                    "egy konténer legfeljebb {MAX_PERMISSION_RULES} engedély-szabályt hordozhat"
                )));
            }
            normalised.insert(key, value);
        }
        Ok(normalised)
    }

    /// Remove one container. Refuses while a card still points at it: deleting the group of a card that the
    /// caller did not look at would silently move it, and a silent move is how "my cards disappeared" bugs start.
    pub fn delete(&mut self, container_id: &str) -> Result<bool, ContainerError> {
        Ok(self.db.delete_container(container_id)?)
    }

    /// Replace the whole table, for an import (`profile_import`, ADR-059).
    pub fn replace_all(&mut self, containers: Vec<Container>) -> Result<usize, ContainerError> {
        let count = containers.len();
        self.db.replace_containers(&containers)?;
        Ok(count)
    }

    /// Write `containers.json` from the rows. A derived copy: nothing reads it after the switch, and a failure is
    /// reported to the caller instead of failing a user's write.
    pub fn export_json(&self) -> Result<PathBuf, ContainerError> {
        let path = self.path();
        write_json_atomic(&path, &self.list()?)?;
        Ok(path)
    }

    /// Compare the export with the rows: `(checked, mismatches)`, where `mismatches = -1` means the file could not be
    /// read at all — `not measured`, never agreement.
    pub fn verify_export(&self) -> Result<(i64, i64), ContainerError> {
        let expected = self.list()?;
        let checked = expected.len() as i64;
        let text = match fs::read_to_string(self.path()) {
            Ok(text) => text,
            Err(_) => return Ok((checked, -1)),
        };
        let exported: Vec<Container> = match serde_json::from_str(&text) {
            Ok(exported) => exported,
            Err(_) => return Ok((checked, -1)),
        };
        let mut mismatches = 0i64;
        if exported.len() != expected.len() {
            mismatches += 1;
        }
        for container in &expected {
            match exported.iter().find(|row| row.id == container.id) {
                Some(row) if row == container => {}
                _ => mismatches += 1,
            }
        }
        Ok((checked, mismatches))
    }

    /// Take the pre-switch document over exactly once: the JSON was the writer of record until this build, so it
    /// wins — and a corrupt one stops the core instead of being replaced with seeds.
    fn adopt_legacy(&mut self) -> Result<(), ContainerError> {
        let path = self.path();
        let containers = if path.exists() {
            let text = fs::read_to_string(&path)?;
            serde_json::from_str::<Vec<Container>>(&text).map_err(|source| {
                ContainerError::Legacy {
                    path: path.clone(),
                    source,
                }
            })?
        } else {
            seed_containers()
        };
        self.db.replace_containers(&containers)?;
        // The adopted document is rewritten at once, so a fresh profile still has its readable copy and the adopted
        // one carries exactly the rows the store now holds.
        self.export_json()?;
        Ok(())
    }
}

/// `#RRGGBB` only: the page puts this value straight into a style, and a colour is not a place to be clever.
fn is_color(value: &str) -> bool {
    value.len() == 7
        && value.starts_with('#')
        && value[1..]
            .chars()
            .all(|character| character.is_ascii_hexdigit())
}

/// A deterministic colour for a container that did not pick one: the same id always gets the same hue, so a
/// rename or a re-save does not make the grid flicker through colours.
fn color_for(id: &str) -> String {
    let hash = id.bytes().fold(0u32, |acc, byte| {
        acc.wrapping_mul(31).wrapping_add(u32::from(byte))
    });
    let hue = hash % 360;
    // HSL → RGB with a fixed saturation and lightness, so every generated colour is legible on both themes.
    let saturation: f64 = 0.55;
    let lightness: f64 = 0.45;
    let chroma = (1.0 - (2.0 * lightness - 1.0).abs()) * saturation;
    let sector = hue as f64 / 60.0;
    let x = chroma * (1.0 - (sector % 2.0 - 1.0).abs());
    let (red, green, blue) = match sector as u32 {
        0 => (chroma, x, 0.0),
        1 => (x, chroma, 0.0),
        2 => (0.0, chroma, x),
        3 => (0.0, x, chroma),
        4 => (x, 0.0, chroma),
        _ => (chroma, 0.0, x),
    };
    let m = lightness - chroma / 2.0;
    let channel = |value: f64| ((value + m) * 255.0).round().clamp(0.0, 255.0) as u8;
    format!(
        "#{:02X}{:02X}{:02X}",
        channel(red),
        channel(green),
        channel(blue)
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("antikolt-containers-{name}-{stamp}"));
        fs::create_dir_all(&dir).expect("temp dir");
        dir
    }

    /// A container with every field spelled out, so a test changes only what it is about.
    fn container(id: &str, name: &str) -> Container {
        Container {
            id: id.to_string(),
            name: name.to_string(),
            color: "#123456".to_string(),
            proxy_mode: default_proxy_mode(),
            proxy_host: String::new(),
            proxy_port: 0,
            proxy_bypass: String::new(),
            permissions: BTreeMap::new(),
        }
    }

    fn custom(id: &str, name: &str) -> Container {
        container(id, name)
    }

    #[test]
    fn a_fresh_store_is_seeded_in_the_database_and_exported() {
        let dir = temp_dir("seed");
        let store = ContainerStore::open(&dir).expect("open");
        let ids: Vec<String> = store
            .list()
            .expect("list")
            .into_iter()
            .map(|entry| entry.id)
            .collect();
        assert_eq!(ids, vec!["c-work", "c-private", "c-money", "c-dev"]);
        assert!(
            dir.join(FILE_NAME).exists(),
            "the export is written beside the database"
        );
        // Reopening reads the rows, not the file: the list is the same and nothing is re-seeded.
        let reopened = ContainerStore::open(&dir).expect("reopen");
        assert_eq!(reopened.list().expect("list").len(), 4);
        drop(reopened);
        drop(store);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_pre_switch_document_is_adopted_once_and_emptying_it_stays_empty() {
        let dir = temp_dir("adopt");
        fs::write(
            dir.join(FILE_NAME),
            serde_json::to_string_pretty(&vec![custom("c-egy", "Egy"), custom("c-ketto", "Kettő")])
                .expect("json"),
        )
        .expect("write the legacy document");

        let store = ContainerStore::open(&dir).expect("open");
        assert_eq!(
            store.list().expect("list"),
            vec![custom("c-egy", "Egy"), custom("c-ketto", "Kettő")],
            "the document is adopted instead of being seeded over, in its own order"
        );
        drop(store);

        // The switch is recorded, so the export is not adopted again — even when the user deletes every container.
        let mut reopened = ContainerStore::open(&dir).expect("reopen");
        for container in reopened.list().expect("list") {
            assert!(reopened.delete(&container.id).expect("delete"));
        }
        assert!(reopened.list().expect("list").is_empty());
        drop(reopened);
        let again = ContainerStore::open(&dir).expect("reopen emptied");
        assert!(
            again.list().expect("list").is_empty(),
            "an emptied container table is respected, not re-seeded"
        );
        drop(again);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn an_unreadable_legacy_document_stops_the_core() {
        let dir = temp_dir("corrupt");
        fs::write(dir.join(FILE_NAME), "{ not a container list").expect("write");
        let error = ContainerStore::open(&dir).expect_err("the open must fail loudly");
        assert!(
            matches!(error, ContainerError::Legacy { .. }),
            "the failure names the document: {error}"
        );
        assert!(
            dir.join(FILE_NAME).exists(),
            "the unreadable file is kept for the user to fix"
        );
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_container_can_be_created_renamed_and_deleted() {
        let dir = temp_dir("crud");
        let mut store = ContainerStore::open(&dir).expect("open");
        let created = store
            .upsert(Container {
                name: "  Próba  ".to_string(),
                color: String::new(),
                ..container("c-proba", "")
            })
            .expect("create");
        assert_eq!(created.name, "Próba", "the name is trimmed");
        assert!(is_color(&created.color), "a missing colour is generated");
        assert_eq!(store.list().expect("list").len(), 5);

        // A rename keeps the id and can change the colour; the id itself is the identity.
        store
            .upsert(Container {
                name: "Próba 2".to_string(),
                color: "#123456".to_string(),
                ..container("c-proba", "")
            })
            .expect("rename");
        let renamed = store
            .list()
            .expect("list")
            .into_iter()
            .find(|entry| entry.id == "c-proba")
            .expect("still there");
        assert_eq!(renamed.name, "Próba 2");
        assert_eq!(renamed.color, "#123456");

        assert!(store.delete("c-proba").expect("delete"), "deleted");
        assert!(!store.delete("c-proba").expect("delete"), "already gone");
        assert_eq!(store.list().expect("list").len(), 4);

        drop(store);
        let reopened = ContainerStore::open(&dir).expect("reopen");
        assert_eq!(reopened.list().expect("list").len(), 4);
        drop(reopened);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn nonsense_is_rejected_and_nonsense_colours_are_replaced() {
        let dir = temp_dir("invalid");
        let mut store = ContainerStore::open(&dir).expect("open");
        assert!(store
            .upsert(Container {
                color: String::new(),
                ..container("nincs-elotag", "x")
            })
            .is_err());
        assert!(store
            .upsert(Container {
                color: String::new(),
                ..container("c-", "x")
            })
            .is_err());
        assert!(store
            .upsert(Container {
                name: "   ".to_string(),
                color: String::new(),
                ..container("c-ures", "")
            })
            .is_err());
        // A colour that is not `#RRGGBB` is a cosmetic problem, not a reason to lose the save.
        let saved = store
            .upsert(Container {
                name: "Szín".to_string(),
                color: "piros".to_string(),
                ..container("c-szin", "")
            })
            .expect("save");
        assert!(is_color(&saved.color));
        assert_eq!(
            saved.color,
            color_for("c-szin"),
            "the colour is deterministic"
        );
        drop(store);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn the_proxy_is_stored_and_a_half_typed_one_is_refused() {
        let dir = temp_dir("proxy");
        let mut store = ContainerStore::open(&dir).expect("open");

        // The default is "the container inherits Chromium's own setting" — never a guess at a proxy.
        let plain = container("c-web", "Web");
        assert_eq!(plain.proxy_mode, "system");

        let fixed = store
            .upsert(Container {
                proxy_mode: "FIXED".to_string(),
                proxy_host: " 127.0.0.1 ".to_string(),
                proxy_port: 9050,
                proxy_bypass: " *.local;<local> ".to_string(),
                ..container("c-tor", "Tor")
            })
            .expect("a fixed proxy is a valid setting");
        assert_eq!(fixed.proxy_mode, "fixed", "the mode is normalised");
        assert_eq!(fixed.proxy_host, "127.0.0.1", "the host is trimmed");
        assert_eq!(fixed.proxy_port, 9050);
        assert_eq!(fixed.proxy_bypass, "*.local;<local>");

        // A mode that is not one of the three, or a fixed proxy with a missing host/port, is refused: silently falling
        // back to the system proxy would send traffic where the user did not choose.
        assert!(store
            .upsert(Container {
                proxy_mode: "socks".to_string(),
                ..container("c-rossz", "Rossz")
            })
            .is_err());
        assert!(store
            .upsert(Container {
                proxy_mode: "fixed".to_string(),
                proxy_host: "  ".to_string(),
                proxy_port: 9050,
                ..container("c-ures-host", "Üres")
            })
            .is_err());
        assert!(store
            .upsert(Container {
                proxy_mode: "fixed".to_string(),
                proxy_host: "proxy.local".to_string(),
                proxy_port: 0,
                ..container("c-ures-port", "Üres")
            })
            .is_err());

        // In `direct` mode the host and port do not mean anything, so they are not carried along.
        let direct = store
            .upsert(Container {
                proxy_mode: "direct".to_string(),
                proxy_host: "proxy.local".to_string(),
                proxy_port: 8080,
                proxy_bypass: "*.local".to_string(),
                ..container("c-direct", "Közvetlen")
            })
            .expect("direct is a valid setting");
        assert_eq!(direct.proxy_host, "");
        assert_eq!(direct.proxy_port, 0);
        assert_eq!(direct.proxy_bypass, "");

        // And it survives a restart, because it is a column now.
        drop(store);
        let reopened = ContainerStore::open(&dir).expect("reopen");
        let stored = reopened
            .list()
            .expect("list")
            .into_iter()
            .find(|entry| entry.id == "c-tor")
            .expect("still there");
        assert_eq!(stored.proxy_mode, "fixed");
        assert_eq!(stored.proxy_host, "127.0.0.1");
        assert_eq!(stored.proxy_port, 9050);
        drop(reopened);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn the_permissions_are_decisions_and_anything_else_is_refused() {
        let dir = temp_dir("permissions");
        let mut store = ContainerStore::open(&dir).expect("open");
        let mut rules = BTreeMap::new();
        rules.insert(" Geolocation ".to_string(), " ALLOW ".to_string());
        rules.insert("notifications".to_string(), "deny".to_string());
        let saved = store
            .upsert(Container {
                permissions: rules,
                ..container("c-sziget", "Sziget")
            })
            .expect("save");
        assert_eq!(
            saved.permissions.get("geolocation").map(String::as_str),
            Some("allow"),
            "keys and values are normalised"
        );
        assert_eq!(
            saved.permissions.get("notifications").map(String::as_str),
            Some("deny")
        );
        // Everything else is missing from the map on purpose: the engine grants only what is named here.
        assert_eq!(saved.permissions.len(), 2);

        for (key, value) in [
            ("geolocation", "maybe"),
            ("", "allow"),
            ("rossz nev", "allow"),
        ] {
            let mut rules = BTreeMap::new();
            rules.insert(key.to_string(), value.to_string());
            let result = store.upsert(Container {
                permissions: rules,
                ..container("c-sziget", "Sziget")
            });
            assert!(
                result.is_err(),
                "`{key}` = `{value}` is not a decision and must be refused"
            );
        }

        // A container with no rules keeps Chromium's own behaviour (which this host answers with a denial, because
        // there is no prompt surface yet) — the empty map is a policy, not a missing value.
        let plain = store
            .upsert(container("c-ures-szabaly", "Üres szabály"))
            .expect("save");
        assert!(plain.permissions.is_empty());
        drop(store);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn the_export_is_written_from_the_rows_and_can_be_checked() {
        let dir = temp_dir("export");
        let mut store = ContainerStore::open(&dir).expect("open");
        store.upsert(custom("c-uj", "Új konténer")).expect("create");
        let path = store.export_json().expect("export");
        assert!(path.exists());
        let checked = store.verify_export().expect("verify");
        assert_eq!(checked, (5, 0), "every row agrees with the export");

        // A file that no longer matches is reported, and a missing one is `not measured` rather than agreement.
        fs::write(&path, "[]").expect("clobber");
        let stale = store.verify_export().expect("verify");
        assert_eq!(stale.0, 5);
        assert!(stale.1 > 0);
        fs::remove_file(&path).expect("remove");
        assert_eq!(store.verify_export().expect("verify").1, -1);
        drop(store);
        fs::remove_dir_all(dir).ok();
    }
}
