// SPDX-License-Identifier: MPL-2.0
//
// antikolt-probe — drives a real engine host over the protocol.
//
// Modes:
//   antikolt-probe <engine-host.exe> [url]                load a page, then destroy the surface
//   antikolt-probe <engine-host.exe> [url] --freeze       snapshot (MHTML + state), then freeze
//   antikolt-probe <engine-host.exe> <file-url> --restore <state.json>
//                                                         open a snapshot offline and put the page
//                                                         state back (scroll, forms, storage, media)
//
// This is the end-to-end proof of the seam between the shell/core and the engine: it speaks exactly
// the JSON in crates/antikolt-ipc.

use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use antikolt_ipc::message::{Viewport, World};
use antikolt_ipc::{EngineEvent, EngineLink, Message, SurfaceConfig, SurfaceId};

const LOAD_TIMEOUT: Duration = Duration::from_secs(45);
const PHASE_PAUSE: Duration = Duration::from_secs(6);
const EVAL_TIMEOUT: Duration = Duration::from_secs(30);

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(exe) = args.first() else {
        eprintln!(
            "usage: antikolt-probe <antikolt-engine-host.exe> [url] [--freeze]\n\
             antikolt-probe <antikolt-engine-host.exe> <file-url> --restore <state.json>"
        );
        return ExitCode::from(2);
    };

    let freeze = args.iter().any(|arg| arg == "--freeze");
    let restore_path = args
        .iter()
        .position(|arg| arg == "--restore")
        .and_then(|index| args.get(index + 1))
        .cloned();
    let url = args
        .iter()
        .skip(1)
        .find(|arg| !arg.starts_with("--"))
        .cloned()
        .unwrap_or_else(|| "https://example.com".to_string());

    let result = match restore_path {
        Some(state) => run_restore(PathBuf::from(exe), url, PathBuf::from(state)),
        None => run(PathBuf::from(exe), url, freeze),
    };

    match result {
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
fn same_host(final_url: &str, requested: &str) -> bool {
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

/// Start an engine host and open one surface on `url`.
fn open_surface(exe: &Path, url: &str) -> Result<(EngineLink, SurfaceId), antikolt_ipc::LinkError> {
    let profile = std::env::temp_dir().join(format!("antikolt-probe-{}", now_ms()));
    std::fs::create_dir_all(&profile).ok();
    let profile_arg = profile.to_string_lossy().to_string();

    let mut link = EngineLink::spawn(exe, &["--user-data-dir", profile_arg.as_str()])?;
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
        // The probe never embeds: the engine host owns the surface's window in its own process.
        parent_window: 0,
    };
    let created = link.request_ok(Message::CreateSurface { config }, Duration::from_secs(20))?;
    let surface_id = created
        .get("surface")
        .and_then(|value| value.as_u64())
        .unwrap_or(0);
    let surface = SurfaceId(surface_id);
    println!("surface={surface_id}");

    let requested = url.to_string();
    link.request_ok(
        Message::Navigate {
            surface,
            url: requested.clone(),
            opts: Default::default(),
        },
        Duration::from_secs(20),
    )?;
    println!("navigate={requested}");

    let event = link.wait_for_event(
        |event| match event {
            // The surface starts on about:blank, so only the load of *the page we asked for*
            // proves that navigation works.
            EngineEvent::LoadFinished { url, .. } => same_host(url, &requested),
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
    Ok((link, surface))
}

fn eval_json(
    link: &mut EngineLink,
    surface: SurfaceId,
    script: String,
) -> Result<serde_json::Value, antikolt_ipc::LinkError> {
    let reply = link.request_ok(
        Message::Eval {
            surface,
            script,
            world: World::Main,
        },
        EVAL_TIMEOUT,
    )?;
    let value = reply
        .get("value")
        .and_then(|value| value.as_str())
        .unwrap_or("")
        .to_string();
    Ok(serde_json::from_str(&value).unwrap_or(serde_json::Value::String(value)))
}

fn run(exe: PathBuf, url: String, freeze: bool) -> Result<(), antikolt_ipc::LinkError> {
    let (mut link, surface) = open_surface(&exe, &url)?;
    let surface_id = surface.0;

    if freeze {
        // Scroll before freezing, otherwise a scroll-restore test proves nothing: the state would
        // record 0 and restoring 0 is indistinguishable from doing nothing.
        let scrolled = eval_json(
            &mut link,
            surface,
            "window.scrollTo(0, 600); JSON.stringify({ y: window.scrollY })".to_string(),
        );
        match &scrolled {
            Ok(value) => println!(
                "pre-scroll: scrollY={}",
                value.get("y").and_then(|v| v.as_i64()).unwrap_or(-1)
            ),
            Err(error) => eprintln!("pre-scroll failed (not fatal): {error}"),
        }
        phase("loaded", "");
        std::thread::sleep(PHASE_PAUSE);

        let snapshot = link.request_ok(Message::Snapshot { surface }, Duration::from_secs(30))?;
        let bytes = snapshot
            .get("bytes")
            .and_then(|value| value.as_i64())
            .unwrap_or(0);
        let state_bytes = snapshot
            .get("state_bytes")
            .and_then(|value| value.as_i64())
            .unwrap_or(0);
        let path = snapshot
            .get("path")
            .and_then(|value| value.as_str())
            .unwrap_or("");
        let state_path = snapshot
            .get("state_path")
            .and_then(|value| value.as_str())
            .unwrap_or("");
        let title = snapshot
            .get("title")
            .and_then(|value| value.as_str())
            .unwrap_or("");
        println!(
            "snapshot bytes={bytes} state_bytes={state_bytes} path={path} state_path={state_path} title={title:?}"
        );
        phase(
            "snapshot",
            &format!(" bytes={bytes} state_bytes={state_bytes}"),
        );

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
    Ok(())
}

/// Open a saved MHTML offline and put the captured state back.
fn run_restore(
    exe: PathBuf,
    file_url: String,
    state_path: PathBuf,
) -> Result<(), antikolt_ipc::LinkError> {
    let state_text = std::fs::read_to_string(&state_path).map_err(|error| {
        antikolt_ipc::LinkError::Io(std::io::Error::other(format!(
            "cannot read {}: {error}",
            state_path.display()
        )))
    })?;
    // The state file is JSON, which is also a valid JavaScript object literal.
    let state: serde_json::Value = serde_json::from_str(&state_text).map_err(|error| {
        antikolt_ipc::LinkError::Io(std::io::Error::other(format!(
            "cannot parse the state file: {error}"
        )))
    })?;
    println!(
        "state loaded: title={:?} scrollY={} forms={}",
        state.get("title").and_then(|v| v.as_str()).unwrap_or(""),
        state.get("scrollY").and_then(|v| v.as_i64()).unwrap_or(0),
        state
            .get("forms")
            .and_then(|v| v.as_array())
            .map(|forms| forms.len())
            .unwrap_or(0)
    );

    let (mut link, surface) = open_surface(&exe, &file_url)?;
    phase("restored_document", "");

    let restore_script = format!(
        r#"(function () {{
  var S = {state};
  var out = {{ forms: 0, session: 0, media: 0, scrollY: 0 }};
  try {{
    for (var k in S.session) {{ sessionStorage.setItem(k, S.session[k]); out.session++; }}
  }} catch (e) {{ }}
  (S.forms || []).forEach(function (f) {{
    var el = null;
    try {{ el = document.querySelector(f.selector); }} catch (e) {{ }}
    if (!el) {{ return; }}
    try {{
      var t = (el.getAttribute('type') || el.tagName || '').toLowerCase();
      if (t === 'checkbox' || t === 'radio') {{ el.checked = !!f.checked; }}
      else if (f.value !== null) {{ el.value = f.value; }}
      el.dispatchEvent(new Event('input', {{ bubbles: true }}));
      el.dispatchEvent(new Event('change', {{ bubbles: true }}));
      out.forms++;
    }} catch (e) {{ }}
  }});
  window.scrollTo(S.scrollX || 0, S.scrollY || 0);
  out.scrollY = window.scrollY;
  (S.media || []).forEach(function (m, i) {{
    var player = document.querySelectorAll('video,audio')[i];
    if (!player) {{ return; }}
    try {{
      if (m.currentTime) {{ player.currentTime = m.currentTime; }}
      player.pause();
      out.media++;
    }} catch (e) {{ }}
  }});
  return JSON.stringify(out);
}})()"#
    );

    let report = eval_json(&mut link, surface, restore_script)?;
    println!(
        "restore applied: forms={} session={} media={} scrollY={}",
        report.get("forms").and_then(|v| v.as_i64()).unwrap_or(0),
        report.get("session").and_then(|v| v.as_i64()).unwrap_or(0),
        report.get("media").and_then(|v| v.as_i64()).unwrap_or(0),
        report.get("scrollY").and_then(|v| v.as_i64()).unwrap_or(0)
    );
    phase("state_applied", "");

    let verify_script = "JSON.stringify({ title: document.title, url: location.href, \
                          scrollY: window.scrollY, \
                          fields: document.querySelectorAll('input,textarea,select').length, \
                          text: (document.body ? document.body.innerText : '').slice(0, 120) })"
        .to_string();
    let verification = eval_json(&mut link, surface, verify_script)?;
    println!(
        "verified: title={:?} url={} scrollY={} fields={} text={:?}",
        verification
            .get("title")
            .and_then(|v| v.as_str())
            .unwrap_or(""),
        verification
            .get("url")
            .and_then(|v| v.as_str())
            .unwrap_or(""),
        verification
            .get("scrollY")
            .and_then(|v| v.as_i64())
            .unwrap_or(0),
        verification
            .get("fields")
            .and_then(|v| v.as_i64())
            .unwrap_or(0),
        verification
            .get("text")
            .and_then(|v| v.as_str())
            .unwrap_or("")
    );

    link.request_ok(Message::DestroySurface { surface }, Duration::from_secs(20))?;
    link.shutdown()?;
    println!("engine_host_exited");
    phase("done", "");
    Ok(())
}
