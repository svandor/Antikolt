// SPDX-License-Identifier: MPL-2.0

//! A client link to an engine host over the child process's standard IO.
//!
//! The wire format is the one in [`crate::message`]: length-prefixed JSON envelopes. The transport
//! is deliberately plain `std::process` + threads rather than async IO: there is exactly one child
//! per browser, frames are far apart in time, and a blocking reader thread is the simplest thing
//! that cannot leak tasks.
//!
//! Timeouts exist because a browser that never answers must not hang the shell. A request that times
//! out leaves the link usable: the reply is simply discarded when it eventually arrives.

use std::io::{self, BufReader, Read, Write};
use std::path::Path;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::message::{EngineEvent, Envelope, Message, ReplyOutcome};
use crate::{decode_frame, encode_frame};

/// Default deadline for one request.
pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Debug, thiserror::Error)]
pub enum LinkError {
    #[error("could not talk to the engine host: {0}")]
    Io(#[from] io::Error),
    #[error("the engine host broke the protocol: {0}")]
    Frame(#[from] crate::FrameError),
    #[error("the engine host closed the link")]
    Closed,
    #[error("timed out waiting for {0}")]
    Timeout(&'static str),
    #[error("expected {expected}, got {got:?}")]
    Unexpected {
        expected: &'static str,
        got: Box<Message>,
    },
    #[error("the engine host reported {code}: {message}")]
    Engine { code: String, message: String },
}

/// One connection to an engine host process.
#[derive(Debug)]
pub struct EngineLink {
    child: Child,
    stdin: ChildStdin,
    frames: Receiver<Result<Envelope, String>>,
    reader: Option<JoinHandle<()>>,
    /// Envelopes read while looking for something else (events arrive between replies).
    pending: Vec<Envelope>,
    next_id: u64,
}

impl EngineLink {
    /// Start the engine host in server mode.
    pub fn spawn(exe: &Path, extra_args: &[&str]) -> Result<Self, LinkError> {
        let mut child = Command::new(exe)
            .arg("--stdio")
            .args(extra_args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()?;

        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| LinkError::Io(io::Error::other("no stdin pipe")))?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| LinkError::Io(io::Error::other("no stdout pipe")))?;

        let (sender, frames) = mpsc::channel();
        let reader = thread::spawn(move || read_frames(stdout, sender));

        Ok(Self {
            child,
            stdin,
            frames,
            reader: Some(reader),
            pending: Vec::new(),
            next_id: 1,
        })
    }

    /// The operating system process id of the engine host (useful for memory measurement).
    pub fn pid(&self) -> u32 {
        self.child.id()
    }

    fn send(&mut self, id: u64, message: Message) -> Result<(), LinkError> {
        let bytes = encode_frame(&Envelope::new(id, message))?;
        eprintln!("[link] -> id={id} {} bytes", bytes.len());
        self.stdin.write_all(&bytes)?;
        self.stdin.flush()?;
        Ok(())
    }

    fn next_message(
        &mut self,
        what: &'static str,
        timeout: Duration,
    ) -> Result<Envelope, LinkError> {
        if !self.pending.is_empty() {
            return Ok(self.pending.remove(0));
        }
        self.recv(what, timeout)
    }

    fn recv(&mut self, what: &'static str, timeout: Duration) -> Result<Envelope, LinkError> {
        match self.frames.recv_timeout(timeout) {
            Ok(Ok(envelope)) => Ok(envelope),
            Ok(Err(detail)) => Err(LinkError::Io(io::Error::other(detail))),
            Err(RecvTimeoutError::Timeout) => Err(LinkError::Timeout(what)),
            Err(RecvTimeoutError::Disconnected) => Err(LinkError::Closed),
        }
    }

    /// Send a request and wait for its reply.
    ///
    /// Events that arrive before the reply are set aside and put back afterwards, in order. They are
    /// *not* re-queued one at a time: an earlier version did that and spun forever on the same event
    /// while the reply sat unread in the channel.
    pub fn request(
        &mut self,
        message: Message,
        timeout: Duration,
    ) -> Result<ReplyOutcome, LinkError> {
        let id = self.next_id;
        self.next_id += 1;
        self.send(id, message)?;

        let deadline = std::time::Instant::now() + timeout;
        let mut deferred: Vec<Envelope> = Vec::new();
        let outcome = loop {
            let envelope = if !self.pending.is_empty() {
                self.pending.remove(0)
            } else {
                let remaining = deadline
                    .checked_duration_since(std::time::Instant::now())
                    .unwrap_or(Duration::from_millis(1));
                self.recv("a reply", remaining)?
            };
            match envelope.message {
                Message::Reply { to, outcome } if to == id => break outcome,
                other => deferred.push(Envelope {
                    id: envelope.id,
                    version: envelope.version,
                    message: other,
                }),
            }
        };

        // Put the earlier events back in front of anything that is still buffered.
        deferred.extend(std::mem::take(&mut self.pending));
        self.pending = deferred;
        Ok(outcome)
    }

    /// Send a request that is expected to succeed and return its JSON result.
    pub fn request_ok(
        &mut self,
        message: Message,
        timeout: Duration,
    ) -> Result<serde_json::Value, LinkError> {
        match self.request(message, timeout)? {
            ReplyOutcome::Ok { json } => Ok(json),
            ReplyOutcome::Err {
                code,
                message: text,
                ..
            } => Err(LinkError::Engine {
                code: code.to_string(),
                message: text,
            }),
        }
    }

    /// Wait for the next engine event matching `want`.
    ///
    /// Events that do not match are consumed and dropped: this is a probe/shell convenience, not a
    /// general event bus. A component that needs every event should read [`EngineLink::next_event`].
    pub fn wait_for_event<F>(
        &mut self,
        want: F,
        timeout: Duration,
    ) -> Result<EngineEvent, LinkError>
    where
        F: Fn(&EngineEvent) -> bool,
    {
        let deadline = std::time::Instant::now() + timeout;
        loop {
            let remaining = deadline
                .checked_duration_since(std::time::Instant::now())
                .unwrap_or(Duration::from_millis(1));
            let envelope = self.next_message("an event", remaining)?;
            if let Message::Event { event } = envelope.message {
                if want(&event) {
                    return Ok(event);
                }
            }
        }
    }

    /// The next event, whatever it is. Events are not re-queued, so a caller that needs them all
    /// must drain continuously.
    pub fn next_event(&mut self, timeout: Duration) -> Result<Option<EngineEvent>, LinkError> {
        let envelope = self.next_message("an event", timeout)?;
        match envelope.message {
            Message::Event { event } => Ok(Some(event)),
            _ => Ok(None),
        }
    }

    /// The `welcome` handshake: send `hello`, expect `welcome` with a compatible protocol version.
    pub fn handshake(&mut self, engine_name: &str) -> Result<crate::EngineInfo, LinkError> {
        let id = self.next_id;
        self.next_id += 1;
        self.send(
            id,
            Message::Hello {
                protocol: crate::PROTOCOL_VERSION,
                engine: engine_name.to_string(),
            },
        )?;

        let deadline = std::time::Instant::now() + DEFAULT_TIMEOUT;
        loop {
            let remaining = deadline
                .checked_duration_since(std::time::Instant::now())
                .unwrap_or(Duration::from_millis(1));
            let envelope = self.next_message("the welcome handshake", remaining)?;
            if let Message::Welcome { protocol, engine } = envelope.message {
                if protocol != crate::PROTOCOL_VERSION {
                    return Err(LinkError::Unexpected {
                        expected: "a matching protocol version",
                        got: Box::new(Message::Welcome { protocol, engine }),
                    });
                }
                return Ok(engine);
            }
            // Anything else (a reply, an early event) is not part of the handshake.
        }
    }

    /// Ask the engine host to stop, then wait for it to exit.
    pub fn shutdown(mut self) -> Result<(), LinkError> {
        let _ = self.request(Message::Shutdown, DEFAULT_TIMEOUT);
        let status = self.child.wait()?;
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
        if !status.success() {
            return Err(LinkError::Closed);
        }
        Ok(())
    }
}

fn read_frames(stdout: impl Read, sender: Sender<Result<Envelope, String>>) {
    let mut reader = BufReader::new(stdout);
    loop {
        let mut header = [0u8; 4];
        if let Err(error) = reader.read_exact(&mut header) {
            let _ = sender.send(Err(format!("reading the frame header: {error}")));
            return;
        }
        let length = u32::from_le_bytes(header) as usize;
        let mut payload = vec![0u8; length];
        if let Err(error) = reader.read_exact(&mut payload) {
            let _ = sender.send(Err(format!("reading the frame body: {error}")));
            return;
        }
        let mut buffer = Vec::with_capacity(4 + length);
        buffer.extend_from_slice(&header);
        buffer.extend_from_slice(&payload);
        match decode_frame(&buffer) {
            Ok((envelope, _)) => {
                if sender.send(Ok(envelope)).is_err() {
                    return;
                }
            }
            Err(error) => {
                let _ = sender.send(Err(format!("decoding a frame: {error}")));
                return;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_spawn_without_a_real_host_fails_cleanly() {
        let result = EngineLink::spawn(Path::new("this-executable-does-not-exist"), &[]);
        assert!(result.is_err());
    }
}
