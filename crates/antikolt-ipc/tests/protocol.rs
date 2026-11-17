// SPDX-License-Identifier: MPL-2.0

//! Protocol-level tests: every message must survive a roundtrip, and hostile framing must be
//! rejected without panicking.

use antikolt_ipc::{
    decode_frame, encode_frame, Capability, ColorScheme, EngineEvent, EngineInfo, Envelope,
    ErrorCode, FrameError, ImageFormat, Message, NavigateOpts, PermissionKind, PopupDisposition,
    Rect, ReplyOutcome, SurfaceConfig, SurfaceId, Viewport, World, MAX_FRAME_BYTES,
    PROTOCOL_VERSION,
};

fn one_of_each() -> Vec<Message> {
    let surface = SurfaceId(7);
    vec![
        Message::Hello {
            protocol: 1,
            engine: "cef".into(),
        },
        Message::Welcome {
            protocol: 1,
            engine: EngineInfo {
                name: "cef".into(),
                version: "142.0.1".into(),
                chromium_major: Some(142),
                capabilities: vec![Capability::Snapshot, Capability::IsolatedWorld],
            },
        },
        Message::Ping,
        Message::CreateSurface {
            config: SurfaceConfig {
                partition: "persist:c_01hq".into(),
                viewport: Viewport::new(1440, 900, 125),
                proxy: None,
                is_primary: true,
                parent_window: 0,
            },
        },
        Message::CreateSurface {
            config: SurfaceConfig {
                partition: "persist:home".into(),
                viewport: Viewport::new(1200, 760, 150),
                proxy: Some("socks5://127.0.0.1:9050".into()),
                is_primary: true,
                // 0x00000000001204C8: a child window owned by the shell process.
                parent_window: 0x0012_04C8,
            },
        },
        Message::SetBounds {
            surface,
            bounds: Rect::new(12, -4, 800, 600),
        },
        Message::Focus {
            surface,
            focused: true,
        },
        Message::DestroySurface { surface },
        Message::Navigate {
            surface,
            url: "https://example.com/a?b=1#c".into(),
            opts: NavigateOpts {
                referrer: None,
                bypass_cache: true,
                timeout_ms: Some(15_000),
            },
        },
        Message::Eval {
            surface,
            script: "document.title".into(),
            world: World::Isolated,
        },
        Message::Scroll {
            surface,
            x: 0,
            y: 12_345,
            animate: false,
        },
        Message::SetZoom {
            surface,
            factor_percent: 110,
        },
        Message::SetMedia {
            surface,
            scheme: ColorScheme::Grey,
            forced_colors: true,
        },
        Message::Capture {
            surface,
            format: ImageFormat::Webp,
            quality: 80,
        },
        Message::Snapshot { surface },
        Message::Metrics { surface },
        Message::ClearOriginData {
            partition: "persist:c_01hq".into(),
            origin: "https://example.com".into(),
            storages: vec!["cookies".into(), "local_storage".into()],
        },
        Message::Reply {
            to: 42,
            outcome: ReplyOutcome::Ok {
                json: serde_json::json!({"surface": 7}),
            },
        },
        Message::Reply {
            to: 43,
            outcome: ReplyOutcome::Err {
                code: ErrorCode::Timeout,
                message: "snapshot took too long".into(),
                retryable: true,
            },
        },
        Message::Shutdown,
        Message::Event {
            event: EngineEvent::LoadFinished {
                surface,
                url: "https://example.com/".into(),
                http_status: Some(200),
            },
        },
        Message::Event {
            event: EngineEvent::AudibleChanged {
                surface,
                audible: true,
                playing: true,
            },
        },
        Message::Event {
            event: EngineEvent::PopupRequested {
                surface,
                url: "https://accounts.example.com/oauth".into(),
                disposition: PopupDisposition::AuthenticatedWindow,
            },
        },
        Message::Event {
            event: EngineEvent::PermissionRequested {
                surface,
                origin: "https://example.com".into(),
                kind: PermissionKind::Notifications,
            },
        },
        Message::Event {
            event: EngineEvent::BridgeCall {
                surface,
                method: "open_card".into(),
                args: serde_json::json!({"id": "c_01hq", "url": "https://example.com/"}),
            },
        },
        Message::Event {
            event: EngineEvent::SurfaceCreated { surface },
        },
        Message::Event {
            event: EngineEvent::SurfaceClosed {
                surface,
                reason: "renderer gone".into(),
            },
        },
        Message::Event {
            event: EngineEvent::SurfaceClosed {
                surface,
                reason: String::new(),
            },
        },
        Message::Event {
            event: EngineEvent::ShuttingDown {
                reason: "core requested stop".into(),
            },
        },
    ]
}

#[test]
fn every_message_roundtrips() {
    for (i, message) in one_of_each().into_iter().enumerate() {
        let envelope = Envelope::new(i as u64, message.clone());
        let bytes = encode_frame(&envelope).expect("encode");
        let (decoded, consumed) = decode_frame(&bytes).expect("decode");
        assert_eq!(consumed, bytes.len(), "consumed the whole frame");
        assert_eq!(decoded, envelope, "roundtrip preserves the message");
        assert_eq!(decoded.version, PROTOCOL_VERSION);
    }
}

#[test]
fn two_frames_back_to_back() {
    let a = encode_frame(&Envelope::new(1, Message::Ping)).unwrap();
    let b = encode_frame(&Envelope::new(
        2,
        Message::Metrics {
            surface: SurfaceId(1),
        },
    ))
    .unwrap();

    let mut buffer = Vec::new();
    buffer.extend_from_slice(&a);
    buffer.extend_from_slice(&b);

    let (first, used) = decode_frame(&buffer).unwrap();
    assert_eq!(first.id, 1);
    let (second, used2) = decode_frame(&buffer[used..]).unwrap();
    assert_eq!(second.id, 2);
    assert_eq!(used + used2, buffer.len());
}

#[test]
fn truncated_header_is_incomplete_not_an_error() {
    let err = decode_frame(&[0u8, 1]).unwrap_err();
    assert!(matches!(err, FrameError::Incomplete { need: 4, have: 2 }));
}

#[test]
fn truncated_payload_is_incomplete() {
    let bytes = encode_frame(&Envelope::new(1, Message::Ping)).unwrap();
    let err = decode_frame(&bytes[..bytes.len() - 1]).unwrap_err();
    assert!(matches!(err, FrameError::Incomplete { .. }));
}

#[test]
fn oversize_length_prefix_is_rejected_before_buffering() {
    let mut header = ((MAX_FRAME_BYTES + 1) as u32).to_le_bytes().to_vec();
    header.extend_from_slice(b"{}");
    let err = decode_frame(&header).unwrap_err();
    assert!(matches!(err, FrameError::TooLarge { .. }));
}

#[test]
fn wrong_version_is_rejected() {
    let mut envelope = Envelope::new(1, Message::Ping);
    envelope.version = PROTOCOL_VERSION + 1;
    let payload = serde_json::to_vec(&envelope).unwrap();
    let mut frame = (payload.len() as u32).to_le_bytes().to_vec();
    frame.extend_from_slice(&payload);

    let err = decode_frame(&frame).unwrap_err();
    match err {
        FrameError::VersionMismatch { got, expected } => {
            assert_eq!(got, PROTOCOL_VERSION + 1);
            assert_eq!(expected, PROTOCOL_VERSION);
        }
        other => panic!("expected a version mismatch, got {other:?}"),
    }
}

#[test]
fn garbage_payload_is_malformed_not_a_panic() {
    let payload = b"{\"version\":1,\"id\":1,\"message\":{\"op\":\"not_a_real_op\"}}";
    let mut frame = (payload.len() as u32).to_le_bytes().to_vec();
    frame.extend_from_slice(payload);
    let err = decode_frame(&frame).unwrap_err();
    assert!(matches!(err, FrameError::Malformed { .. }));
}

#[test]
fn replies_are_correlated_to_requests() {
    let request = Envelope::new(
        99,
        Message::Snapshot {
            surface: SurfaceId(3),
        },
    );
    assert!(request.message.expects_reply());

    let reply = Envelope::new(
        100,
        Message::Reply {
            to: 99,
            outcome: ReplyOutcome::Err {
                code: ErrorCode::NotSupported,
                message: "no snapshot in this engine".into(),
                retryable: false,
            },
        },
    );
    assert!(!reply.message.expects_reply());

    let bytes = encode_frame(&reply).unwrap();
    let (decoded, _) = decode_frame(&bytes).unwrap();
    match decoded.message {
        Message::Reply { to, outcome } => {
            assert_eq!(to, 99);
            assert!(matches!(
                outcome,
                ReplyOutcome::Err {
                    code: ErrorCode::NotSupported,
                    ..
                }
            ));
        }
        other => panic!("expected a reply, got {other:?}"),
    }
}

#[test]
fn capabilities_are_queryable() {
    let info = EngineInfo {
        name: "cef".into(),
        version: "142".into(),
        chromium_major: Some(142),
        capabilities: vec![Capability::Capture, Capability::OffscreenSurface],
    };
    assert!(info.supports(Capability::Capture));
    assert!(!info.supports(Capability::VirtualAuthenticator));
}

#[test]
fn default_quality_is_stable() {
    // The serde default on `Capture.quality` must not silently change: it is part of the wire format.
    let json = serde_json::json!({
        "version": 1,
        "id": 1,
        "message": { "op": "capture", "surface": 1, "format": "webp" }
    });
    let envelope: Envelope = serde_json::from_value(json).unwrap();
    match envelope.message {
        Message::Capture { quality, .. } => assert_eq!(quality, 80),
        other => panic!("expected capture, got {other:?}"),
    }
}

#[test]
fn error_code_display_matches_the_wire_format() {
    // Diagnostics and the protocol must not disagree about what an error is called.
    let codes = [
        ErrorCode::Timeout,
        ErrorCode::NotSupported,
        ErrorCode::InvalidArgument,
        ErrorCode::SurfaceNotFound,
        ErrorCode::EngineBusy,
        ErrorCode::VaultLocked,
        ErrorCode::Cancelled,
        ErrorCode::Internal,
    ];
    for code in codes {
        let wire = serde_json::to_value(code).expect("serialise error code");
        assert_eq!(
            wire.as_str().expect("error code is a string"),
            code.to_string(),
            "Display and serde disagree for {code:?}"
        );
    }
}

#[test]
fn parent_window_is_additive_and_optional() {
    // A create_surface frame from before embedding existed must still parse: the field is additive,
    // so the protocol version does not move and an older shell keeps working.
    let legacy = serde_json::json!({
        "version": 1, "id": 1,
        "message": {
            "op": "create_surface",
            "config": {
                "partition": "persist:home",
                "viewport": {"width": 800, "height": 600, "device_scale_percent": 100},
                "proxy": null,
                "is_primary": true
            }
        }
    });
    let envelope: Envelope = serde_json::from_value(legacy).expect("legacy create_surface");
    match envelope.message {
        Message::CreateSurface { config } => {
            assert_eq!(
                config.parent_window, 0,
                "absent parent_window means top-level"
            );
        }
        other => panic!("expected create_surface, got {other:?}"),
    }

    // And the reverse direction: a peer that does not know the field must not choke on it. Our side
    // proves this by round-tripping an embedded config through JSON and back.
    let embedded = Message::CreateSurface {
        config: SurfaceConfig {
            partition: "persist:home".into(),
            viewport: Viewport::new(800, 600, 100),
            proxy: None,
            is_primary: true,
            parent_window: 0x0000_1234_ABCD_0000,
        },
    };
    let json = serde_json::to_value(&embedded).expect("serialise");
    assert_eq!(
        json["config"]["parent_window"], 0x0000_1234_ABCD_0000u64,
        "the handle must survive JSON exactly, not as a rounded double"
    );
    let back: Message = serde_json::from_value(json).expect("deserialise");
    assert_eq!(back, embedded);
}

#[test]
fn set_bounds_carries_physical_pixels_and_signed_origins() {
    let message = Message::SetBounds {
        surface: SurfaceId(7),
        bounds: Rect::new(-8, 24, 1024, 768),
    };
    let json = serde_json::to_value(&message).expect("serialise");
    assert_eq!(json["bounds"]["x"], -8);
    assert_eq!(json["bounds"]["y"], 24);
    assert_eq!(json["bounds"]["width"], 1024);
}

#[test]
fn engine_events_from_the_host_deserialise() {
    // Shapes the C++ engine host actually sends (see apps/engine-host/src/protocol.cpp).
    let welcome = serde_json::json!({
        "version": 1, "id": 1,
        "message": {
            "op": "welcome", "protocol": 1,
            "engine": {
                "name": "cef", "version": "158.0.3", "chromium_major": 158,
                "capabilities": ["capture", "media_emulation", "isolated_world", "offscreen_surface"]
            }
        }
    });
    let envelope: Envelope = serde_json::from_value(welcome).expect("welcome");
    match envelope.message {
        Message::Welcome { protocol, engine } => {
            assert_eq!(protocol, 1);
            assert_eq!(engine.name, "cef");
            assert!(engine.supports(Capability::IsolatedWorld));
            assert!(!engine.supports(Capability::VirtualAuthenticator));
        }
        other => panic!("expected welcome, got {other:?}"),
    }

    let reply = serde_json::json!({
        "version": 1, "id": 2,
        "message": { "op": "reply", "to": 1, "outcome": { "kind": "ok", "json": { "surface": 7 } } }
    });
    let envelope: Envelope = serde_json::from_value(reply).expect("reply");
    match envelope.message {
        Message::Reply { to, outcome } => {
            assert_eq!(to, 1);
            match outcome {
                ReplyOutcome::Ok { json } => assert_eq!(json["surface"], 7),
                other => panic!("expected ok, got {other:?}"),
            }
        }
        other => panic!("expected reply, got {other:?}"),
    }

    let closed = serde_json::json!({
        "version": 1, "id": 3,
        "message": { "op": "event", "event": { "type": "surface_closed", "surface": 7 } }
    });
    let envelope: Envelope =
        serde_json::from_value(closed).expect("surface_closed without a reason");
    match envelope.message {
        Message::Event { event } => {
            assert!(
                matches!(event, EngineEvent::SurfaceClosed { reason, .. } if reason.is_empty())
            );
        }
        other => panic!("expected event, got {other:?}"),
    }
}
