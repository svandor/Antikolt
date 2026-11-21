// SPDX-License-Identifier: MPL-2.0

//! The one place an export is written (ADR-069/ADR-070).
//!
//! Since the stores live in SQLite, the four JSON documents beside the database are **derived**: they are written
//! from the rows, never read back after the switch. They still matter — a person can open them, a downgrade or a
//! bug report can use them — so the write is atomic (a temporary file, `fsync`, then a rename): a crash must never
//! leave half a card list where a backup used to be.

use std::fs;
use std::io::Write;
use std::path::Path;

use serde::Serialize;

/// Write `value` as pretty JSON to `path`, atomically.
///
/// The serialisation error is folded into an `io::Error` because every caller already reports an I/O failure for the
/// same file; a store's export failing is one problem, not two.
pub fn write_json_atomic<T: Serialize>(path: &Path, value: &T) -> std::io::Result<()> {
    let text = serde_json::to_string_pretty(value).map_err(std::io::Error::other)?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("json.tmp");
    {
        let mut file = fs::File::create(&temporary)?;
        file.write_all(text.as_bytes())?;
        // The rename below is only atomic if the bytes really reached the disk first.
        file.sync_all()?;
    }
    fs::rename(&temporary, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_export_replaces_the_file_in_one_step_and_leaves_no_temporary() {
        let dir = std::env::temp_dir().join(format!(
            "antikolt-persist-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("clock")
                .as_nanos()
        ));
        fs::create_dir_all(&dir).expect("temp dir");
        let path = dir.join("store.json");
        write_json_atomic(&path, &["első"]).expect("first write");
        write_json_atomic(&path, &["második"]).expect("second write");
        let text = fs::read_to_string(&path).expect("read");
        assert!(text.contains("második"), "the new content won: {text}");
        assert!(
            !path.with_extension("json.tmp").exists(),
            "the temporary file is renamed away, not left behind"
        );
        fs::remove_dir_all(dir).ok();
    }
}
