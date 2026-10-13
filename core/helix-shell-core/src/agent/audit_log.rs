//! Append-only agent audit log (`audit.jsonl`, mode 0600). Port of C++ `helix::agent::AuditLog`.

use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};

use super::risk::Tier;

pub const MAX_ARG_BYTES: usize = 120;

#[derive(Clone, Debug)]
pub struct AuditRecord {
    pub client: String,
    pub tool: String,
    pub args: Vec<String>,
    pub tier: Tier,
    pub decision: String,
    pub outcome: String,
    pub tainted: bool,
}

#[derive(Clone, Debug)]
pub struct AuditLog {
    file: PathBuf,
}

impl AuditLog {
    pub fn new(file: impl Into<PathBuf>) -> Self {
        Self { file: file.into() }
    }

    pub fn path(&self) -> &Path {
        &self.file
    }

    pub fn append(&self, record: &AuditRecord) -> Result<(), String> {
        let when = SystemTime::now();
        let args: Vec<Value> = record
            .args
            .iter()
            .map(|arg| Value::String(truncate(arg, MAX_ARG_BYTES)))
            .collect();
        let line = json!({
            "time": iso_utc(when),
            "client": record.client,
            "tool": record.tool,
            "args": args,
            "tier": record.tier.name(),
            "decision": record.decision,
            "outcome": record.outcome,
            "tainted": record.tainted,
        });
        let text = format!(
            "{}\n",
            serde_json::to_string(&line).map_err(|error| format!("audit: {error}"))?
        );
        if let Some(parent) = self.file.parent() {
            fs::create_dir_all(parent).map_err(|error| format!("audit: {error}"))?;
        }
        let mut options = OpenOptions::new();
        options.create(true).append(true).write(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file = options
            .open(&self.file)
            .map_err(|error| format!("audit: {error}"))?;
        file.write_all(text.as_bytes())
            .map_err(|error| format!("audit: {error}"))?;
        Ok(())
    }

    /// Chronological tail of the log (same window as C++ `readAudit`), capped at `limit` (max 500).
    pub fn read_recent(&self, since_unix: u64, limit: u32) -> Vec<Value> {
        let limit = limit.min(500) as usize;
        let file = match File::open(&self.file) {
            Ok(file) => file,
            Err(_) => return Vec::new(),
        };
        let mut records = Vec::new();
        for line in BufReader::new(file).lines().flatten() {
            if line.trim().is_empty() {
                continue;
            }
            let Ok(value) = serde_json::from_str::<Value>(&line) else {
                continue;
            };
            if since_unix > 0 {
                let ts = parse_time(value.get("time"));
                if ts < since_unix {
                    continue;
                }
            }
            records.push(value);
        }
        if records.len() <= limit {
            return records;
        }
        records.split_off(records.len() - limit)
    }
}

fn truncate(text: &str, max: usize) -> String {
    if text.len() <= max {
        return text.to_string();
    }
    format!("{}…", text.chars().take(max.saturating_sub(1)).collect::<String>())
}

fn parse_time(value: Option<&Value>) -> u64 {
    match value {
        Some(Value::String(text)) => parse_iso_utc(text).unwrap_or(0),
        Some(Value::Number(number)) => number.as_u64().unwrap_or(0),
        _ => 0,
    }
}

fn iso_utc(when: SystemTime) -> String {
    let secs = when
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let days = (secs / 86_400) as i32;
    let rem = secs % 86_400;
    let hour = rem / 3600;
    let minute = (rem % 3600) / 60;
    let second = rem % 60;
    let (year, month, day) = civil_from_days(days);
    format!(
        "{year:04}-{month:02}-{day:02}T{hour:02}:{minute:02}:{second:02}Z"
    )
}

fn parse_iso_utc(text: &str) -> Option<u64> {
    if text.len() != 20 || !text.ends_with('Z') {
        return None;
    }
    let year = text[0..4].parse::<i32>().ok()?;
    let month = text[5..7].parse::<u32>().ok()?;
    let day = text[8..10].parse::<u32>().ok()?;
    let hour = text[11..13].parse::<u64>().ok()?;
    let minute = text[14..16].parse::<u64>().ok()?;
    let second = text[17..19].parse::<u64>().ok()?;
    let days = days_from_civil(year, month, day)?;
    Some(days as u64 * 86_400 + hour * 3600 + minute * 60 + second)
}

// Howard Hinnant's algorithms (public domain).
fn civil_from_days(days: i32) -> (i32, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = mp + if mp < 10 { 3 } else { -9 };
    let year = y + if m <= 2 { 1 } else { 0 };
    (year, m as u32, d as u32)
}

fn days_from_civil(year: i32, month: u32, day: u32) -> Option<i32> {
    if month < 1 || month > 12 || day < 1 || day > 31 {
        return None;
    }
    let y = year - if month <= 2 { 1 } else { 0 };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let month = month as i32;
    let doy = (153 * if month > 2 { month - 3 } else { month + 9 } + 2) / 5 + day as i32 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    Some(era * 146_097 + doe - 719_468)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_iso_utc() {
        let when = UNIX_EPOCH + std::time::Duration::from_secs(1_704_067_200);
        let text = iso_utc(when);
        assert_eq!(text, "2024-01-01T00:00:00Z");
        assert_eq!(parse_iso_utc(&text), Some(1_704_067_200));
    }
}
