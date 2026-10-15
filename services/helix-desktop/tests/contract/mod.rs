//! Behavioural contract for `org.helix.Desktop` (one helix-desktop process per test run).

mod bootstrap;
mod bluetooth;
mod configuration;
mod helpers;

use crate::harness::boot;

pub async fn run_all() {
    let (mut session, desktop) = boot().await;
    bootstrap::run(&desktop).await;
    if let Some(fake) = crate::harness::FakeBluez::start() {
        bluetooth::run(&desktop, fake).await;
    } else {
        eprintln!(
            "skip bluetooth contract (build helix-fake-bluez: cargo build --profile fast -p helix-desktop --bin helix-fake-bluez)"
        );
    }
    configuration::run(&session, &desktop).await;
    configuration::run_persistence(&mut session, &desktop).await;
}
