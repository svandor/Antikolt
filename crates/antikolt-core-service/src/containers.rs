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

use crate::persist::write_json_atomic;
use crate::sqlite_store::{Db, DbError};

/// The export beside the database: the legacy document, and the readable copy this build keeps.
pub const FILE_NAME: &str = "containers.json";

/// One container, as the grid draws it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Container {
    pub id: String,
    pub name: String,
    /// `#RRGGBB`; the page uses it for the dot and the legend chip.
    pub color: String,
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

/// The containers the product ships with. Ids are stable and referenced by the cards that already exist.
fn seed_containers() -> Vec<Container> {
    vec![
        Container {
            id: "c-work".to_string(),
            name: "Munka".to_string(),
            color: "#2F6FEB".to_string(),
        },
        Container {
            id: "c-private".to_string(),
            name: "Magán".to_string(),
            color: "#2E9E5B".to_string(),
        },
        Container {
            id: "c-money".to_string(),
            name: "Pénz".to_string(),
            color: "#E3A008".to_string(),
        },
        Container {
            id: "c-dev".to_string(),
            name: "Fejlesztés".to_string(),
            color: "#8E44AD".to_string(),
        },
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
        self.db.upsert_container(&container)?;
        Ok(container)
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

    fn custom(id: &str, name: &str) -> Container {
        Container {
            id: id.to_string(),
            name: name.to_string(),
            color: "#123456".to_string(),
        }
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
                id: "c-proba".to_string(),
                name: "  Próba  ".to_string(),
                color: String::new(),
            })
            .expect("create");
        assert_eq!(created.name, "Próba", "the name is trimmed");
        assert!(is_color(&created.color), "a missing colour is generated");
        assert_eq!(store.list().expect("list").len(), 5);

        // A rename keeps the id and can change the colour; the id itself is the identity.
        store
            .upsert(Container {
                id: "c-proba".to_string(),
                name: "Próba 2".to_string(),
                color: "#123456".to_string(),
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
                id: "nincs-elotag".to_string(),
                name: "x".to_string(),
                color: String::new(),
            })
            .is_err());
        assert!(store
            .upsert(Container {
                id: "c-".to_string(),
                name: "x".to_string(),
                color: String::new(),
            })
            .is_err());
        assert!(store
            .upsert(Container {
                id: "c-ures".to_string(),
                name: "   ".to_string(),
                color: String::new(),
            })
            .is_err());
        // A colour that is not `#RRGGBB` is a cosmetic problem, not a reason to lose the save.
        let saved = store
            .upsert(Container {
                id: "c-szin".to_string(),
                name: "Szín".to_string(),
                color: "piros".to_string(),
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
