//! The notification store behind `org.freedesktop.Notifications`, as a pure model. Everything an application sends (name, summary,
//! body, action labels) is hostile text: it is sanitized and bounded here before any view exists, markup is reduced to plain text,
//! and nothing from a notification is ever handed to a trusted surface. Time is passed in (milliseconds on any monotonic clock) so
//! the model needs no timers: the host asks [`Notifications::next_deadline`] and sleeps until then.

use serde::Serialize;

use crate::display::sanitize_for_display;

pub const MAX_ITEMS: usize = 50;
pub const MAX_PER_APP: usize = 8;
pub const MAX_TOASTS: usize = 3;
pub const MAX_ACTIONS: usize = 4;
/// More than this many from one application inside [`FLOOD_WINDOW_MS`] are dropped.
pub const FLOOD_LIMIT: usize = 20;
pub const FLOOD_WINDOW_MS: u64 = 10_000;
pub const DEFAULT_TOAST_MS: u64 = 6_000;
pub const MAX_TOAST_MS: u64 = 15_000;
const MAX_KEY: usize = 64;
/// Text is cut to this many characters *before* it is parsed, so a hostile body costs a bounded amount of work.
const MAX_RAW_SUMMARY: usize = 600;
const MAX_RAW_BODY: usize = 3000;
/// A tag is at most this long; a `<` with no `>` close by is just a character.
const MAX_TAG: usize = 64;
/// The most flood-history entries kept, whatever is thrown at the store.
const MAX_RECENT: usize = 4 * FLOOD_LIMIT;

pub const CLOSED_EXPIRED: u32 = 1;
pub const CLOSED_DISMISSED: u32 = 2;
pub const CLOSED_BY_APP: u32 = 3;
pub const CLOSED_UNDEFINED: u32 = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
#[cfg_attr(feature = "ts", derive(ts_rs::TS), ts(export))]
pub enum Urgency {
    Low,
    Normal,
    Critical,
}

impl Urgency {
    pub fn from_hint(value: u8) -> Self {
        match value {
            0 => Self::Low,
            2 => Self::Critical,
            _ => Self::Normal,
        }
    }
}

/// One `Notify` call, as received.
#[derive(Clone, Debug)]
pub struct Incoming {
    /// The bus connection that sent it (unforgeable, unlike the name it gives itself). Empty when unknown.
    pub owner: String,
    pub app: String,
    pub replaces: u32,
    pub summary: String,
    pub body: String,
    /// The spec's flat list: key, label, key, label ...
    pub actions: Vec<String>,
    pub urgency: Urgency,
    /// -1: the server decides; 0: never expires; otherwise milliseconds.
    pub timeout_ms: i32,
}

/// What the host must put on the bus.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    Closed { id: u32, reason: u32 },
    ActionInvoked { id: u32, key: String },
}

#[derive(Clone, Debug)]
struct Item {
    id: u32,
    owner: String,
    /// Starts at 1 and grows each time the notification is replaced: a click is only honoured for the revision that was on screen.
    revision: u32,
    app: String,
    summary: String,
    body: String,
    actions: Vec<(String, String)>,
    urgency: Urgency,
    created: u64,
    /// While set and in the future, the notification is "live": it may be a toast and its actions work.
    live_until: Option<u64>,
    live: bool,
    toast_until: Option<u64>,
    read: bool,
    /// The application is set to Quiet: kept in the list, never a banner (unless urgent).
    quiet_app: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Notifications {
    next_id: u32,
    items: Vec<Item>,
    dnd: bool,
    /// Quiet hours are in effect right now. Ordinary banners stay hidden; the manual switch is unchanged.
    quiet: bool,
    recent: Vec<(String, u64)>,
    /// What the person chose in Settings; `None` means the defaults above.
    default_toast_ms: Option<u64>,
    max_toasts: Option<usize>,
    /// What the person chose per application (lower-cased name): `true` is Off (dropped), `false` is Quiet (listed, no banner).
    app_rules: std::collections::HashMap<String, bool>,
    /// Applications that have sent a notification, most recent last, so Settings can offer a rule for each.
    seen_apps: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS), ts(export))]
pub struct ActionView {
    pub label: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS), ts(export))]
pub struct ItemView {
    pub id: u32,
    pub revision: u32,
    pub app: String,
    pub summary: String,
    pub body: String,
    pub urgency: Urgency,
    pub live: bool,
    pub read: bool,
    pub actions: Vec<ActionView>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS), ts(export))]
pub struct NotificationsView {
    pub dnd: bool,
    pub unread: usize,
    /// Newest first, at most [`MAX_TOASTS`]: what to draw over the desktop right now.
    pub toasts: Vec<ItemView>,
    /// Newest first: the shade.
    pub items: Vec<ItemView>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    Unknown,
    /// The notification was replaced after the person saw it, so the click was for something else.
    Stale,
    NotLive,
    NoSuchAction,
}

/// Application markup (`<b>`, `<i>`, `<u>`, `<a href>`, `<img/>`) reduced to its text, and the XML entities undone. Everything else is
/// kept as text: a body is never interpreted as anything but characters.
pub fn plain_text(markup: &str) -> String {
    let mut out = String::with_capacity(markup.len());
    let mut rest = markup;
    while let Some(start) = rest.find('<') {
        out.push_str(&rest[..start]);
        let tail = &rest[start..];
        let known = tail
            .char_indices()
            .take(MAX_TAG)
            .find(|(_, c)| *c == '>')
            .map(|(end, _)| end)
            .and_then(|end| {
                let name: String = tail[1..end]
                    .trim_start_matches('/')
                    .chars()
                    .take_while(|c| c.is_ascii_alphabetic())
                    .collect::<String>()
                    .to_ascii_lowercase();
                ["b", "i", "u", "a", "img", "br"]
                    .contains(&name.as_str())
                    .then_some(end)
            });
        match known {
            Some(end) => rest = &tail[end + 1..],
            None => {
                out.push('<');
                rest = &tail[1..];
            }
        }
    }
    out.push_str(rest);
    out.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&amp;", "&")
}

impl Notifications {
    pub fn new() -> Self {
        Self {
            next_id: 1,
            ..Self::default()
        }
    }

    fn fresh_id(&mut self) -> u32 {
        let id = self.next_id.max(1);
        self.next_id = id.checked_add(1).unwrap_or(1);
        id
    }

    /// Keyed by who sent it (the bus connection), so renaming itself does not buy a sender a fresh allowance. The history is bounded.
    fn flooding(&mut self, key: &str, now: u64) -> bool {
        self.recent
            .retain(|(_, at)| now.saturating_sub(*at) < FLOOD_WINDOW_MS);
        let count = self.recent.iter().filter(|(name, _)| name == key).count();
        self.recent.push((key.to_string(), now));
        if self.recent.len() > MAX_RECENT {
            self.recent.remove(0);
        }
        count >= FLOOD_LIMIT
    }

    /// Stores or replaces a notification and returns its id. The events are what the host must signal.
    pub fn notify(&mut self, incoming: Incoming, now: u64) -> (u32, Vec<Event>) {
        let mut events = Vec::new();
        let app = sanitize_for_display(&incoming.app, 40);
        self.note_app(&app);
        let rule = self.app_rules.get(&app.trim().to_lowercase()).copied();
        // Off: not kept at all. An urgent one is never silenced by a rule.
        if rule == Some(true) && incoming.urgency != Urgency::Critical {
            return (self.fresh_id(), events);
        }
        // Only the connection that posted a notification can replace it.
        let replaced = incoming.replaces != 0
            && self
                .items
                .iter()
                .any(|item| item.id == incoming.replaces && item.owner == incoming.owner);
        let id = if replaced {
            incoming.replaces
        } else {
            self.fresh_id()
        };
        let flood_key = if incoming.owner.is_empty() {
            app.clone()
        } else {
            incoming.owner.clone()
        };
        if !replaced && self.flooding(&flood_key, now) {
            return (id, events);
        }
        let mut actions = Vec::new();
        for pair in incoming.actions.chunks(2) {
            if let [key, label] = pair {
                // "default" is the click on the notification itself; it has no button of its own.
                if actions.len() < MAX_ACTIONS
                    && !key.is_empty()
                    && key.len() <= MAX_KEY
                    && key != "default"
                {
                    actions.push((key.clone(), sanitize_for_display(label, 40)));
                }
            }
        }
        let live_for = match (incoming.urgency, incoming.timeout_ms) {
            (Urgency::Critical, _) | (_, 0) => None,
            (_, ms) if ms > 0 => Some(u64::try_from(ms).unwrap_or(0)),
            _ => Some(self.default_toast_ms.unwrap_or(DEFAULT_TOAST_MS)),
        };
        let toast_for = live_for.unwrap_or(MAX_TOAST_MS).clamp(1_000, MAX_TOAST_MS);
        let cut = |text: &str, max: usize| -> String { text.chars().take(max).collect() };
        let revision = self
            .items
            .iter()
            .find(|existing| replaced && existing.id == id)
            .map_or(1, |existing| existing.revision.saturating_add(1));
        let item = Item {
            id,
            owner: incoming.owner.clone(),
            revision,
            app: app.clone(),
            summary: sanitize_for_display(
                &plain_text(&cut(&incoming.summary, MAX_RAW_SUMMARY)),
                120,
            ),
            body: sanitize_for_display(&plain_text(&cut(&incoming.body, MAX_RAW_BODY)), 400),
            actions,
            urgency: incoming.urgency,
            created: now,
            live_until: live_for.map(|ms| now.saturating_add(ms)),
            live: true,
            toast_until: Some(now.saturating_add(toast_for)),
            read: false,
            quiet_app: rule == Some(false) && incoming.urgency != Urgency::Critical,
        };
        if replaced {
            if let Some(slot) = self.items.iter_mut().find(|existing| existing.id == id) {
                *slot = item;
            }
        } else {
            self.items.push(item);
            // One application cannot crowd out the rest: its oldest go first.
            // "Same sender" is the bus connection when known (a name it gives itself proves nothing), else the name.
            let same = |i: &Item| {
                if flood_key == app {
                    i.app == app
                } else {
                    i.owner == incoming.owner
                }
            };
            while self.items.iter().filter(|i| same(i)).count() > MAX_PER_APP {
                if let Some(index) = self.items.iter().position(&same) {
                    let gone = self.items.remove(index);
                    events.push(Event::Closed {
                        id: gone.id,
                        reason: CLOSED_UNDEFINED,
                    });
                }
            }
            while self.items.len() > MAX_ITEMS {
                let gone = self.items.remove(0);
                events.push(Event::Closed {
                    id: gone.id,
                    reason: CLOSED_UNDEFINED,
                });
            }
        }
        (id, events)
    }

    /// The application asked to close it (`CloseNotification`).
    pub fn close_by_app(&mut self, id: u32) -> Vec<Event> {
        self.remove(id, CLOSED_BY_APP)
    }

    fn remove(&mut self, id: u32, reason: u32) -> Vec<Event> {
        match self.items.iter().position(|item| item.id == id) {
            Some(index) => {
                self.items.remove(index);
                vec![Event::Closed { id, reason }]
            }
            None => Vec::new(),
        }
    }

    pub fn dismiss(&mut self, id: u32) -> Result<Vec<Event>, RequestError> {
        let events = self.remove(id, CLOSED_DISMISSED);
        if events.is_empty() {
            Err(RequestError::Unknown)
        } else {
            Ok(events)
        }
    }

    pub fn clear(&mut self) -> Vec<Event> {
        let ids: Vec<u32> = self.items.iter().map(|item| item.id).collect();
        ids.into_iter()
            .flat_map(|id| self.remove(id, CLOSED_DISMISSED))
            .collect()
    }

    /// The person pressed one of a live notification's buttons. The key comes from the stored list, never from the caller.
    pub fn invoke(
        &mut self,
        id: u32,
        revision: u32,
        index: usize,
    ) -> Result<Vec<Event>, RequestError> {
        let item = self
            .items
            .iter()
            .find(|item| item.id == id)
            .ok_or(RequestError::Unknown)?;
        if item.revision != revision {
            return Err(RequestError::Stale);
        }
        if !item.live {
            return Err(RequestError::NotLive);
        }
        let key = item
            .actions
            .get(index)
            .ok_or(RequestError::NoSuchAction)?
            .0
            .clone();
        let mut events = vec![Event::ActionInvoked { id, key }];
        events.extend(self.remove(id, CLOSED_DISMISSED));
        Ok(events)
    }

    /// How long a banner stays when its application did not ask for a time, and how many show at once (both bounded here).
    /// Only banners drawn from now on use the time; a banner already showing keeps its own.
    pub fn set_toast_prefs(&mut self, default_ms: u64, max_toasts: usize) {
        self.default_toast_ms = Some(default_ms.clamp(1_000, MAX_TOAST_MS));
        self.max_toasts = Some(max_toasts.clamp(1, MAX_TOASTS));
    }

    /// Per-application rules from Settings: (application name, `true` for Off or `false` for Quiet).
    pub fn set_app_rules(&mut self, rules: impl IntoIterator<Item = (String, bool)>) {
        self.app_rules = rules
            .into_iter()
            .map(|(name, off)| (name.trim().to_lowercase(), off))
            .collect();
    }

    fn note_app(&mut self, app: &str) {
        if app.is_empty() || self.seen_apps.iter().any(|a| a.eq_ignore_ascii_case(app)) {
            return;
        }
        self.seen_apps.push(app.to_string());
        if self.seen_apps.len() > 100 {
            self.seen_apps.remove(0);
        }
    }

    /// The applications that have sent a notification since the shell started (and any the host added back from its last run).
    pub fn seen_apps(&self) -> &[String] {
        &self.seen_apps
    }

    /// Remembers applications seen in an earlier run.
    pub fn remember_apps(&mut self, apps: impl IntoIterator<Item = String>) {
        for app in apps {
            self.note_app(&app);
        }
    }

    pub fn set_dnd(&mut self, on: bool) {
        self.dnd = on;
    }

    /// Quiet hours, as the shell computed them from the clock. This does not change the Do not disturb switch.
    pub fn set_quiet(&mut self, on: bool) {
        self.quiet = on;
    }

    /// The shade was looked at: nothing is unread any more.
    pub fn mark_all_read(&mut self) {
        for item in &mut self.items {
            item.read = true;
        }
    }

    /// The earliest moment [`Self::expire`] has something to do, for the host to sleep until. No polling.
    pub fn next_deadline(&self) -> Option<u64> {
        self.items
            .iter()
            .flat_map(|item| [item.live_until.filter(|_| item.live), item.toast_until])
            .flatten()
            .min()
    }

    /// Lets notifications whose time has passed stop being live (their buttons no longer work; they stay in the shade).
    pub fn expire(&mut self, now: u64) -> Vec<Event> {
        let mut events = Vec::new();
        for item in &mut self.items {
            if item.toast_until.is_some_and(|until| until <= now) {
                item.toast_until = None;
            }
            if item.live && item.live_until.is_some_and(|until| until <= now) {
                item.live = false;
                events.push(Event::Closed {
                    id: item.id,
                    reason: CLOSED_EXPIRED,
                });
            }
        }
        events
    }

    pub fn view(&self, now: u64) -> NotificationsView {
        let row = |item: &Item| ItemView {
            id: item.id,
            revision: item.revision,
            app: item.app.clone(),
            summary: item.summary.clone(),
            body: item.body.clone(),
            urgency: item.urgency,
            live: item.live,
            read: item.read,
            actions: if item.live {
                item.actions
                    .iter()
                    .map(|(_, label)| ActionView {
                        label: label.clone(),
                    })
                    .collect()
            } else {
                Vec::new()
            },
        };
        let mut newest: Vec<&Item> = self.items.iter().collect();
        newest.sort_by(|a, b| b.created.cmp(&a.created).then(b.id.cmp(&a.id)));
        let toasts = newest
            .iter()
            .filter(|item| {
                item.toast_until.is_some_and(|until| until > now)
                    && (!(self.dnd || self.quiet) || item.urgency == Urgency::Critical)
                    && item.urgency != Urgency::Low
                    && !item.quiet_app
            })
            .take(self.max_toasts.unwrap_or(MAX_TOASTS).clamp(1, MAX_TOASTS))
            .map(|item| row(item))
            .collect();
        NotificationsView {
            dnd: self.dnd,
            unread: self.items.iter().filter(|item| !item.read).count(),
            toasts,
            items: newest.iter().map(|item| row(item)).collect(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn incoming(app: &str, summary: &str) -> Incoming {
        Incoming {
            owner: String::new(),
            app: app.into(),
            replaces: 0,
            summary: summary.into(),
            body: String::new(),
            actions: Vec::new(),
            urgency: Urgency::Normal,
            timeout_ms: -1,
        }
    }

    #[test]
    fn a_notification_is_stored_shown_as_a_toast_and_listed() {
        let mut n = Notifications::new();
        let (id, events) = n.notify(incoming("Mail", "Hello"), 0);
        assert_eq!(id, 1);
        assert!(events.is_empty());
        let view = n.view(100);
        assert_eq!(view.toasts.len(), 1);
        assert_eq!(view.items[0].summary, "Hello");
        assert_eq!(view.unread, 1);
    }

    #[test]
    fn text_is_sanitized_and_markup_is_only_text() {
        let mut n = Notifications::new();
        let mut hostile = incoming(
            "Bank\u{202e}\nOf",
            "<b>Allow</b> &amp; <i>approve</i>\u{202e}",
        );
        hostile.body = "<a href=\"x\">click</a><img src=\"y\"/> &lt;script&gt; <3".into();
        n.notify(hostile, 0);
        let row = &n.view(0).items[0];
        assert!(!row.app.contains('\u{202e}') && !row.app.contains('\n'));
        assert_eq!(row.summary, "Allow & approve");
        assert_eq!(row.body, "click <script> <3");
    }

    #[test]
    fn long_text_is_bounded() {
        let mut n = Notifications::new();
        let mut big = incoming(&"a".repeat(500), &"s".repeat(5000));
        big.body = "b".repeat(50_000);
        n.notify(big, 0);
        let row = &n.view(0).items[0];
        // The limit counts characters kept; a cut text gains one ellipsis.
        assert!(row.app.chars().count() <= 41);
        assert!(row.summary.chars().count() <= 121);
        assert!(row.body.chars().count() <= 401);
    }

    #[test]
    fn replacing_keeps_the_id_and_unknown_ids_get_a_new_one() {
        let mut n = Notifications::new();
        let (id, _) = n.notify(incoming("A", "one"), 0);
        let mut again = incoming("A", "two");
        again.replaces = id;
        assert_eq!(n.notify(again, 5).0, id);
        assert_eq!(n.view(5).items.len(), 1);
        assert_eq!(n.view(5).items[0].summary, "two");
        let mut stale = incoming("A", "three");
        stale.replaces = 999;
        assert_ne!(n.notify(stale, 6).0, 999);
        assert_eq!(n.view(6).items.len(), 2);
    }

    #[test]
    fn actions_come_from_the_stored_list_and_are_bounded() {
        let mut n = Notifications::new();
        let mut with = incoming("A", "x");
        with.actions = [
            "default", "Open", "reply", "Reply", "k3", "B", "k4", "C", "k5", "D", "k6", "E", "",
            "empty", "odd",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        let (id, _) = n.notify(with, 0);
        let view = n.view(0);
        let labels: Vec<_> = view.items[0]
            .actions
            .iter()
            .map(|a| a.label.as_str())
            .collect();
        assert_eq!(
            labels,
            ["Reply", "B", "C", "D"],
            "default has no button, at most four, empty keys dropped"
        );
        let events = n.invoke(id, 1, 0).unwrap();
        assert_eq!(
            events,
            vec![
                Event::ActionInvoked {
                    id,
                    key: "reply".into()
                },
                Event::Closed {
                    id,
                    reason: CLOSED_DISMISSED
                }
            ]
        );
        assert_eq!(n.invoke(id, 1, 0), Err(RequestError::Unknown));
    }

    #[test]
    fn an_action_index_that_was_not_offered_is_refused() {
        let mut n = Notifications::new();
        let mut with = incoming("A", "x");
        with.actions = vec!["k".into(), "Label".into()];
        let (id, _) = n.notify(with, 0);
        assert_eq!(n.invoke(id, 1, 1), Err(RequestError::NoSuchAction));
        assert!(n.invoke(id, 1, 0).is_ok());
    }

    #[test]
    fn an_expired_notification_stays_in_the_shade_but_its_buttons_stop() {
        let mut n = Notifications::new();
        let mut with = incoming("A", "x");
        with.actions = vec!["k".into(), "Label".into()];
        with.timeout_ms = 2000;
        let (id, _) = n.notify(with, 0);
        assert_eq!(n.next_deadline(), Some(2000));
        let events = n.expire(2500);
        assert_eq!(
            events,
            vec![Event::Closed {
                id,
                reason: CLOSED_EXPIRED
            }]
        );
        let view = n.view(2500);
        assert_eq!(view.items.len(), 1);
        assert!(view.items[0].actions.is_empty());
        assert_eq!(n.invoke(id, 1, 0), Err(RequestError::NotLive));
        assert!(view.toasts.is_empty());
    }

    #[test]
    fn a_quiet_application_is_listed_but_never_a_banner_and_an_off_one_is_dropped() {
        let mut n = Notifications::new();
        n.set_app_rules([("Chat".to_string(), false), ("Spam".to_string(), true)]);
        n.notify(incoming("Chat", "hi"), 1_000);
        n.notify(incoming("Spam", "buy"), 1_000);
        n.notify(incoming("Mail", "you have mail"), 1_000);
        let view = n.view(1_500);
        let apps = |items: &[ItemView]| items.iter().map(|i| i.app.clone()).collect::<Vec<_>>();
        assert_eq!(apps(&view.items), ["Mail", "Chat"], "Spam is not kept");
        assert_eq!(
            apps(&view.toasts),
            ["Mail"],
            "Chat is listed but is not a banner"
        );
        // An urgent one is never silenced by a rule.
        let mut urgent = incoming("Spam", "disk is full");
        urgent.urgency = Urgency::Critical;
        n.notify(urgent, 2_000);
        let view = n.view(2_500);
        assert!(apps(&view.toasts).contains(&"Spam".to_string()));
        // All three are offered in Settings.
        assert_eq!(n.seen_apps().len(), 3);
        n.remember_apps(["Old".to_string(), "chat".to_string()]);
        assert_eq!(n.seen_apps().len(), 4, "a known name is not added twice");
    }

    #[test]
    fn critical_never_expires_and_ignores_do_not_disturb() {
        let mut n = Notifications::new();
        let mut c = incoming("Power", "Battery 3%");
        c.urgency = Urgency::Critical;
        c.timeout_ms = 1000;
        let (power, _) = n.notify(c, 0);
        n.set_dnd(true);
        n.notify(incoming("Chat", "hi"), 1);
        let view = n.view(2);
        assert_eq!(
            view.toasts.len(),
            1,
            "only the critical one is shown while do-not-disturb is on"
        );
        assert_eq!(view.toasts[0].app, "Power");
        assert_eq!(view.items.len(), 2, "the other is still recorded");
        assert!(view.dnd);
        n.set_dnd(false);
        n.set_quiet(true);
        let view = n.view(3);
        assert!(
            !view.dnd,
            "quiet hours do not flip the Do not disturb switch"
        );
        assert_eq!(view.toasts.len(), 1);
        assert_eq!(view.toasts[0].app, "Power");
        assert_eq!(view.items.len(), 2);
        let events = n.expire(1_000_000);
        assert!(events.iter().all(|e| *e
            != Event::Closed {
                id: power,
                reason: CLOSED_EXPIRED
            }));
        assert_eq!(events.len(), 1, "the ordinary one expires");
    }

    #[test]
    fn low_urgency_is_recorded_but_never_a_toast() {
        let mut n = Notifications::new();
        let mut low = incoming("A", "quiet");
        low.urgency = Urgency::Low;
        n.notify(low, 0);
        assert!(n.view(0).toasts.is_empty());
        assert_eq!(n.view(0).items.len(), 1);
    }

    #[test]
    fn at_most_three_toasts_newest_first() {
        let mut n = Notifications::new();
        for i in 0..5 {
            n.notify(incoming(&format!("App{i}"), &format!("m{i}")), i as u64);
        }
        let view = n.view(10);
        assert_eq!(view.toasts.len(), MAX_TOASTS);
        assert_eq!(view.toasts[0].summary, "m4");
        assert_eq!(view.items.len(), 5);
    }

    #[test]
    fn the_chosen_banner_time_and_count_apply_where_the_app_did_not_say() {
        let mut n = Notifications::new();
        n.set_toast_prefs(2_000, 1);
        n.notify(incoming("A", "x"), 100);
        assert_eq!(n.next_deadline(), Some(100 + 2_000));
        n.notify(incoming("B", "y"), 101);
        let view = n.view(102);
        assert_eq!(view.toasts.len(), 1, "one banner at once");
        assert_eq!(view.toasts[0].summary, "y");
        assert_eq!(view.items.len(), 2, "the other waits in the list");
        let mut timed = incoming("C", "z");
        timed.timeout_ms = 9_000;
        n.expire(5_000);
        n.notify(timed, 5_000);
        assert_eq!(
            n.next_deadline(),
            Some(14_000),
            "an application that asks for a time keeps it"
        );
        n.set_toast_prefs(0, 99);
        for (i, app) in ["D", "E", "F"].iter().enumerate() {
            n.notify(incoming(app, "w"), 6_000 + i as u64);
        }
        assert_eq!(
            n.view(6_010).toasts.len(),
            MAX_TOASTS,
            "the count stays within the model's bound"
        );
    }

    #[test]
    fn toasts_end_on_time_and_the_deadline_is_reported() {
        let mut n = Notifications::new();
        n.notify(incoming("A", "x"), 100);
        assert_eq!(n.next_deadline(), Some(100 + DEFAULT_TOAST_MS));
        assert_eq!(n.view(100 + DEFAULT_TOAST_MS - 1).toasts.len(), 1);
        n.expire(100 + DEFAULT_TOAST_MS);
        assert!(n.view(100 + DEFAULT_TOAST_MS).toasts.is_empty());
        assert_eq!(n.next_deadline(), None);
    }

    #[test]
    fn one_application_cannot_crowd_out_the_others() {
        let mut n = Notifications::new();
        n.notify(incoming("Other", "keep"), 0);
        for i in 0..MAX_PER_APP + 4 {
            n.notify(incoming("Spam", &format!("s{i}")), 1 + i as u64);
        }
        let view = n.view(100);
        assert_eq!(
            view.items.iter().filter(|i| i.app == "Spam").count(),
            MAX_PER_APP
        );
        assert!(view.items.iter().any(|i| i.app == "Other"));
    }

    #[test]
    fn a_flood_is_dropped_and_recovers_after_the_window() {
        let mut n = Notifications::new();
        for i in 0..(FLOOD_LIMIT + 30) {
            n.notify(incoming("Loud", &format!("{i}")), 0);
        }
        assert!(n.view(0).items.len() <= MAX_PER_APP);
        let before = n.view(0).items[0].summary.clone();
        n.notify(incoming("Loud", "later"), FLOOD_WINDOW_MS + 1);
        assert_ne!(n.view(FLOOD_WINDOW_MS + 1).items[0].summary, before);
        assert_eq!(n.view(FLOOD_WINDOW_MS + 1).items[0].summary, "later");
    }

    #[test]
    fn the_history_is_bounded_and_the_oldest_leave_first() {
        let mut n = Notifications::new();
        let mut closed = 0;
        for i in 0..(MAX_ITEMS + 10) {
            let (_, events) = n.notify(incoming(&format!("App{i}"), "x"), i as u64);
            closed += events.len();
        }
        assert_eq!(n.view(0).items.len(), MAX_ITEMS);
        assert_eq!(closed, 10);
    }

    #[test]
    fn dismiss_clear_and_close_by_app_report_the_right_reasons() {
        let mut n = Notifications::new();
        let (a, _) = n.notify(incoming("A", "1"), 0);
        let (b, _) = n.notify(incoming("B", "2"), 0);
        let (c, _) = n.notify(incoming("C", "3"), 0);
        assert_eq!(
            n.dismiss(a).unwrap(),
            vec![Event::Closed {
                id: a,
                reason: CLOSED_DISMISSED
            }]
        );
        assert_eq!(n.dismiss(a), Err(RequestError::Unknown));
        assert_eq!(
            n.close_by_app(b),
            vec![Event::Closed {
                id: b,
                reason: CLOSED_BY_APP
            }]
        );
        assert!(n.close_by_app(b).is_empty());
        assert_eq!(
            n.clear(),
            vec![Event::Closed {
                id: c,
                reason: CLOSED_DISMISSED
            }]
        );
        assert!(n.view(0).items.is_empty());
    }

    #[test]
    fn reading_the_shade_clears_the_unread_count() {
        let mut n = Notifications::new();
        n.notify(incoming("A", "x"), 0);
        assert_eq!(n.view(0).unread, 1);
        n.mark_all_read();
        assert_eq!(n.view(0).unread, 0);
        n.notify(incoming("A", "y"), 1);
        assert_eq!(n.view(1).unread, 1);
    }

    #[test]
    fn ids_never_collide_and_never_are_zero() {
        let mut n = Notifications::new();
        n.next_id = u32::MAX;
        let (a, _) = n.notify(incoming("A", "x"), 0);
        let (b, _) = n.notify(incoming("B", "y"), 0);
        assert_eq!(a, u32::MAX);
        assert_eq!(b, 1);
    }

    #[test]
    fn an_empty_action_key_is_dropped_even_when_there_is_room() {
        let mut n = Notifications::new();
        let mut with = incoming("A", "x");
        with.actions = ["", "Nothing", "k", "Real"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        n.notify(with, 0);
        let labels: Vec<_> = n.view(0).items[0]
            .actions
            .iter()
            .map(|a| a.label.clone())
            .collect();
        assert_eq!(labels, ["Real"]);
    }

    #[test]
    fn a_flood_stops_being_stored_after_the_limit() {
        let mut n = Notifications::new();
        let mut evicted = 0;
        for i in 0..(FLOOD_LIMIT + 30) {
            evicted += n.notify(incoming("Loud", &format!("{i}")), 0).1.len();
        }
        // Only FLOOD_LIMIT were stored; the per-application cap evicted all but MAX_PER_APP of them.
        assert_eq!(evicted, FLOOD_LIMIT - MAX_PER_APP);
        // Replacing an existing one is not a new notification, so it is never counted as flooding.
        let id = n.view(0).items[0].id;
        let mut again = incoming("Loud", "updated");
        again.replaces = id;
        n.notify(again, 0);
        assert_eq!(
            n.view(0)
                .items
                .iter()
                .filter(|i| i.summary == "updated")
                .count(),
            1
        );
    }

    #[test]
    fn button_labels_are_bounded_and_sanitized() {
        let mut n = Notifications::new();
        let mut with = incoming("A", "x");
        with.actions = vec!["k".into(), format!("{}\u{202e}", "L".repeat(200))];
        n.notify(with, 0);
        let label = n.view(0).items[0].actions[0].label.clone();
        assert!(label.chars().count() <= 41 && !label.contains('\u{202e}'));
    }

    #[test]
    fn a_default_constructed_store_still_hands_out_valid_ids() {
        let mut n = Notifications::default();
        assert_eq!(n.notify(incoming("A", "x"), 0).0, 1);
    }

    #[test]
    fn a_long_timeout_does_not_keep_the_toast_on_screen() {
        let mut n = Notifications::new();
        let mut long = incoming("A", "x");
        long.timeout_ms = 600_000;
        n.notify(long, 0);
        assert!(n.view(MAX_TOAST_MS + 1).toasts.is_empty());
        assert_eq!(n.view(MAX_TOAST_MS + 1).items.len(), 1);
        assert!(
            n.view(MAX_TOAST_MS + 1).items[0].live,
            "it is still live in the shade"
        );
    }

    #[test]
    fn a_tiny_timeout_is_honoured_not_replaced_by_the_default() {
        let mut n = Notifications::new();
        let mut quick = incoming("A", "x");
        quick.timeout_ms = 1;
        let (id, _) = n.notify(quick, 0);
        assert_eq!(
            n.expire(2),
            vec![Event::Closed {
                id,
                reason: CLOSED_EXPIRED
            }]
        );
    }

    fn from(owner: &str, app: &str, summary: &str) -> Incoming {
        Incoming {
            owner: owner.into(),
            ..incoming(app, summary)
        }
    }

    #[test]
    fn only_the_sender_can_replace_its_own_notification() {
        let mut n = Notifications::new();
        let (id, _) = n.notify(from(":1.10", "Mail", "mine"), 0);
        let mut hijack = from(":1.99", "Mail", "forged");
        hijack.replaces = id;
        let (other, _) = n.notify(hijack, 1);
        assert_ne!(
            other, id,
            "someone else's replace request makes a new notification"
        );
        let view = n.view(1);
        assert_eq!(view.items.len(), 2);
        assert!(view.items.iter().any(|i| i.id == id && i.summary == "mine"));
        let mut honest = from(":1.10", "Mail", "updated");
        honest.replaces = id;
        assert_eq!(n.notify(honest, 2).0, id);
    }

    #[test]
    fn a_click_is_only_honoured_for_the_revision_that_was_shown() {
        let mut n = Notifications::new();
        let mut first = from(":1.1", "A", "x");
        first.actions = vec!["safe".into(), "Open".into()];
        let (id, _) = n.notify(first, 0);
        let shown = n.view(0).items[0].revision;
        let mut changed = from(":1.1", "A", "x");
        changed.replaces = id;
        changed.actions = vec!["danger".into(), "Open".into()];
        n.notify(changed, 1);
        assert_eq!(
            n.invoke(id, shown, 0),
            Err(RequestError::Stale),
            "index 0 now means something else"
        );
        let now = n.view(1).items[0].revision;
        assert_eq!(now, shown + 1);
        let events = n.invoke(id, now, 0).unwrap();
        assert_eq!(
            events[0],
            Event::ActionInvoked {
                id,
                key: "danger".into()
            }
        );
    }

    #[test]
    fn renaming_does_not_buy_a_sender_a_new_allowance() {
        let mut n = Notifications::new();
        let mut stored = 0;
        for i in 0..(FLOOD_LIMIT + 30) {
            let before = n.view(0).items.len();
            n.notify(from(":1.5", &format!("Name{i}"), "spam"), 0);
            stored += usize::from(n.view(0).items.len() > before);
        }
        assert!(
            n.view(0).items.len() <= MAX_PER_APP,
            "one connection stays under the per-sender cap"
        );
        let _ = stored;
        // A different connection is unaffected.
        let before = n.view(0).items.len();
        n.notify(from(":1.6", "Calm", "hello"), 0);
        assert_eq!(n.view(0).items.len(), before + 1);
    }

    #[test]
    fn the_flood_memory_cannot_grow_without_bound() {
        let mut n = Notifications::new();
        for i in 0..5_000 {
            n.notify(from(&format!(":1.{}", i % 7), "x", "y"), 0);
        }
        assert!(n.recent.len() <= MAX_RECENT, "{}", n.recent.len());
    }

    #[test]
    fn hostile_markup_costs_bounded_work_and_stays_bounded() {
        let mut n = Notifications::new();
        let mut hostile = incoming("A", &"<".repeat(1_000_000));
        hostile.body = "<".repeat(5_000_000);
        let start = std::time::Instant::now();
        n.notify(hostile, 0);
        assert!(
            start.elapsed() < std::time::Duration::from_millis(500),
            "{:?}",
            start.elapsed()
        );
        let view = n.view(0);
        assert!(
            view.items[0].body.chars().count() <= 401
                && view.items[0].summary.chars().count() <= 121
        );
    }

    #[test]
    fn a_tag_must_be_short_to_be_stripped() {
        assert_eq!(plain_text("<b>bold</b>"), "bold");
        let long = format!("<a {}>text", "x".repeat(100));
        assert!(
            plain_text(&long).starts_with("<a xxx"),
            "an over-long tag is just text"
        );
    }
}
