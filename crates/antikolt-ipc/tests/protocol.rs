//! Protocol-level tests: every message must survive a roundtrip, and hostile framing must be
//! rejected without panicking.

use antikolt_ipc::{
    decode_frame, encode_frame, Capability, ColorScheme, EngineEvent, EngineInfo, Envelope,
    ErrorCode, FrameError, ImageFormat, Message, NavigateOpts, PermissionKind, PopupDisposition,
    ReplyOutcome, SurfaceConfig, SurfaceId, Viewport, World, MAX_FRAME_BYTES, PROTOCOL_VERSION,
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
            },
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
