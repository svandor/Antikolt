// SPDX-License-Identifier: MPL-2.0

//! The workspace table the core serves.
//!
//! A workspace is the top-level tab a card lives under (`w-home`, `w-work`, …). Until now the page invented the
//! assignment (`index > 12 ? "w-read" : "w-home"`) — which meant the thirteenth card silently claimed to belong
//! to "Olvasnivaló". The table and the assignment belong where every other fact lives: the core (ADR-056).
//!
//! Like the container store it is one JSON document, rewritten atomically, and additive: a profile that predates
//! it is seeded with the three workspaces the product shipped with.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

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
    #[error("invalid workspace: {0}")]
    Invalid(String),
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

/// The workspace table, backed by one JSON file.
#[derive(Debug)]
pub struct WorkspaceStore {
    path: PathBuf,
    workspaces: Vec<Workspace>,
}

impl WorkspaceStore {
    pub fn open(directory: &Path) -> Result<Self, WorkspaceError> {
        fs::create_dir_all(directory)?;
        let path = directory.join("workspaces.json");
        let workspaces = match fs::read_to_string(&path) {
            Ok(text) => serde_json::from_str::<Vec<Workspace>>(&text)?,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                let store = Self {
                    path,
                    workspaces: seed_workspaces(),
                };
                store.save()?;
                return Ok(store);
            }
            Err(error) => return Err(error.into()),
        };
        Ok(Self { path, workspaces })
    }

    pub fn list(&self) -> Vec<Workspace> {
        self.workspaces.clone()
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
        match self
            .workspaces
            .iter_mut()
            .find(|entry| entry.id == workspace.id)
        {
            Some(existing) => *existing = workspace.clone(),
            None => self.workspaces.push(workspace.clone()),
        }
        self.save()?;
        Ok(workspace)
    }

    /// Remove one workspace. Like the containers, the caller decides about the cards: the service refuses while
    /// a card still points at it.
    pub fn delete(&mut self, workspace_id: &str) -> Result<bool, WorkspaceError> {
        let Some(index) = self
            .workspaces
            .iter()
            .position(|entry| entry.id == workspace_id)
        else {
            return Ok(false);
        };
        self.workspaces.remove(index);
        self.save()?;
        Ok(true)
    }

    /// Replace the whole table, for an import (`profile_import`, ADR-059).
    pub fn replace_all(&mut self, workspaces: Vec<Workspace>) -> Result<usize, WorkspaceError> {
        self.workspaces = workspaces;
        let count = self.workspaces.len();
        self.save()?;
        Ok(count)
    }

    fn save(&self) -> Result<(), WorkspaceError> {
        let text = serde_json::to_string_pretty(&self.workspaces)?;
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent)?;
        }
        let temporary = self.path.with_extension("json.tmp");
        {
            let mut file = fs::File::create(&temporary)?;
            file.write_all(text.as_bytes())?;
            file.sync_all()?;
        }
        fs::rename(&temporary, &self.path)?;
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

    #[test]
    fn a_fresh_store_is_seeded_with_the_shipped_workspaces() {
        let dir = temp_dir("seed");
        let store = WorkspaceStore::open(&dir).expect("open");
        let ids: Vec<String> = store.list().into_iter().map(|entry| entry.id).collect();
        assert_eq!(ids, vec!["w-home", "w-work", "w-read"]);
        assert!(dir.join("workspaces.json").exists(), "the seed is on disk");
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
        assert_eq!(store.list().len(), 4);
        assert!(store.delete("w-proba").expect("delete"));
        assert!(!store.delete("w-proba").expect("delete"), "already gone");
        assert_eq!(store.list().len(), 3);
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
        fs::remove_dir_all(dir).ok();
    }
}
