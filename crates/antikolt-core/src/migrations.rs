// SPDX-License-Identifier: MPL-2.0

//! Schema migrations.
//!
//! The SQL lives in `migrations/*.sql` and is embedded at compile time, so a released binary can
//! always create its own schema without shipping loose files. Migrations are **append-only**: a
//! shipped migration is never edited, only followed by a new one.
//!
//! The storage layer (not yet written — see the development log) is responsible for the two
//! mechanical parts: applying the pragmas from [`sqlite_pragmas`] and recording the applied
//! version in `PRAGMA user_version`.

/// One migration file.
#[derive(Debug, Clone, Copy)]
pub struct Migration {
    pub version: u32,
    pub name: &'static str,
    pub sql: &'static str,
}

/// Every migration this build knows, in ascending version order.
pub const MIGRATIONS: &[Migration] = &[Migration {
    version: 1,
    name: "init",
    sql: include_str!("../migrations/0001_init.sql"),
}];

/// The schema version a freshly migrated database reports.
pub const SCHEMA_VERSION: u32 = 1;

/// Pragmas the storage layer must apply on every connection, before anything else.
///
/// `WAL` gives readers and the writer concurrency; `foreign_keys` is off by default in SQLite and
/// must be enabled per connection; `synchronous=NORMAL` is the documented WAL pairing; the
/// busy timeout keeps a second process from failing instantly instead of waiting.
pub fn sqlite_pragmas() -> &'static str {
    "PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;"
}

/// The tables and virtual tables the schema must contain. Used by tests and by a startup
/// self-check that refuses to run against a half-migrated database.
pub const REQUIRED_TABLES: &[&str] = &[
    "app_meta",
    "workspace",
    "container",
    "card",
    "snapshot",
    "pending_maintenance",
    "credential",
    "session",
    "site_recipe",
    "tag",
    "card_tag",
    "event",
    "card_fts",
    "card_fts_trigram",
];

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum MigrationError {
    #[error("the migration list is empty")]
    Empty,
    #[error("the first migration must be version 1, found {0}")]
    WrongFirstVersion(u32),
    #[error("migration versions must be contiguous: expected {expected}, found {found}")]
    NonContiguous { expected: u32, found: u32 },
    #[error("duplicate migration name {0:?}")]
    DuplicateName(&'static str),
    #[error("migration {0:?} has empty SQL")]
    EmptySql(&'static str),
    #[error("migration {0:?} still contains an unresolved {1:?} marker")]
    UnresolvedMarker(&'static str, &'static str),
}

/// Validate a migration list: contiguous from 1, unique names, non-empty SQL, no leftover markers.
pub fn validate(migrations: &[Migration]) -> Result<(), MigrationError> {
    let Some(first) = migrations.first() else {
        return Err(MigrationError::Empty);
    };
    if first.version != 1 {
        return Err(MigrationError::WrongFirstVersion(first.version));
    }

    let mut seen_names: Vec<&'static str> = Vec::new();
    for (index, migration) in migrations.iter().enumerate() {
        let expected = index as u32 + 1;
        if migration.version != expected {
            return Err(MigrationError::NonContiguous {
                expected,
                found: migration.version,
            });
        }
        if seen_names.contains(&migration.name) {
            return Err(MigrationError::DuplicateName(migration.name));
        }
        seen_names.push(migration.name);
        if migration.sql.trim().is_empty() {
            return Err(MigrationError::EmptySql(migration.name));
        }
        for marker in ["TODO", "FIXME", "XXX"] {
            if migration.sql.contains(marker) {
                return Err(MigrationError::UnresolvedMarker(migration.name, marker));
            }
        }
    }
    Ok(())
}

/// Versions that still need to be applied on a database currently at `current_version`.
pub fn pending(current_version: u32) -> impl Iterator<Item = u32> {
    MIGRATIONS
        .iter()
        .map(|m| m.version)
        .filter(move |v| *v > current_version)
}

/// Case-insensitive check that a migration creates the given table or virtual table.
///
/// Deliberately textual: it exists so the test suite fails loudly if a documented table is missing,
/// not to be a SQL parser.
pub fn creates_table(sql: &str, table: &str) -> bool {
    let lowered = sql.to_ascii_lowercase();
    let collapsed = lowered.split_whitespace().collect::<Vec<_>>().join(" ");
    collapsed.contains(&format!("create table {table}"))
        || collapsed.contains(&format!("create virtual table {table}"))
        || collapsed.contains(&format!("create table if not exists {table}"))
        || collapsed.contains(&format!("create virtual table if not exists {table}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_shipped_migrations_are_valid() {
        validate(MIGRATIONS).unwrap();
        assert_eq!(MIGRATIONS.len() as u32, SCHEMA_VERSION);
    }

    #[test]
    fn every_required_table_is_created() {
        let all: String = MIGRATIONS
            .iter()
            .map(|m| m.sql)
            .collect::<Vec<_>>()
            .join("\n");
        for table in REQUIRED_TABLES {
            assert!(creates_table(&all, table), "missing table {table}");
        }
    }

    #[test]
    fn the_documented_columns_exist() {
        // A few load-bearing columns that the documented queries depend on.
        let card = MIGRATIONS[0].sql;
        for column in [
            "session_policy",
            "theme_technique",
            "luminance_permille",
            "keep_alive",
            "partition_key",
        ] {
            assert!(card.contains(column), "missing column {column}");
        }
    }

    #[test]
    fn pragmas_cover_wal_foreign_keys_and_timeout() {
        let pragmas = sqlite_pragmas();
        assert!(pragmas.contains("journal_mode = WAL"));
        assert!(pragmas.contains("foreign_keys = ON"));
        assert!(pragmas.contains("busy_timeout"));
        assert!(pragmas.contains("synchronous = NORMAL"));
    }

    #[test]
    fn fresh_database_has_every_migration_pending() {
        assert_eq!(pending(0).collect::<Vec<_>>(), vec![1]);
        assert!(pending(SCHEMA_VERSION).next().is_none());
    }

    #[test]
    fn invalid_migration_lists_are_rejected() {
        assert_eq!(validate(&[]), Err(MigrationError::Empty));

        let wrong_first = [Migration {
            version: 2,
            name: "x",
            sql: "select 1;",
        }];
        assert_eq!(
            validate(&wrong_first),
            Err(MigrationError::WrongFirstVersion(2))
        );

        let gap = [
            Migration {
                version: 1,
                name: "a",
                sql: "select 1;",
            },
            Migration {
                version: 3,
                name: "c",
                sql: "select 1;",
            },
        ];
        assert_eq!(
            validate(&gap),
            Err(MigrationError::NonContiguous {
                expected: 2,
                found: 3
            })
        );

        let duplicate_names = [
            Migration {
                version: 1,
                name: "same",
                sql: "select 1;",
            },
            Migration {
                version: 2,
                name: "same",
                sql: "select 1;",
            },
        ];
        assert_eq!(
            validate(&duplicate_names),
            Err(MigrationError::DuplicateName("same"))
        );

        let empty_sql = [Migration {
            version: 1,
            name: "empty",
            sql: "  \n ",
        }];
        assert_eq!(validate(&empty_sql), Err(MigrationError::EmptySql("empty")));

        let marker = [Migration {
            version: 1,
            name: "todo",
            sql: "-- TODO: write the schema",
        }];
        assert_eq!(
            validate(&marker),
            Err(MigrationError::UnresolvedMarker("todo", "TODO"))
        );
    }
}
