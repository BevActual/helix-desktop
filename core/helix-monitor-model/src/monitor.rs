//! Pure System Monitor models: parse `/proc`-shaped text and screen-time report lines, and present
//! Overview / Processes / Screen Time views. The host reads files and schedules polls; this module
//! never opens a path.

use serde::Serialize;

use crate::display::sanitize_for_display;

const MAX_NAME: usize = 64;
const MAX_SEARCH: usize = 80;

#[derive(Clone, Debug, Default, PartialEq, Serialize)]
pub struct CpuSnap {
    pub total: u64,
    pub idle: u64,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize)]
pub struct MemorySnap {
    pub total: u64,
    pub available: u64,
    pub swap_total: u64,
    pub swap_free: u64,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize)]
pub struct NetSnap {
    pub rx: u64,
    pub tx: u64,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize)]
pub struct Overview {
    pub cpu_percent: f64,
    pub mem_percent: f64,
    pub mem_used: u64,
    pub mem_total: u64,
    pub swap_percent: f64,
    pub net_rx: u64,
    pub net_tx: u64,
    pub load_1: f64,
    pub load_5: f64,
    pub load_15: f64,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ProcessRow {
    pub pid: i32,
    pub name: String,
    pub state: char,
    pub threads: u64,
    pub rss: u64,
    pub cpu_ticks: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProcessSort {
    Cpu,
    Memory,
    Name,
    Pid,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ScreenTimeRow {
    pub app: String,
    pub seconds: u64,
}

#[derive(Clone, Debug, Default)]
pub struct Monitor {
    prev_cpu: Option<CpuSnap>,
    prev_net: Option<NetSnap>,
    overview: Overview,
    processes: Vec<ProcessRow>,
    screen_time: Vec<ScreenTimeRow>,
}

impl Monitor {
    pub fn overview(&self) -> Overview {
        self.overview.clone()
    }

    pub fn processes(&self, query: &str, sort: ProcessSort) -> Vec<ProcessRow> {
        let q = sanitize_for_display(query, MAX_SEARCH).to_ascii_lowercase();
        let mut rows: Vec<_> = self
            .processes
            .iter()
            .filter(|p| q.is_empty() || p.name.to_ascii_lowercase().contains(&q) || p.pid.to_string().contains(&q))
            .cloned()
            .collect();
        match sort {
            ProcessSort::Cpu => rows.sort_by_key(|a| std::cmp::Reverse(a.cpu_ticks)),
            ProcessSort::Memory => rows.sort_by_key(|a| std::cmp::Reverse(a.rss)),
            ProcessSort::Name => rows.sort_by_key(|a| a.name.to_ascii_lowercase()),
            ProcessSort::Pid => rows.sort_by_key(|p| p.pid),
        }
        rows
    }

    pub fn screen_time(&self) -> Vec<ScreenTimeRow> {
        self.screen_time.clone()
    }

    pub fn set_cpu(&mut self, snap: CpuSnap) {
        if let Some(prev) = &self.prev_cpu {
            let dt = snap.total.saturating_sub(prev.total);
            let di = snap.idle.saturating_sub(prev.idle);
            if dt > 0 {
                self.overview.cpu_percent = ((dt - di) as f64 / dt as f64 * 100.0).clamp(0.0, 100.0);
            }
        }
        self.prev_cpu = Some(snap);
    }

    pub fn set_memory(&mut self, snap: MemorySnap) {
        self.overview.mem_total = snap.total;
        self.overview.mem_used = snap.total.saturating_sub(snap.available.min(snap.total));
        self.overview.mem_percent = if snap.total > 0 {
            self.overview.mem_used as f64 / snap.total as f64 * 100.0
        } else {
            0.0
        };
        self.overview.swap_percent = if snap.swap_total > 0 {
            snap.swap_total.saturating_sub(snap.swap_free) as f64 / snap.swap_total as f64 * 100.0
        } else {
            0.0
        };
    }

    pub fn set_net(&mut self, snap: NetSnap) {
        if let Some(prev) = &self.prev_net {
            self.overview.net_rx = snap.rx.saturating_sub(prev.rx);
            self.overview.net_tx = snap.tx.saturating_sub(prev.tx);
        }
        self.prev_net = Some(snap);
    }

    pub fn set_load(&mut self, one: f64, five: f64, fifteen: f64) {
        self.overview.load_1 = one;
        self.overview.load_5 = five;
        self.overview.load_15 = fifteen;
    }

    pub fn set_processes(&mut self, rows: Vec<ProcessRow>) {
        self.processes = rows
            .into_iter()
            .filter(|p| p.pid > 0)
            .map(|p| ProcessRow {
                name: sanitize_for_display(&p.name, MAX_NAME),
                ..p
            })
            .collect();
    }

    pub fn set_screen_time(&mut self, rows: Vec<ScreenTimeRow>) {
        self.screen_time = rows
            .into_iter()
            .filter(|r| r.seconds > 0 && !r.app.is_empty())
            .map(|r| ScreenTimeRow {
                app: sanitize_for_display(&r.app, MAX_NAME),
                seconds: r.seconds,
            })
            .collect();
    }
}

/// Parse the first `cpu` line of `/proc/stat`.
pub fn parse_cpu(text: &str) -> Option<CpuSnap> {
    let line = text.lines().next()?;
    let mut parts = line.split_whitespace();
    if parts.next()? != "cpu" {
        return None;
    }
    let mut values = Vec::new();
    for token in parts {
        values.push(token.parse::<u64>().ok()?);
    }
    if values.len() < 4 {
        return None;
    }
    let total: u64 = values.iter().take(8).sum();
    if total == 0 {
        return None;
    }
    let idle = values[3].saturating_add(values.get(4).copied().unwrap_or(0));
    Some(CpuSnap { total, idle })
}

/// Parse `/proc/meminfo` (kB lines).
pub fn parse_memory(text: &str) -> Option<MemorySnap> {
    let mut total = 0u64;
    let mut available = 0u64;
    let mut swap_total = 0u64;
    let mut swap_free = 0u64;
    for line in text.lines() {
        let mut parts = line.split_whitespace();
        let key = parts.next()?;
        let value: u64 = parts.next()?.parse().ok()?;
        let unit = parts.next().unwrap_or("");
        if unit != "kB" {
            continue;
        }
        let bytes = value.checked_mul(1024)?;
        match key {
            "MemTotal:" => total = bytes,
            "MemAvailable:" => available = bytes,
            "SwapTotal:" => swap_total = bytes,
            "SwapFree:" => swap_free = bytes,
            _ => {}
        }
    }
    if total == 0 {
        return None;
    }
    Some(MemorySnap {
        total,
        available: available.min(total),
        swap_total,
        swap_free: swap_free.min(swap_total),
    })
}

/// Aggregate `/proc/net/dev` excluding loopback.
pub fn parse_net(text: &str) -> Option<NetSnap> {
    let mut rx = 0u64;
    let mut tx = 0u64;
    for line in text.lines().skip(2) {
        let mut parts = line.split_whitespace();
        let name = parts.next()?.trim_end_matches(':');
        if name == "lo" {
            continue;
        }
        let r: u64 = parts.next()?.parse().ok()?;
        for _ in 0..7 {
            parts.next()?;
        }
        let t: u64 = parts.next()?.parse().ok()?;
        rx = rx.saturating_add(r);
        tx = tx.saturating_add(t);
    }
    Some(NetSnap { rx, tx })
}

/// Parse one `/proc/<pid>/stat` line.
pub fn parse_process(text: &str, page_size: u64) -> Option<ProcessRow> {
    if page_size == 0 {
        return None;
    }
    let start = text.find('(')?;
    let end = text.rfind(')')?;
    if end <= start {
        return None;
    }
    let pid: i32 = text[..start].trim().parse().ok()?;
    if pid <= 0 {
        return None;
    }
    let name = text[start + 1..end].to_string();
    let rest = &text[end + 1..];
    let tokens: Vec<&str> = rest.split_whitespace().collect();
    // After `)`: state (1), … utime(14), stime(15), … num_threads(20), … rss(24) — 1-based field numbers from man proc.
    if tokens.len() < 22 || tokens[0].len() != 1 {
        return None;
    }
    let state = tokens[0].chars().next()?;
    let utime: u64 = tokens[11].parse().ok()?;
    let stime: u64 = tokens[12].parse().ok()?;
    let threads: u64 = tokens[17].parse().ok()?;
    let rss_pages: u64 = tokens[21].parse().ok()?;
    Some(ProcessRow {
        pid,
        name: sanitize_for_display(&name, MAX_NAME),
        state,
        threads,
        rss: rss_pages.saturating_mul(page_size),
        cpu_ticks: utime.saturating_add(stime),
    })
}

/// Screen-time report lines: `app_id seconds` (malformed lines skipped).
pub fn parse_screen_time(text: &str) -> Vec<ScreenTimeRow> {
    let mut rows = Vec::new();
    for line in text.lines() {
        let mut parts = line.split_whitespace();
        let Some(app) = parts.next() else { continue };
        let Some(seconds) = parts.next().and_then(|s| s.parse().ok()) else { continue };
        if seconds == 0 || app.is_empty() || parts.next().is_some() {
            continue;
        }
        rows.push(ScreenTimeRow {
            app: sanitize_for_display(app, MAX_NAME),
            seconds,
        });
    }
    rows.sort_by_key(|a| std::cmp::Reverse(a.seconds));
    rows
}

/// Process actions are out of scope for v1; refuse any unknown key so a later wire-up stays strict.
pub fn refuse_process_action(key: &str) -> bool {
    !matches!(key, "list" | "search" | "sort")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpu_memory_and_net_parse() {
        let cpu = parse_cpu("cpu  10 20 30 40 5 0 0 0\ncpu0 1 2 3 4\n").unwrap();
        assert_eq!(cpu.total, 105);
        assert_eq!(cpu.idle, 45);
        let mem = parse_memory("MemTotal:       1000 kB\nMemAvailable:    400 kB\nSwapTotal: 200 kB\nSwapFree: 50 kB\n").unwrap();
        assert_eq!(mem.total, 1000 * 1024);
        assert_eq!(mem.available, 400 * 1024);
        let net = parse_net("Inter-|   Receive\n face |bytes\n  eth0: 100 0 0 0 0 0 0 0 50 0 0 0 0 0 0 0\n    lo: 999 0 0 0 0 0 0 0 999 0 0 0 0 0 0 0\n").unwrap();
        assert_eq!(net.rx, 100);
        assert_eq!(net.tx, 50);
    }

    #[test]
    fn process_and_screen_time() {
        // pid (name) state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime cutime cstime priority nice num_threads itrealvalue starttime vsize rss
        let line = "42 (bash) S 1 1 1 0 -1 0 0 0 0 0 10 5 0 0 20 0 1 0 100 0 200\n";
        let p = parse_process(line, 4096).unwrap();
        assert_eq!(p.pid, 42);
        assert_eq!(p.name, "bash");
        assert_eq!(p.cpu_ticks, 15);
        assert_eq!(p.rss, 200 * 4096);
        let rows = parse_screen_time("org.helix.Settings 120\nbad\norg.helix.Mobius 30 extra\norg.helix.Mobius 60\n");
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].app, "org.helix.Settings");
        assert_eq!(rows[0].seconds, 120);
    }

    #[test]
    fn overview_deltas_and_process_filter() {
        let mut m = Monitor::default();
        m.set_cpu(CpuSnap { total: 100, idle: 50 });
        m.set_cpu(CpuSnap { total: 200, idle: 80 });
        assert!((m.overview().cpu_percent - 70.0).abs() < 0.01);
        m.set_memory(MemorySnap {
            total: 1000,
            available: 250,
            swap_total: 100,
            swap_free: 40,
        });
        assert!((m.overview().mem_percent - 75.0).abs() < 0.01);
        m.set_processes(vec![
            ProcessRow { pid: 1, name: "init".into(), state: 'S', threads: 1, rss: 10, cpu_ticks: 1 },
            ProcessRow { pid: 2, name: "helix-shell".into(), state: 'R', threads: 4, rss: 90, cpu_ticks: 50 },
        ]);
        let rows = m.processes("helix", ProcessSort::Memory);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].pid, 2);
    }

    #[test]
    fn unknown_process_actions_are_refused() {
        assert!(refuse_process_action("kill"));
        assert!(refuse_process_action("renice"));
        assert!(!refuse_process_action("list"));
    }
}
