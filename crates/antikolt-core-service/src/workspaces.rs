// SPDX-License-Identifier: MPL-2.0

//! The workspace table the core serves.
//!
//! A workspace is the top-level tab a card lives under (`w-home`, `w-work`, …). Until now the page invented the
//! assignment (`index > 12 ? "w-read" : "w-home"`) — which meant the thirteenth card silently claimed to belong
//! to "Olvasnivaló". The table and the assignment belong where every other fact lives: the core (ADR-056).
//!
//! Since ADR-070 the table is **SQLite-first**: the rows are the truth and `workspaces.json` is a derived export,
//! written on adoption, on shutdown and when `db_status` asks for a sync — read once, to adopt a pre-switch profile.

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::persist::write_json_atomic;
use crate::sqlite_store::{Db, DbError};

/// The export beside the database: the legacy document, and the readable copy this build keeps.
pub const FILE_NAME: &str = "workspaces.json";

/// One workspace, as the tab row shows it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Workspace {
    pub id: String,
    pub name: String,
}

#[derive(Debug, thiserror::Error)]
pub enum WorkspaceError {
    #[error("cannot read or write the workspace store: {0}")]
    Io(#[from] std::io::Error),
    #[error("the workspace store is not valid JSON: {0}")]
    Json(#[from] serde_json::Error),
    /// The pre-switch document could not be read: it was the only copy of the tab row at that moment.
    #[error("cannot read the workspace list written by an earlier version ({path}): {source}")]
    Legacy {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("invalid workspace: {0}")]
    Invalid(String),
    #[error("the database refused the workspace table: {0}")]
    Db(#[from] DbError),
}

fn seed_workspaces() -> Vec<Workspace> {
    vec![
        Workspace {
            id: "w-home".to_string(),
            name: "Otthon".to_string(),
        },
        Workspace {
            id: "w-work".to_string(),
            name: "Munka".to_string(),
        },
        Workspace {
            id: "w-read".to_string(),
            name: "Olvasnivaló".to_string(),
        },
    ]
}

/// The workspace table: rows in SQLite, with `workspaces.json` as a derived export.
#[derive(Debug)]
pub struct WorkspaceStore {
    data_dir: PathBuf,
    db: Db,
}

impl WorkspaceStore {
    /// Open the store, adopting the pre-switch `workspaces.json` once (ADR-070). A missing document is seeded with the
    /// three workspaces the product shipped with; one that exists — even empty — is adopted as it is.
    pub fn open(directory: &Path) -> Result<Self, WorkspaceError> {
        fs::create_dir_all(directory)?;
        let db = Db::open(directory)?;
        let mut store = Self {
            data_dir: directory.to_path_buf(),
            db,
        };
        if !store.db.was_adopted("workspaces")? {
            store.adopt_legacy()?;
            store.db.mark_adopted("workspaces")?;
        }
        Ok(store)
    }

    /// The export's path inside the profile.
    pub fn path(&self) -> PathBuf {
        self.data_dir.join(FILE_NAME)
    }

    pub fn list(&self) -> Result<Vec<Workspace>, WorkspaceError> {
        Ok(self.db.all_workspaces()?)
    }

    /// Create or rename one workspace. The id must look like `w-…`, and the name must not be empty.
    pub fn upsert(&mut self, mut workspace: Workspace) -> Result<Workspace, WorkspaceError> {
        workspace.id = workspace.id.trim().to_string();
        if !workspace.id.starts_with("w-") || workspace.id.len() < 3 {
            return Err(WorkspaceError::Invalid(
                "a munkaterület id `w-` előtaggal kezdődik".to_string(),
            ));
        }
        workspace.name = workspace.name.trim().to_string();
        if workspace.name.is_empty() {
            return Err(WorkspaceError::Invalid(
                "a munkaterület neve nem lehet üres".to_string(),
            ));
        }
        self.db.upsert_workspace(&workspace)?;
        Ok(workspace)
    }

    /// Remove one workspace. Like the containers, the caller decides about the cards: the service refuses while
    /// a card still points at it.
    pub fn delete(&mut self, workspace_id: &str) -> Result<bool, WorkspaceError> {
        Ok(self.db.delete_workspace(workspace_id)?)
    }

    /// Replace the whole table, for an import (`profile_import`, ADR-059).
    pub fn replace_all(&mut self, workspaces: Vec<Workspace>) -> Result<usize, WorkspaceError> {
        let count = workspaces.len();
        self.db.replace_workspaces(&workspaces)?;
        Ok(count)
    }

    /// Write `workspaces.json` from the rows; nothing reads it after the switch.
    pub fn export_json(&self) -> Result<PathBuf, WorkspaceError> {
        let path = self.path();
        write_json_atomic(&path, &self.list()?)?;
        Ok(path)
    }

    /// Compare the export with the rows: `(checked, mismatches)`, `-1` meaning the file is unreadable (`not measured`).
    pub fn verify_export(&self) -> Result<(i64, i64), WorkspaceError> {
        let expected = self.list()?;
        let checked = expected.len() as i64;
        let text = match fs::read_to_string(self.path()) {
            Ok(text) => text,
            Err(_) => return Ok((checked, -1)),
        };
        let exported: Vec<Workspace> = match serde_json::from_str(&text) {
            Ok(exported) => exported,
            Err(_) => return Ok((checked, -1)),
        };
        let mut mismatches = 0i64;
        if exported.len() != expected.len() {
            mismatches += 1;
        }
        for workspace in &expected {
            match exported.iter().find(|row| row.id == workspace.id) {
                Some(row) if row == workspace => {}
                _ => mismatches += 1,
            }
        }
        Ok((checked, mismatches))
    }

    /// Take the pre-switch document over exactly once: the JSON was the writer of record until this build.
    fn adopt_legacy(&mut self) -> Result<(), WorkspaceError> {
        let path = self.path();
        let workspaces = if path.exists() {
            let text = fs::read_to_string(&path)?;
            serde_json::from_str::<Vec<Workspace>>(&text).map_err(|source| {
                WorkspaceError::Legacy {
                    path: path.clone(),
                    source,
                }
            })?
        } else {
            seed_workspaces()
        };
        self.db.replace_workspaces(&workspaces)?;
        self.export_json()?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("antikolt-workspaces-{name}-{stamp}"));
        fs::create_dir_all(&dir).expect("temp dir");
        dir
    }

    fn custom(id: &str, name: &str) -> Workspace {
        Workspace {
            id: id.to_string(),
            name: name.to_string(),
        }
    }

    #[test]
    fn a_fresh_store_is_seeded_in_the_database_and_exported() {
        let dir = temp_dir("seed");
        let store = WorkspaceStore::open(&dir).expect("open");
        let ids: Vec<String> = store
            .list()
            .expect("list")
            .into_iter()
            .map(|entry| entry.id)
            .collect();
        assert_eq!(ids, vec!["w-home", "w-work", "w-read"]);
        assert!(
            dir.join(FILE_NAME).exists(),
            "the export is written beside the database"
        );
        let reopened = WorkspaceStore::open(&dir).expect("reopen");
        assert_eq!(reopened.list().expect("list").len(), 3);
        drop(reopened);
        drop(store);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_pre_switch_document_is_adopted_once_and_emptying_it_stays_empty() {
        let dir = temp_dir("adopt");
        fs::write(
            dir.join(FILE_NAME),
            serde_json::to_string_pretty(&vec![custom("w-egy", "Egy"), custom("w-ketto", "Kettő")])
                .expect("json"),
        )
        .expect("write the legacy document");

        let store = WorkspaceStore::open(&dir).expect("open");
        assert_eq!(
            store.list().expect("list"),
            vec![custom("w-egy", "Egy"), custom("w-ketto", "Kettő")],
            "the document is adopted in its own tab order"
        );
        drop(store);

        let mut reopened = WorkspaceStore::open(&dir).expect("reopen");
        for workspace in reopened.list().expect("list") {
            assert!(reopened.delete(&workspace.id).expect("delete"));
        }
        drop(reopened);
        let again = WorkspaceStore::open(&dir).expect("reopen emptied");
        assert!(
            again.list().expect("list").is_empty(),
            "an emptied tab row is respected, not re-seeded"
        );
        drop(again);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn an_unreadable_legacy_document_stops_the_core() {
        let dir = temp_dir("corrupt");
        fs::write(dir.join(FILE_NAME), "{ not a workspace list").expect("write");
        let error = WorkspaceStore::open(&dir).expect_err("the open must fail loudly");
        assert!(
            matches!(error, WorkspaceError::Legacy { .. }),
            "the failure names the document: {error}"
        );
        assert!(dir.join(FILE_NAME).exists());
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn a_workspace_can_be_created_renamed_and_deleted() {
        let dir = temp_dir("crud");
        let mut store = WorkspaceStore::open(&dir).expect("open");
        store
            .upsert(Workspace {
                id: "w-proba".to_string(),
                name: "  Próba  ".to_string(),
            })
            .expect("create");
        let renamed = store
            .upsert(Workspace {
                id: "w-proba".to_string(),
                name: "Próba 2".to_string(),
            })
            .expect("rename");
        assert_eq!(renamed.name, "Próba 2");
        assert_eq!(store.list().expect("list").len(), 4);
        assert!(store.delete("w-proba").expect("delete"));
        assert!(!store.delete("w-proba").expect("delete"), "already gone");
        assert_eq!(store.list().expect("list").len(), 3);
        drop(store);
        let reopened = WorkspaceStore::open(&dir).expect("reopen");
        assert_eq!(reopened.list().expect("list").len(), 3);
        drop(reopened);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn nonsense_is_rejected() {
        let dir = temp_dir("invalid");
        let mut store = WorkspaceStore::open(&dir).expect("open");
        assert!(store
            .upsert(Workspace {
                id: "nincs-elotag".to_string(),
                name: "x".to_string(),
            })
            .is_err());
        assert!(store
            .upsert(Workspace {
                id: "w-".to_string(),
                name: "x".to_string(),
            })
            .is_err());
        assert!(store
            .upsert(Workspace {
                id: "w-ures".to_string(),
                name: "   ".to_string(),
            })
            .is_err());
        drop(store);
        fs::remove_dir_all(dir).ok();
    }

    #[test]
    fn the_export_is_written_from_the_rows_and_can_be_checked() {
        let dir = temp_dir("export");
        let mut store = WorkspaceStore::open(&dir).expect("open");
        store.upsert(custom("w-uj", "Új tér")).expect("create");
        let path = store.export_json().expect("export");
        assert_eq!(store.verify_export().expect("verify"), (4, 0));
        fs::write(&path, "[]").expect("clobber");
        let stale = store.verify_export().expect("verify");
        assert_eq!(stale.0, 4);
        assert!(stale.1 > 0);
        fs::remove_file(&path).expect("remove");
        assert_eq!(store.verify_export().expect("verify").1, -1);
        drop(store);
        fs::remove_dir_all(dir).ok();
    }
}
