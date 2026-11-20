// SPDX-License-Identifier: MPL-2.0

//! The container table the core serves.
//!
//! A container is the colour-coded grouping a card belongs to (docs/HU/02 §7, table `container`). Until now the
//! shell kept this table in the *page* (`PAGE_CONTAINERS` in `home/app.js`), which meant a rename lived in a
//! JSON constant and the colour a card showed was decided by whichever layer drew it. This store moves the table
//! where every other fact lives: the core.
//!
//! Like the card store it is one JSON document, rewritten atomically, and additive: a profile that predates it
//! is seeded with the four containers the product shipped with, so nothing the user sees disappears.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

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
    #[error("invalid container: {0}")]
    Invalid(String),
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

/// The container table, backed by one JSON file.
#[derive(Debug)]
pub struct ContainerStore {
    path: PathBuf,
    containers: Vec<Container>,
}

impl ContainerStore {
    /// Open (or create) the store next to the cards.
    pub fn open(directory: &Path) -> Result<Self, ContainerError> {
        fs::create_dir_all(directory)?;
        let path = directory.join("containers.json");
        let containers = match fs::read_to_string(&path) {
            Ok(text) => serde_json::from_str::<Vec<Container>>(&text)?,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                let seeded = seed_containers();
                let store = Self {
                    path,
                    containers: seeded,
                };
                store.save()?;
                return Ok(store);
            }
            Err(error) => return Err(error.into()),
        };
        Ok(Self { path, containers })
    }

    pub fn list(&self) -> Vec<Container> {
        self.containers.clone()
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
        match self
            .containers
            .iter_mut()
            .find(|entry| entry.id == container.id)
        {
            Some(existing) => *existing = container.clone(),
            None => self.containers.push(container.clone()),
        }
        self.save()?;
        Ok(container)
    }

    /// Remove one container. Refuses while a card still points at it: deleting the group of a card that the
    /// caller did not look at would silently move it, and a silent move is how "my cards disappeared" bugs start.
    pub fn delete(&mut self, container_id: &str) -> Result<bool, ContainerError> {
        let Some(index) = self
            .containers
            .iter()
            .position(|entry| entry.id == container_id)
        else {
            return Ok(false);
        };
        self.containers.remove(index);
        self.save()?;
        Ok(true)
    }

    /// Replace the whole table, for an import (`profile_import`, ADR-059).
    pub fn replace_all(&mut self, containers: Vec<Container>) -> Result<usize, ContainerError> {
        self.containers = containers;
        let count = self.containers.len();
        self.save()?;
        Ok(count)
    }

    fn save(&self) -> Result<(), ContainerError> {
        let text = serde_json::to_string_pretty(&self.containers)?;
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

    #[test]
    fn a_fresh_store_is_seeded_with_the_shipped_containers() {
        let dir = temp_dir("seed");
        let store = ContainerStore::open(&dir).expect("open");
        let ids: Vec<String> = store.list().into_iter().map(|entry| entry.id).collect();
        assert_eq!(ids, vec!["c-work", "c-private", "c-money", "c-dev"]);
        assert!(dir.join("containers.json").exists(), "the seed is on disk");
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
        assert_eq!(store.list().len(), 5);

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
            .into_iter()
            .find(|entry| entry.id == "c-proba")
            .expect("still there");
        assert_eq!(renamed.name, "Próba 2");
        assert_eq!(renamed.color, "#123456");

        assert!(store.delete("c-proba").expect("delete"), "deleted");
        assert!(!store.delete("c-proba").expect("delete"), "already gone");
        assert_eq!(store.list().len(), 4);
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
        fs::remove_dir_all(dir).ok();
    }
}
