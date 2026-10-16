//! The system tray as a pure model over what StatusNotifierItem applications register and publish. Everything an application says (its
//! title, icon names, icon pixels, the bus name and object path it registers) is hostile input: names and paths are validated before the
//! shell ever talks to them, pixels are size-checked, text is sanitized, and the page only ever refers to an item by a key this model made.

use serde::Serialize;

use crate::display::sanitize_for_display;

pub const MAX_ITEMS: usize = 24;
/// Icons larger than this (in either direction) are ignored: a tray icon is never bigger than a few dozen pixels.
pub const MAX_PIXMAP: usize = 64;
const MAX_NAME: usize = 255;
const DEFAULT_PATH: &str = "/StatusNotifierItem";

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Status {
    Passive,
    Active,
    /// The application wants attention (shown prominently).
    Attention,
}

impl Status {
    pub fn parse(text: &str) -> Self {
        match text {
            "Passive" => Self::Passive,
            "NeedsAttention" => Self::Attention,
            _ => Self::Active,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Pixmap {
    pub width: usize,
    pub height: usize,
    /// Straight RGBA, `width * height * 4` bytes.
    pub rgba: Vec<u8>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Item {
    key: String,
    bus_name: String,
    path: String,
    id: String,
    title: String,
    icon_name: String,
    icon_theme_path: String,
    menu_path: String,
    status: Status,
    pixmap: Option<Pixmap>,
}

/// What an item publishes, already read off the bus.
#[derive(Clone, Debug, Default)]
pub struct Props {
    pub id: String,
    pub title: String,
    pub icon_name: String,
    pub icon_theme_path: String,
    /// The object path of the item's `com.canonical.dbusmenu`, if it has one.
    pub menu: String,
    pub status: String,
    /// `(width, height, ARGB32 bytes in network order)` as the specification delivers them.
    pub pixmaps: Vec<(i32, i32, Vec<u8>)>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct PixmapView {
    pub width: usize,
    pub height: usize,
    pub rgba: Vec<u8>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ItemView {
    pub key: String,
    pub title: String,
    pub icon_name: String,
    pub status: Status,
    pub pixmap: Option<PixmapView>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Registered {
    Added,
    AlreadyKnown,
    Full,
    Invalid,
}

#[derive(Clone, Debug, Default)]
pub struct Tray {
    items: Vec<Item>,
}

/// A D-Bus bus name: a unique name (`:1.23`) or a well-known one (dotted, at least two elements).
pub fn valid_bus_name(name: &str) -> bool {
    if name.is_empty() || name.len() > MAX_NAME {
        return false;
    }
    let element_ok = |e: &str| {
        !e.is_empty()
            && e.chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
    };
    if let Some(rest) = name.strip_prefix(':') {
        return rest.split('.').count() >= 2 && rest.split('.').all(element_ok);
    }
    let parts: Vec<&str> = name.split('.').collect();
    parts.len() >= 2
        && parts
            .iter()
            .all(|p| element_ok(p) && !p.starts_with(|c: char| c.is_ascii_digit()))
}

/// A D-Bus object path.
pub fn valid_object_path(path: &str) -> bool {
    if path == "/" {
        return true;
    }
    path.len() <= MAX_NAME
        && path.starts_with('/')
        && path[1..]
            .split('/')
            .all(|s| !s.is_empty() && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_'))
}

/// What `RegisterStatusNotifierItem` was given, together with the connection that sent it. The specification allows a bus name, an object
/// path (the item then lives on the sender), or `name/path`. Anything that is not clearly one of those is refused.
pub fn parse_service(service: &str, sender: &str) -> Option<(String, String)> {
    if service.starts_with('/') {
        return (valid_bus_name(sender) && valid_object_path(service))
            .then(|| (sender.to_string(), service.to_string()));
    }
    let (name, path) = match service.find('/') {
        Some(slash) => (&service[..slash], &service[slash..]),
        None => (service, DEFAULT_PATH),
    };
    (valid_bus_name(name) && valid_object_path(path)).then(|| (name.to_string(), path.to_string()))
}

/// ARGB32 (as the specification sends: alpha, red, green, blue per pixel) to RGBA, if the size is sane and the data matches it.
pub fn pixmap_from_argb(width: i32, height: i32, data: &[u8]) -> Option<Pixmap> {
    let (w, h) = (usize::try_from(width).ok()?, usize::try_from(height).ok()?);
    if w == 0 || h == 0 || w > MAX_PIXMAP || h > MAX_PIXMAP || data.len() != w * h * 4 {
        return None;
    }
    let mut rgba = Vec::with_capacity(data.len());
    // The length was checked to be a whole number of pixels above, so nothing is left over.
    let (pixels, _) = data.as_chunks::<4>();
    for px in pixels {
        rgba.extend_from_slice(&[px[1], px[2], px[3], px[0]]);
    }
    Some(Pixmap {
        width: w,
        height: h,
        rgba,
    })
}

/// The largest usable pixmap of those offered.
fn best_pixmap(offered: &[(i32, i32, Vec<u8>)]) -> Option<Pixmap> {
    offered
        .iter()
        .filter_map(|(w, h, data)| pixmap_from_argb(*w, *h, data))
        .max_by_key(|p| p.width * p.height)
}

/// An icon name is a single component (no path climbing), as theme icon names always are.
fn clean_icon_name(name: &str) -> String {
    if name.is_empty()
        || name.len() > 200
        || name.contains('/')
        || name.contains("..")
        || name.contains('\0')
    {
        String::new()
    } else {
        name.to_string()
    }
}

impl Tray {
    pub fn register(&mut self, bus_name: &str, path: &str) -> Registered {
        if !valid_bus_name(bus_name) || !valid_object_path(path) {
            return Registered::Invalid;
        }
        let key = format!("{bus_name}{path}");
        if self.items.iter().any(|i| i.key == key) {
            return Registered::AlreadyKnown;
        }
        if self.items.len() >= MAX_ITEMS {
            return Registered::Full;
        }
        self.items.push(Item {
            key,
            bus_name: bus_name.into(),
            path: path.into(),
            id: String::new(),
            title: String::new(),
            icon_name: String::new(),
            icon_theme_path: String::new(),
            menu_path: String::new(),
            status: Status::Active,
            pixmap: None,
        });
        Registered::Added
    }

    /// Stores what an item published. Unknown keys are ignored.
    pub fn update(&mut self, key: &str, props: &Props) {
        let Some(item) = self.items.iter_mut().find(|i| i.key == key) else {
            return;
        };
        item.id = sanitize_for_display(&props.id, 60);
        item.title = sanitize_for_display(&props.title, 80);
        item.icon_name = clean_icon_name(&props.icon_name);
        item.icon_theme_path = if props.icon_theme_path.starts_with('/')
            && !props.icon_theme_path.contains("..")
            && props.icon_theme_path.len() <= 400
        {
            props.icon_theme_path.clone()
        } else {
            String::new()
        };
        item.menu_path = if valid_object_path(&props.menu) && props.menu != "/" {
            props.menu.clone()
        } else {
            String::new()
        };
        item.status = Status::parse(&props.status);
        item.pixmap = best_pixmap(&props.pixmaps);
    }

    pub fn remove(&mut self, key: &str) -> bool {
        let before = self.items.len();
        self.items.retain(|i| i.key != key);
        self.items.len() != before
    }

    /// Everything an application that has left the bus had registered; the keys that went.
    pub fn remove_bus_name(&mut self, bus_name: &str) -> Vec<String> {
        let gone: Vec<String> = self
            .items
            .iter()
            .filter(|i| i.bus_name == bus_name)
            .map(|i| i.key.clone())
            .collect();
        self.items.retain(|i| i.bus_name != bus_name);
        gone
    }

    pub fn knows(&self, key: &str) -> bool {
        self.items.iter().any(|i| i.key == key)
    }

    /// Where to talk to the item: its bus name and object path. Only for keys this model made.
    pub fn address(&self, key: &str) -> Option<(&str, &str)> {
        self.items
            .iter()
            .find(|i| i.key == key)
            .map(|i| (i.bus_name.as_str(), i.path.as_str()))
    }

    /// Where the item's menu lives, when it publishes one.
    pub fn menu_path(&self, key: &str) -> Option<&str> {
        self.items
            .iter()
            .find(|i| i.key == key)
            .map(|i| i.menu_path.as_str())
            .filter(|p| !p.is_empty())
    }

    pub fn icon_theme_path(&self, key: &str) -> Option<&str> {
        self.items
            .iter()
            .find(|i| i.key == key)
            .map(|i| i.icon_theme_path.as_str())
            .filter(|p| !p.is_empty())
    }

    /// Passive items are hidden (that is what the status means); the rest in registration order.
    pub fn view(&self) -> Vec<ItemView> {
        self.items
            .iter()
            .filter(|i| i.status != Status::Passive)
            .map(|i| ItemView {
                key: i.key.clone(),
                title: if i.title.is_empty() {
                    i.id.clone()
                } else {
                    i.title.clone()
                },
                icon_name: i.icon_name.clone(),
                status: i.status,
                pixmap: i.pixmap.as_ref().map(|p| PixmapView {
                    width: p.width,
                    height: p.height,
                    rgba: p.rgba.clone(),
                }),
            })
            .collect()
    }

    /// The registered items as the watcher reports them (`bus-name/path`).
    pub fn registered(&self) -> Vec<String> {
        self.items.iter().map(|i| i.key.clone()).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bus_names_and_paths_are_validated() {
        for ok in [
            ":1.23",
            ":1.2.3",
            "org.kde.StatusNotifierItem-1234-1",
            "com.example.App",
            "a_b.c-d",
        ] {
            assert!(valid_bus_name(ok), "{ok}");
        }
        for bad in [
            "",
            "plain",
            ":1",
            ":1.",
            ".a.b",
            "a..b",
            "a.b c",
            "org.1bad.name",
            &"a.".repeat(200),
            "a.b\n",
            "a.b/c",
            &format!("a.{}", "b".repeat(300)),
        ] {
            assert!(!valid_bus_name(bad), "{bad:?}");
        }
        for ok in [
            "/",
            "/StatusNotifierItem",
            "/org/ayatana/NotificationItem/app_1",
        ] {
            assert!(valid_object_path(ok), "{ok}");
        }
        for bad in [
            "",
            "no/leading",
            "/trailing/",
            "/a//b",
            "/a-b",
            "/a b",
            "/..",
            &format!("/{}", "a".repeat(300)),
        ] {
            assert!(!valid_object_path(bad), "{bad:?}");
        }
    }

    #[test]
    fn a_service_string_is_a_name_a_path_or_both() {
        assert_eq!(
            parse_service("org.app.Tray", ":1.5"),
            Some(("org.app.Tray".into(), "/StatusNotifierItem".into()))
        );
        assert_eq!(
            parse_service("/org/ayatana/item", ":1.5"),
            Some((":1.5".into(), "/org/ayatana/item".into()))
        );
        assert_eq!(
            parse_service(":1.9/MyItem", ":1.5"),
            Some((":1.9".into(), "/MyItem".into())),
            "name/path"
        );
        assert_eq!(
            parse_service(":1.9/My/Item", ":1.5"),
            Some((":1.9".into(), "/My/Item".into()))
        );
        for bad in ["", "garbage", "org..x", "/bad path", "name/", "name/a-b"] {
            assert_eq!(parse_service(bad, ":1.5"), None, "{bad:?}");
        }
        assert_eq!(
            parse_service("/item", "not a name"),
            None,
            "a path needs a valid sender to live on"
        );
    }

    #[test]
    fn pixels_are_converted_and_size_checked() {
        let argb = [255, 10, 20, 30, 128, 40, 50, 60];
        let p = pixmap_from_argb(2, 1, &argb).unwrap();
        assert_eq!(p.rgba, [10, 20, 30, 255, 40, 50, 60, 128]);
        assert!(
            pixmap_from_argb(2, 1, &argb[..7]).is_none(),
            "data must match the size"
        );
        assert!(pixmap_from_argb(0, 1, &[]).is_none());
        assert!(pixmap_from_argb(-1, 1, &argb).is_none());
        assert!(
            pixmap_from_argb(65, 1, &vec![0; 65 * 4]).is_none(),
            "too wide"
        );
        assert!(
            pixmap_from_argb(1, 65, &vec![0; 65 * 4]).is_none(),
            "too tall"
        );
        assert!(pixmap_from_argb(64, 64, &vec![0; 64 * 64 * 4]).is_some());
    }

    #[test]
    fn the_largest_usable_pixmap_wins() {
        let small = (1, 1, vec![255, 1, 2, 3]);
        let big = (2, 2, vec![255; 16]);
        let bad = (3, 3, vec![0; 4]);
        let best = best_pixmap(&[small, bad, big]).unwrap();
        assert_eq!((best.width, best.height), (2, 2));
        assert!(best_pixmap(&[]).is_none());
    }

    fn props() -> Props {
        Props {
            id: "app".into(),
            title: "My App".into(),
            icon_name: "my-icon".into(),
            status: "Active".into(),
            ..Props::default()
        }
    }

    #[test]
    fn registering_validates_and_does_not_duplicate_and_is_bounded() {
        let mut t = Tray::default();
        assert_eq!(
            t.register("org.a.B", "/StatusNotifierItem"),
            Registered::Added
        );
        assert_eq!(
            t.register("org.a.B", "/StatusNotifierItem"),
            Registered::AlreadyKnown
        );
        assert_eq!(t.register("bad name", "/x"), Registered::Invalid);
        assert_eq!(t.register("org.a.B", "bad"), Registered::Invalid);
        for i in 0..MAX_ITEMS {
            t.register(&format!("org.n{i}.App"), "/StatusNotifierItem");
        }
        assert_eq!(t.registered().len(), MAX_ITEMS);
        assert_eq!(
            t.register("org.extra.App", "/StatusNotifierItem"),
            Registered::Full
        );
    }

    #[test]
    fn what_an_item_says_is_sanitized_and_passive_ones_are_hidden() {
        let mut t = Tray::default();
        t.register("org.a.B", "/StatusNotifierItem");
        let key = t.registered()[0].clone();
        let mut hostile = props();
        hostile.title = "Pay\u{202e}pal\nverify".into();
        hostile.icon_name = "../../etc/passwd".into();
        hostile.icon_theme_path = "relative/../x".into();
        t.update(&key, &hostile);
        let v = &t.view()[0];
        assert!(!v.title.contains('\u{202e}') && !v.title.contains('\n'));
        assert_eq!(v.icon_name, "", "a path-like icon name is dropped");
        assert_eq!(t.icon_theme_path(&key), None);
        let mut passive = props();
        passive.status = "Passive".into();
        t.update(&key, &passive);
        assert!(t.view().is_empty(), "passive means hidden");
        let mut attention = props();
        attention.status = "NeedsAttention".into();
        t.update(&key, &attention);
        assert_eq!(t.view()[0].status, Status::Attention);
    }

    #[test]
    fn the_title_falls_back_to_the_id() {
        let mut t = Tray::default();
        t.register("org.a.B", "/StatusNotifierItem");
        let key = t.registered()[0].clone();
        let mut p = props();
        p.title = String::new();
        t.update(&key, &p);
        assert_eq!(t.view()[0].title, "app");
    }

    #[test]
    fn an_application_that_leaves_takes_its_items_with_it() {
        let mut t = Tray::default();
        t.register("org.a.B", "/one");
        t.register("org.a.B", "/two");
        t.register("org.c.D", "/StatusNotifierItem");
        let gone = t.remove_bus_name("org.a.B");
        assert_eq!(gone.len(), 2);
        assert_eq!(t.registered(), ["org.c.D/StatusNotifierItem"]);
        assert!(!t.remove("org.a.B/one"));
        assert!(t.remove("org.c.D/StatusNotifierItem"));
    }

    #[test]
    fn the_page_can_only_address_items_the_model_made() {
        let mut t = Tray::default();
        t.register("org.a.B", "/StatusNotifierItem");
        assert!(t.knows("org.a.B/StatusNotifierItem"));
        assert_eq!(
            t.address("org.a.B/StatusNotifierItem"),
            Some(("org.a.B", "/StatusNotifierItem"))
        );
        assert!(!t.knows("org.evil.X/StatusNotifierItem"));
        assert_eq!(t.address("org.evil.X/StatusNotifierItem"), None);
    }

    #[test]
    fn an_absurd_theme_path_is_ignored_and_a_good_one_kept() {
        let mut t = Tray::default();
        t.register("org.a.B", "/StatusNotifierItem");
        let key = t.registered()[0].clone();
        let mut p = props();
        p.icon_theme_path = "/home/me/.local/share/app/icons".into();
        t.update(&key, &p);
        assert_eq!(
            t.icon_theme_path(&key),
            Some("/home/me/.local/share/app/icons")
        );
        p.icon_theme_path = format!("/{}", "a".repeat(500));
        t.update(&key, &p);
        assert_eq!(t.icon_theme_path(&key), None);
    }

    #[test]
    fn a_menu_path_is_kept_only_if_it_is_a_real_object_path() {
        let mut t = Tray::default();
        t.register("org.a.B", "/StatusNotifierItem");
        let key = t.registered()[0].clone();
        let mut p = props();
        p.menu = "/MenuBar".into();
        t.update(&key, &p);
        assert_eq!(t.menu_path(&key), Some("/MenuBar"));
        for bad in ["/", "", "not/a/path", "/a b", "/a//b"] {
            p.menu = bad.into();
            t.update(&key, &p);
            assert_eq!(t.menu_path(&key), None, "{bad:?}");
        }
    }
}
