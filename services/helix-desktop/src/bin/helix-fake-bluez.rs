//! Minimal `org.bluez` stub for nested desktop contract tests (session or “system” bus).
//! Minimal org.bluez stub for nested desktop contract tests.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU32, Ordering};
use zbus::interface;
use zbus::names::WellKnownName;
use zbus::zvariant::{ObjectPath, OwnedObjectPath, OwnedValue, Value};

static CANCEL_COUNT: AtomicU32 = AtomicU32::new(0);

struct FakeDevice;

#[interface(name = "org.bluez.Device1")]
impl FakeDevice {
    fn pair(&self) {}

    fn cancel_pairing(&self) {
        CANCEL_COUNT.fetch_add(1, Ordering::Relaxed);
    }

    #[zbus(name = "GetCancelCount")]
    fn get_cancel_count(&self) -> u32 {
        CANCEL_COUNT.load(Ordering::Relaxed)
    }
}

struct FakeAgentManager;

#[interface(name = "org.bluez.AgentManager1")]
impl FakeAgentManager {
    fn register_agent(&self, _agent: ObjectPath<'_>, _capability: &str) {}
    fn request_default_agent(&self, _agent: ObjectPath<'_>) {}
    fn unregister_agent(&self, _agent: ObjectPath<'_>) {}
}

struct FakeObjectManager;

#[interface(name = "org.freedesktop.DBus.ObjectManager")]
impl FakeObjectManager {
    fn get_managed_objects(
        &self,
    ) -> HashMap<OwnedObjectPath, HashMap<String, HashMap<String, OwnedValue>>> {
        let mut objects = HashMap::new();
        let adapter_path: OwnedObjectPath = "/org/bluez/hci0".try_into().expect("path");
        let device_path: OwnedObjectPath = "/org/bluez/hci0/dev_FAKE".try_into().expect("path");

        let mut adapter_props = HashMap::new();
        adapter_props.insert("Powered".into(), OwnedValue::from(false));
        adapter_props.insert(
            "Alias".into(),
            Value::from("Test Adapter").try_into().expect("alias"),
        );
        let mut adapter_ifaces = HashMap::new();
        adapter_ifaces.insert("org.bluez.Adapter1".into(), adapter_props);
        objects.insert(adapter_path, adapter_ifaces);

        let mut device_props = HashMap::new();
        device_props.insert(
            "Address".into(),
            Value::from("00:00:00:00:00:01")
                .try_into()
                .expect("address"),
        );
        device_props.insert(
            "Alias".into(),
            Value::from("Test Device").try_into().expect("device alias"),
        );
        device_props.insert("Paired".into(), OwnedValue::from(false));
        device_props.insert("Connected".into(), OwnedValue::from(false));
        let mut device_ifaces = HashMap::new();
        device_ifaces.insert("org.bluez.Device1".into(), device_props);
        objects.insert(device_path, device_ifaces);

        objects
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> zbus::Result<()> {
    let conn = zbus::Connection::session().await?;
    conn.object_server().at("/", FakeObjectManager).await?;
    conn.object_server().at("/org/bluez", FakeAgentManager).await?;
    conn.object_server()
        .at("/org/bluez/hci0/dev_FAKE", FakeDevice)
        .await?;
    let name = WellKnownName::try_from("org.bluez")?;
    conn.request_name(name).await?;
    eprintln!("helix-fake-bluez listening on org.bluez");
    std::future::pending::<()>().await;
    Ok(())
}
