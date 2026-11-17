// SPDX-License-Identifier: MPL-2.0
//
// antikolt-probe — drives a real engine host over the protocol.
//
// This is the first end-to-end proof of the seam between the shell/core and the engine: it speaks
// exactly the JSON in crates/antikolt-ipc, creates a surface, navigates, waits for `load_finished`,
// then closes the surface and the host. If this works, the browser's two halves are talking.
//
//   cargo run -p antikolt-probe -- <path-to-antikolt-engine-host.exe> [url]

use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Duration;

use antikolt_ipc::message::Viewport;
use antikolt_ipc::{EngineEvent, EngineLink, Message, SurfaceConfig};

const LOAD_TIMEOUT: Duration = Duration::from_secs(45);

fn main() -> ExitCode {
    let mut args = std::env::args().skip(1);
    let Some(exe) = args.next() else {
        eprintln!("usage: antikolt-probe <antikolt-engine-host.exe> [url]");
        return ExitCode::from(2);
    };
    let url = args
        .next()
        .unwrap_or_else(|| "https://example.com".to_string());

    match run(PathBuf::from(exe), url) {
        Ok(()) => {
            println!("probe=ok");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("probe=failed: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run(exe: PathBuf, url: String) -> Result<(), antikolt_ipc::LinkError> {
    let requested = url.clone();
    let profile = std::env::temp_dir().join(format!(
        "antikolt-probe-{}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis())
            .unwrap_or_default()
    ));
    std::fs::create_dir_all(&profile).ok();
    let profile_arg = profile.to_string_lossy().to_string();

    let mut link = EngineLink::spawn(&exe, &["--user-data-dir", profile_arg.as_str()])?;
    println!("spawned pid={}", link.pid());

    let engine = link.handshake("antikolt-probe")?;
    println!(
        "engine={} version={} chromium_major={:?} capabilities={}",
        engine.name,
        engine.version,
        engine.chromium_major,
        engine.capabilities.len()
    );

    let config = SurfaceConfig {
        partition: "persist:probe".to_string(),
        viewport: Viewport::new(1280, 800, 100),
        proxy: None,
        is_primary: true,
    };
    let created = link.request_ok(Message::CreateSurface { config }, Duration::from_secs(20))?;
    let surface_id = created
        .get("surface")
        .and_then(|value| value.as_u64())
        .unwrap_or(0);
    println!("surface={surface_id}");

    link.request_ok(
        Message::Navigate {
            surface: antikolt_ipc::SurfaceId(surface_id),
            url: url.clone(),
            opts: Default::default(),
        },
        Duration::from_secs(20),
    )?;
    println!("navigate={url}");

    let event = link.wait_for_event(
        |event| match event {
            // The surface starts on about:blank, so only the load of *the page we asked for*
            // proves that navigation works.
            EngineEvent::LoadFinished { url, .. } => same_site(url, &requested),
            EngineEvent::LoadFailed { .. } => true,
            _ => false,
        },
        LOAD_TIMEOUT,
    )?;
    match event {
        EngineEvent::LoadFinished {
            surface,
            url,
            http_status,
        } => println!(
            "load_finished surface={} status={:?} url={url}",
            surface.0, http_status
        ),
        EngineEvent::LoadFailed {
            surface,
            url,
            reason,
        } => {
            println!(
                "load_failed surface={} url={url} reason={reason}",
                surface.0
            );
            return Err(antikolt_ipc::LinkError::Engine {
                code: "load_failed".into(),
                message: reason,
            });
        }
        _ => unreachable!("the predicate only accepts load events"),
    }

    link.request_ok(
        Message::DestroySurface {
            surface: antikolt_ipc::SurfaceId(surface_id),
        },
        Duration::from_secs(20),
    )?;
    println!("surface_destroyed={surface_id}");

    link.shutdown()?;
    println!("engine_host_exited");

    std::fs::remove_dir_all(&profile).ok();
    Ok(())
}

/// Compare loosely: the engine reports the final URL, which may differ in scheme case, a trailing
/// slash or query order from what was requested.
fn same_site(final_url: &str, requested: &str) -> bool {
    let host = |value: &str| {
        value
            .split("://")
            .nth(1)
            .unwrap_or(value)
            .split('/')
            .next()
            .unwrap_or(value)
            .to_ascii_lowercase()
    };
    !final_url.is_empty() && host(final_url) == host(requested) && !final_url.starts_with("about:")
}
