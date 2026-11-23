// SPDX-License-Identifier: MPL-2.0

//! Password CSV import: Chrome's and Firefox's exported password lists become vault credentials.
//!
//! Both exporters share the `url`, `username` and `password` columns; Chrome adds `name` (the label),
//! Firefox adds several bookkeeping columns. Columns are mapped by **name**, not position, so either
//! exporter's order — and a UTF-8 BOM in front of the header — is accepted. A row without a URL or a
//! password is skipped rather than imported as a credential that would mislead the autofill later.

use crate::credentials::NewCredential;

/// One parsed row, ready to become a `NewCredential`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImportRow {
    pub origin: String,
    pub username: String,
    pub password: String,
    pub label: Option<String>,
}

/// What a parse produced: the valid rows plus how many were refused.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedCsv {
    pub rows: Vec<ImportRow>,
    pub skipped: usize,
}

/// Parse a Chrome or Firefox password CSV export.
pub fn parse_credential_csv(text: &str) -> Result<ParsedCsv, String> {
    let mut reader = csv::ReaderBuilder::new()
        .trim(csv::Trim::All)
        .from_reader(text.as_bytes());
    let headers = reader
        .headers()
        .map_err(|error| format!("cannot read the CSV header: {error}"))?
        .clone();

    let column = |name: &str| -> Option<usize> {
        headers.iter().position(|header| {
            header
                .trim_start_matches('\u{feff}')
                .eq_ignore_ascii_case(name)
        })
    };
    let url_col = column("url").ok_or("the CSV has no `url` column")?;
    let username_col = column("username");
    let password_col = column("password").ok_or("the CSV has no `password` column")?;
    let name_col = column("name");

    let mut rows = Vec::new();
    let mut skipped = 0usize;
    for record in reader.records() {
        let record = record.map_err(|error| format!("cannot read a CSV row: {error}"))?;
        let field = |col: Option<usize>| -> String {
            col.and_then(|index| record.get(index))
                .unwrap_or("")
                .to_string()
        };
        let origin = field(Some(url_col)).trim().to_string();
        let username = field(username_col);
        let password = field(Some(password_col));
        let label = name_col
            .and_then(|index| record.get(index))
            .map(str::to_string)
            .filter(|value| !value.is_empty());
        if origin.is_empty() || password.is_empty() {
            skipped += 1;
            continue;
        }
        rows.push(ImportRow {
            origin,
            username,
            password,
            label,
        });
    }
    Ok(ParsedCsv { rows, skipped })
}

/// Turn parsed rows into the vault's create-request shape (`kind = password`).
pub fn to_new_credentials(
    rows: Vec<ImportRow>,
    container_id: Option<String>,
) -> Vec<NewCredential> {
    rows.into_iter()
        .map(|row| NewCredential {
            origin: row.origin,
            kind: "password".to_string(),
            container_id: container_id.clone(),
            label: row.label,
            username: Some(row.username).filter(|value| !value.is_empty()),
            password: Some(row.password),
            totp_secret: None,
            totp_params: None,
            passkey: None,
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chrome_columns_map_by_name_in_any_order() {
        // Chrome: name,url,username,password — here shuffled to prove name-based mapping.
        let csv = "url,password,name,username\nhttps://a.hu,secret,Alfa,alice\n";
        let parsed = parse_credential_csv(csv).expect("parse");
        assert_eq!(parsed.rows.len(), 1);
        assert_eq!(parsed.rows[0].origin, "https://a.hu");
        assert_eq!(parsed.rows[0].username, "alice");
        assert_eq!(parsed.rows[0].password, "secret");
        assert_eq!(parsed.rows[0].label.as_deref(), Some("Alfa"));
    }

    #[test]
    fn firefox_columns_are_accepted() {
        let csv = "\"url\",\"username\",\"password\",\"httpRealm\",\"formActionOrigin\",\"guid\",\
                   \"timeCreated\",\"timeLastUsed\",\"timePasswordChanged\"\n\
                   \"https://b.hu\",\"bob\",\"hunter2\",\"\",\"\",\"g1\",\"1\",\"2\",\"3\"\n";
        let parsed = parse_credential_csv(csv).expect("parse");
        assert_eq!(parsed.rows.len(), 1);
        assert_eq!(parsed.rows[0].origin, "https://b.hu");
        assert_eq!(parsed.rows[0].username, "bob");
        assert_eq!(parsed.rows[0].password, "hunter2");
    }

    #[test]
    fn a_utf8_bom_in_front_of_the_header_is_ignored() {
        let csv = "\u{feff}url,username,password\nhttps://c.hu,carol,secret\n";
        let parsed = parse_credential_csv(csv).expect("parse");
        assert_eq!(parsed.rows.len(), 1);
        assert_eq!(parsed.rows[0].origin, "https://c.hu");
    }

    #[test]
    fn quoted_commas_and_missing_fields_are_handled() {
        let csv = "url,username,password,name\n\
                   \"https://d.hu\",\"da,vid\",\"p,ass\",\"Dé\"\n\
                   https://e.hu,,,NoName\n";
        let parsed = parse_credential_csv(csv).expect("parse");
        // The d.hu row is imported with its quoted commas intact; the e.hu row has no password and is skipped.
        assert_eq!(parsed.rows.len(), 1);
        assert_eq!(parsed.rows[0].username, "da,vid");
        assert_eq!(parsed.rows[0].password, "p,ass");
        assert_eq!(parsed.rows[0].label.as_deref(), Some("Dé"));
        assert_eq!(parsed.skipped, 1);
    }

    #[test]
    fn a_row_without_url_or_password_is_skipped() {
        let csv =
            "url,username,password\n,someone,secret\nhttps://f.hu,,secret\nhttps://g.hu,user,\n";
        let parsed = parse_credential_csv(csv).expect("parse");
        assert_eq!(parsed.rows.len(), 1, "only the f.hu row is complete");
        assert_eq!(parsed.rows[0].origin, "https://f.hu");
        assert_eq!(parsed.skipped, 2);
    }

    #[test]
    fn rows_become_password_credentials() {
        let rows = vec![ImportRow {
            origin: "https://h.hu".to_string(),
            username: "henry".to_string(),
            password: "pw".to_string(),
            label: Some("H".to_string()),
        }];
        let credentials = to_new_credentials(rows, Some("c-private".to_string()));
        assert_eq!(credentials.len(), 1);
        assert_eq!(credentials[0].kind, "password");
        assert_eq!(credentials[0].origin, "https://h.hu");
        assert_eq!(credentials[0].username.as_deref(), Some("henry"));
        assert_eq!(credentials[0].password.as_deref(), Some("pw"));
    }
}
