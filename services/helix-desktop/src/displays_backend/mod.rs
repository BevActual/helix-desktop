//! Displays1 inventory: `niri msg` when the compositor socket is present, otherwise mock.

mod mock;
mod niri;

use serde_json::Value;

#[derive(Clone, Debug)]
enum Inner {
    Mock,
    Niri(niri::NiriBackend),
}

#[derive(Clone, Debug)]
pub struct DisplaysBackend {
    inner: Inner,
}

impl DisplaysBackend {
    pub fn new() -> Self {
        let inner = if mock::force_mock() {
            Inner::Mock
        } else if niri::probe() {
            Inner::Niri(niri::NiriBackend::new())
        } else {
            Inner::Mock
        };
        Self { inner }
    }

    pub fn refresh(&mut self, desktop_registered: bool) -> bool {
        match &mut self.inner {
            Inner::Mock => false,
            Inner::Niri(backend) => backend.refresh(desktop_registered),
        }
    }

    pub fn snapshot(&self, desktop_registered: bool) -> Value {
        if !desktop_registered {
            return mock::unavailable_snapshot();
        }
        match &self.inner {
            Inner::Mock => mock::mock_snapshot(),
            Inner::Niri(backend) => backend.snapshot(),
        }
    }
}
