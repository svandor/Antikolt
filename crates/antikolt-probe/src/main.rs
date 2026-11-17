// SPDX-License-Identifier: MPL-2.0
//
// antikolt-probe — drives a real engine host over the protocol.
//
// Two modes:
//   antikolt-probe <engine-host.exe> [url]
//       handshake, create a surface, navigate, wait for the load, destroy, shut down.
//   antikolt-probe <engine-host.exe> [url] --freeze
//       the same, but capture a snapshot **before** destroying the surface and pause between the
//       phases with `phase=` markers, so an external measurement can see how much memory the freeze
//       actually returns (perf/measure-freeze.ps1).
//
// This is the first end-to-end proof of the seam between the shell/core and the engine: it speaks
// exactly the JSON in crates/antikolt-ipc.

use std::path::PathBuf;
use std::process::ExitCode;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use antikolt_ipc::message::Viewport;
use antikolt_ipc::{EngineEvent, EngineLink, Message, SurfaceConfig, SurfaceId};

const LOAD_TIMEOUT: Duration = Duration::from_secs(45);
const PHASE_PAUSE: Duration = Duration::from_secs(6);

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(exe) = args.first() else {
        eprintln!("usage: antikolt-probe <antikolt-engine-host.exe> [url] [--freeze]");
        return ExitCode::from(2);
    };
    let freeze = args.iter().any(|arg| arg == "--freeze");
    let url = args
        .iter()
        .skip(1)
        .find(|arg| !arg.starts_with("--"))
        .cloned()
        .unwrap_or_else(|| "https://example.com".to_string());

    match run(PathBuf::from(exe), url, freeze) {
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

fn now_ms() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis())
        .unwrap_or_default()
}

fn phase(name: &str, extra: &str) {
    println!("phase={name} ts={}{extra}", now_ms());
}

/// Compare loosely: the engine reports the final URL, which may differ from what was requested.
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

fn run(exe: PathBuf, url: String, freeze: bool) -> Result<(), antikolt_ipc::LinkError> {
    let requested = url.clone();
    let profile = std::env::temp_dir().join(format!("antikolt-probe-{}", now_ms()));
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
    let surface = SurfaceId(surface_id);
    println!("surface={surface_id}");

    link.request_ok(
        Message::Navigate {
            surface,
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
            println!("load_failed surface={} url={url} reason={reason}", surface.0);
            return Err(antikolt_ipc::LinkError::Engine {
                code: "load_failed".into(),
                message: reason,
            });
        }
        _ => unreachable!("the predicate only accepts load events"),
    }

    if freeze {
        phase("loaded", "");
        std::thread::sleep(PHASE_PAUSE);

        let snapshot = link.request_ok(Message::Snapshot { surface }, Duration::from_secs(30))?;
        let bytes = snapshot
            .get("bytes")
            .and_then(|value| value.as_i64())
            .unwrap_or(0);
        let path = snapshot
            .get("path")
            .and_then(|value| value.as_str())
            .unwrap_or("");
        let title = snapshot
            .get("title")
            .and_then(|value| value.as_str())
            .unwrap_or("");
        println!("snapshot bytes={bytes} path={path} title={title:?}");
        phase("snapshot", &format!(" bytes={bytes}"));

        std::thread::sleep(Duration::from_secs(2));
        link.request_ok(Message::DestroySurface { surface }, Duration::from_secs(20))?;
        println!("surface_destroyed={surface_id}");
        phase("frozen", "");

        // Stay alive with no surface so the measurement can see the memory that was returned.
        std::thread::sleep(PHASE_PAUSE);
    } else {
        link.request_ok(Message::DestroySurface { surface }, Duration::from_secs(20))?;
        println!("surface_destroyed={surface_id}");
    }

    link.shutdown()?;
    println!("engine_host_exited");
    phase("done", "");

    // Freeze mode keeps the profile: the snapshot file lives there and must be inspectable.
    if freeze {
        println!("profile={}", profile.display());
    } else {
        std::fs::remove_dir_all(&profile).ok();
    }
    Ok(())
}
