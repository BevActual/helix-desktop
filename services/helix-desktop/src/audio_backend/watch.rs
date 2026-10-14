//! PipeWire change watcher.
//!
//! The device list is read by shelling out to `wpctl`, which is far too expensive to do on a
//! timer (several processes, each a PipeWire client). This thread listens on a registry
//! connection instead and only raises a flag when something that can alter the list happens:
//! nodes appear/disappear, a node's params (volume, mute) change, or the default-device
//! metadata changes. The backend refreshes when the flag is set (plus a slow safety refresh).

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;

use pipewire as pw;
use pw::loop_::Timeout;
use pw::metadata::Metadata;
use pw::node::{Node, NodeChangeMask};
use pw::proxy::{Listener, ProxyT};
use pw::types::ObjectType;

const RECONNECT_DELAY: Duration = Duration::from_secs(2);
const LOOP_SLICE: Duration = Duration::from_millis(100);

struct Shared {
    dirty: AtomicBool,
    stop: AtomicBool,
}

pub struct Watch {
    shared: Arc<Shared>,
    thread: Option<JoinHandle<()>>,
}

impl std::fmt::Debug for Watch {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Watch").finish_non_exhaustive()
    }
}

impl Watch {
    pub fn start() -> Self {
        let shared = Arc::new(Shared { dirty: AtomicBool::new(true), stop: AtomicBool::new(false) });
        let thread_shared = Arc::clone(&shared);
        let thread = std::thread::Builder::new()
            .name("pw-audio-watch".into())
            .spawn(move || run(thread_shared))
            .ok();
        Self { shared, thread }
    }

    /// Whether something changed since the last call (also true right after (re)connecting).
    pub fn take_dirty(&self) -> bool {
        self.shared.dirty.swap(false, Ordering::AcqRel)
    }
}

impl Drop for Watch {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn run(shared: Arc<Shared>) {
    pw::init();
    while !shared.stop.load(Ordering::Relaxed) {
        if let Err(error) = watch_once(&shared) {
            tracing::debug!(%error, "PipeWire watch ended");
        }
        // Connection lost or PipeWire not running yet: refresh once it is back.
        shared.dirty.store(true, Ordering::Release);
        let mut waited = Duration::ZERO;
        while waited < RECONNECT_DELAY && !shared.stop.load(Ordering::Relaxed) {
            std::thread::sleep(Duration::from_millis(100));
            waited += Duration::from_millis(100);
        }
    }
}

type Proxies = Rc<RefCell<HashMap<u32, (Box<dyn ProxyT>, Vec<Box<dyn Listener>>)>>>;

fn watch_once(shared: &Arc<Shared>) -> Result<(), pw::Error> {
    let mainloop = pw::main_loop::MainLoopRc::new(None)?;
    let context = pw::context::ContextRc::new(&mainloop, None)?;
    let core = context.connect_rc(None)?;

    let lost = Rc::new(RefCell::new(false));
    let lost_flag = Rc::clone(&lost);
    let _core_listener = core
        .add_listener_local()
        .error(move |id, _seq, _res, _message| {
            if id == 0 {
                *lost_flag.borrow_mut() = true;
            }
        })
        .register();

    let registry = core.get_registry_rc()?;
    let registry_weak = registry.downgrade();
    let proxies: Proxies = Rc::new(RefCell::new(HashMap::new()));

    let global_shared = Arc::clone(shared);
    let global_proxies = Rc::clone(&proxies);
    let remove_shared = Arc::clone(shared);
    let remove_proxies = Rc::clone(&proxies);

    let _registry_listener = registry
        .add_listener_local()
        .global(move |global| {
            let Some(registry) = registry_weak.upgrade() else { return };
            let dirty = Arc::clone(&global_shared);
            let entry: Option<(Box<dyn ProxyT>, Box<dyn Listener>)> = match global.type_ {
                ObjectType::Node => {
                    let is_audio = global
                        .props
                        .as_ref()
                        .and_then(|p| p.get("media.class"))
                        .is_some_and(|class| class.starts_with("Audio/") || class.starts_with("Stream/"));
                    if !is_audio {
                        None
                    } else {
                        registry.bind::<Node, _>(global).ok().map(|node| {
                            let listener = node
                                .add_listener_local()
                                // Only parameter changes (volume, mute, ...) alter the device list;
                                // state and property churn from busy streams must not trigger reads.
                                .info(move |info| {
                                    if info.change_mask().contains(NodeChangeMask::PARAMS) {
                                        dirty.dirty.store(true, Ordering::Release);
                                    }
                                })
                                .register();
                            (Box::new(node) as Box<dyn ProxyT>, Box::new(listener) as Box<dyn Listener>)
                        })
                    }
                }
                ObjectType::Metadata => registry.bind::<Metadata, _>(global).ok().map(|metadata| {
                    let listener = metadata
                        .add_listener_local()
                        .property(move |_subject, _key, _type, _value| {
                            dirty.dirty.store(true, Ordering::Release);
                            0
                        })
                        .register();
                    (Box::new(metadata) as Box<dyn ProxyT>, Box::new(listener) as Box<dyn Listener>)
                }),
                _ => None,
            };
            // Clients (every `wpctl` run is one), ports, links and the like never change the
            // device list; counting them would make each refresh trigger the next.
            if let Some((proxy, listener)) = entry {
                let id = proxy.upcast_ref().id();
                global_proxies.borrow_mut().insert(id, (proxy, vec![listener]));
                global_shared.dirty.store(true, Ordering::Release);
            }
        })
        .global_remove(move |id| {
            // Only objects we track (audio nodes, metadata) matter.
            if remove_proxies.borrow_mut().remove(&id).is_some() {
                remove_shared.dirty.store(true, Ordering::Release);
            }
        })
        .register();

    while !shared.stop.load(Ordering::Relaxed) && !*lost.borrow() {
        mainloop.loop_().iterate(Timeout::Finite(LOOP_SLICE));
    }
    Ok(())
}
