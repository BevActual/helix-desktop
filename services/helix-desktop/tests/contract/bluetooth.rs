use super::helpers::{wait_bluetooth_available, CompletionLog};
use crate::harness::FakeBluez;
use helix_desktop_api::{Desktop, Domain};
use serde_json::json;

pub async fn run(desktop: &Desktop, fake: FakeBluez) {
    wait_bluetooth_available(desktop, true).await;
    let completions = CompletionLog::spawn_bluetooth(desktop);

    let operation = desktop
        .action(
            Domain::Bluetooth,
            "pair",
            &json!({"path": "/org/bluez/hci0/dev_FAKE"}),
        )
        .await
        .expect("pair");
    desktop.bluetooth_cancel(operation).await.expect("cancel");
    assert!(desktop.bluetooth_cancel(operation).await.is_err());
    let (ok, message) = completions.wait(operation).await;
    assert!(!ok);
    assert_eq!(message, "Cancelled");

    let interrupted = desktop
        .action(
            Domain::Bluetooth,
            "pair",
            &json!({"path": "/org/bluez/hci0/dev_FAKE"}),
        )
        .await
        .expect("pair again");
    // Dropping fake Bluez removes org.bluez; service should fail the operation.
    drop(fake);
    wait_bluetooth_available(desktop, false).await;
    let (ok, message) = completions.wait(interrupted).await;
    assert!(!ok);
    assert_eq!(message, "Device service disconnected");
}
