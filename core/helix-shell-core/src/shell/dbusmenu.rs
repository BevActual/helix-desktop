//! A tray application's menu (the `com.canonical.dbusmenu` layout) as a pure model. The layout comes from the application, so it is hostile:
//! depth, size and text are bounded, labels lose their keyboard-mnemonic underscores and are sanitized, and the page can only activate an id
//! that this layout contained, was enabled, and was not a separator or a submenu.

use serde::Serialize;
use serde_json::Value;

use crate::display::sanitize_for_display;

pub const MAX_DEPTH: usize = 3;
pub const MAX_ITEMS: usize = 80;
const MAX_LABEL: usize = 80;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    Item,
    Separator,
    Check,
    Radio,
    Submenu,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct MenuItem {
    pub id: i32,
    pub label: String,
    pub enabled: bool,
    pub kind: Kind,
    pub checked: bool,
    pub children: Vec<MenuItem>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Menu {
    items: Vec<MenuItem>,
}

/// `_File` is the mnemonic F; `__` is a literal underscore.
pub fn strip_mnemonic(label: &str) -> String {
    let mut out = String::new();
    let mut chars = label.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '_' {
            if chars.peek() == Some(&'_') {
                out.push('_');
                chars.next();
            }
        } else {
            out.push(c);
        }
    }
    out
}

fn flag(props: &Value, key: &str, default: bool) -> bool {
    props.get(key).and_then(Value::as_bool).unwrap_or(default)
}

fn text(props: &Value, key: &str) -> String {
    props
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string()
}

/// One node: `{"id": n, "props": {...}, "children": [...]}`.
fn node(value: &Value, depth: usize, budget: &mut usize) -> Option<MenuItem> {
    if *budget == 0 {
        return None;
    }
    let id = i32::try_from(value.get("id")?.as_i64()?).ok()?;
    let props = value.get("props").cloned().unwrap_or(Value::Null);
    if !flag(&props, "visible", true) {
        return None;
    }
    *budget -= 1;
    let mut children = Vec::new();
    if depth < MAX_DEPTH {
        if let Some(list) = value.get("children").and_then(Value::as_array) {
            for child in list {
                if let Some(item) = node(child, depth + 1, budget) {
                    children.push(item);
                }
            }
        }
    }
    let kind = if text(&props, "type") == "separator" {
        Kind::Separator
    } else if !children.is_empty() || text(&props, "children-display") == "submenu" {
        Kind::Submenu
    } else {
        match text(&props, "toggle-type").as_str() {
            "checkmark" => Kind::Check,
            "radio" => Kind::Radio,
            _ => Kind::Item,
        }
    };
    Some(MenuItem {
        id,
        label: sanitize_for_display(&strip_mnemonic(&text(&props, "label")), MAX_LABEL),
        enabled: flag(&props, "enabled", true),
        kind,
        checked: props.get("toggle-state").and_then(Value::as_i64) == Some(1),
        children,
    })
}

fn collect<'a>(items: &'a [MenuItem], out: &mut Vec<&'a MenuItem>) {
    for item in items {
        out.push(item);
        collect(&item.children, out);
    }
}

impl Menu {
    /// From the root node of a `GetLayout` reply (the root itself is not an entry; its children are the menu).
    pub fn from_layout(root: &Value) -> Self {
        let mut budget = MAX_ITEMS;
        let items = root
            .get("children")
            .and_then(Value::as_array)
            .map(|list| {
                list.iter()
                    .filter_map(|child| node(child, 1, &mut budget))
                    .collect()
            })
            .unwrap_or_default();
        let mut menu = Self { items };
        menu.trim_separators();
        menu
    }

    /// No separator at the start or end, none twice in a row (an application's menu often has them for entries it then hides).
    fn trim_separators(&mut self) {
        fn clean(items: &mut Vec<MenuItem>) {
            for item in items.iter_mut() {
                clean(&mut item.children);
            }
            let mut previous_separator = true;
            items.retain(|i| {
                let separator = i.kind == Kind::Separator;
                let keep = !(separator && previous_separator);
                previous_separator = separator;
                keep
            });
            while items.last().is_some_and(|i| i.kind == Kind::Separator) {
                items.pop();
            }
        }
        clean(&mut self.items);
    }

    pub fn items(&self) -> &[MenuItem] {
        &self.items
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// Whether choosing `id` may be reported to the application: it must be in this menu, enabled, and a real entry (not a separator or a
    /// submenu header).
    pub fn may_activate(&self, id: i32) -> bool {
        let mut all = Vec::new();
        collect(&self.items, &mut all);
        all.iter()
            .any(|i| i.id == id && i.enabled && !matches!(i.kind, Kind::Separator | Kind::Submenu))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn item(id: i64, label: &str) -> Value {
        json!({"id": id, "props": {"label": label}, "children": []})
    }

    fn root(children: Vec<Value>) -> Value {
        json!({"id": 0, "props": {"children-display": "submenu"}, "children": children})
    }

    #[test]
    fn mnemonics_lose_their_underscore_and_double_underscores_are_literal() {
        assert_eq!(strip_mnemonic("_File"), "File");
        assert_eq!(strip_mnemonic("Save _As…"), "Save As…");
        assert_eq!(strip_mnemonic("snake__case"), "snake_case");
        assert_eq!(strip_mnemonic("trailing_"), "trailing");
    }

    #[test]
    fn a_layout_becomes_labelled_entries_with_kinds() {
        let menu = Menu::from_layout(&root(vec![
            item(1, "_Open"),
            json!({"id": 2, "props": {"type": "separator"}}),
            json!({"id": 3, "props": {"label": "Mute", "toggle-type": "checkmark", "toggle-state": 1}}),
            json!({"id": 4, "props": {"label": "Quality", "children-display": "submenu"}, "children": [item(5, "High"), item(6, "Low")]}),
            json!({"id": 7, "props": {"label": "Gone", "enabled": false}}),
            json!({"id": 8, "props": {"label": "Hidden", "visible": false}}),
        ]));
        let items = menu.items();
        let labels: Vec<&str> = items.iter().map(|i| i.label.as_str()).collect();
        assert_eq!(
            labels,
            ["Open", "", "Mute", "Quality", "Gone"],
            "a hidden entry is not listed"
        );
        assert_eq!(items[1].kind, Kind::Separator);
        assert_eq!((items[2].kind, items[2].checked), (Kind::Check, true));
        assert_eq!((items[3].kind, items[3].children.len()), (Kind::Submenu, 2));
        assert!(!items[4].enabled);
    }

    #[test]
    fn separators_are_tidied() {
        let sep = |id| json!({"id": id, "props": {"type": "separator"}});
        let menu = Menu::from_layout(&root(vec![
            sep(1),
            sep(2),
            item(3, "A"),
            sep(4),
            sep(5),
            item(6, "B"),
            sep(7),
        ]));
        let kinds: Vec<Kind> = menu.items().iter().map(|i| i.kind).collect();
        assert_eq!(kinds, [Kind::Item, Kind::Separator, Kind::Item]);
    }

    #[test]
    fn only_listed_enabled_real_entries_may_be_activated() {
        let menu = Menu::from_layout(&root(vec![
            item(1, "Open"),
            json!({"id": 2, "props": {"type": "separator"}}),
            json!({"id": 3, "props": {"label": "Sub", "children-display": "submenu"}, "children": [item(4, "Inner")]}),
            json!({"id": 5, "props": {"label": "Off", "enabled": false}}),
            item(6, "Last"),
        ]));
        assert!(menu.may_activate(1));
        assert!(menu.may_activate(4), "an entry inside a submenu");
        assert!(!menu.may_activate(2), "a separator");
        assert!(!menu.may_activate(3), "a submenu header");
        assert!(!menu.may_activate(5), "disabled");
        assert!(!menu.may_activate(99), "not in the menu");
    }

    #[test]
    fn labels_are_sanitized_and_bounded() {
        let menu = Menu::from_layout(&root(vec![
            item(1, "Pay\u{202e}pal\nverify"),
            item(2, &"x".repeat(1000)),
        ]));
        assert!(
            !menu.items()[0].label.contains('\u{202e}') && !menu.items()[0].label.contains('\n')
        );
        assert!(menu.items()[1].label.chars().count() <= MAX_LABEL + 1);
    }

    #[test]
    fn depth_and_size_are_bounded() {
        let mut deep = item(100, "leaf");
        for i in 0..20 {
            deep = json!({"id": 200 + i, "props": {"label": "level"}, "children": [deep]});
        }
        let menu = Menu::from_layout(&root(vec![deep]));
        fn depth(items: &[MenuItem]) -> usize {
            items
                .iter()
                .map(|i| 1 + depth(&i.children))
                .max()
                .unwrap_or(0)
        }
        assert!(depth(menu.items()) <= MAX_DEPTH, "{}", depth(menu.items()));
        let many: Vec<Value> = (0..500).map(|i| item(i + 1, "x")).collect();
        assert_eq!(Menu::from_layout(&root(many)).items().len(), MAX_ITEMS);
    }

    #[test]
    fn nonsense_layouts_give_an_empty_menu() {
        for bad in [
            json!(null),
            json!({}),
            json!({"children": "no"}),
            json!({"children": [{"props": {}}]}),
            json!({"children": [{"id": "x"}]}),
        ] {
            assert!(Menu::from_layout(&bad).is_empty(), "{bad}");
        }
        assert!(
            Menu::from_layout(&root(vec![json!({"id": 99999999999_i64, "props": {}})])).is_empty(),
            "an id that is not an i32"
        );
    }

    #[test]
    fn a_submenu_whose_entries_have_not_arrived_yet_is_still_a_submenu() {
        let menu = Menu::from_layout(&root(vec![
            json!({"id": 1, "props": {"label": "Later", "children-display": "submenu"}, "children": []}),
        ]));
        assert_eq!(menu.items()[0].kind, Kind::Submenu);
        assert!(!menu.may_activate(1), "its header is not an action");
    }
}
