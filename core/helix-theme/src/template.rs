//! Template rendering (`theme/template_engine.cpp`, the `EngineImpl` render path).
//!
//! Covers `{{ ... }}` expressions, `<* for/if/else/endif/endfor *>` blocks, the `colors.*`
//! accessors and every filter except the Material tonal-palette ones: `palettes.*` expressions
//! set [`RenderResult::unsupported`] so callers can fall back to the C++ renderer.
//!
//! Known deliberate difference: map iteration (`for k, v in <config table>`) is key-sorted here;
//! the C++ `unordered_map` order is unspecified.

use std::collections::{BTreeMap, HashMap};

use regex::Regex;
use serde_json::Value;

use crate::color::Color;

/// mode name (`dark`/`light`/custom) → token name → `#rrggbb`.
pub type ThemeData = HashMap<String, HashMap<String, String>>;

#[derive(Clone, Debug)]
pub struct Options {
    pub default_mode: String,
    pub image_path: String,
    pub closest_color: String,
    pub config_dir: String,
    pub config_file: String,
    /// `[config]`-style table exposed as `config.*` (integers become numbers).
    pub config_table: Option<Value>,
    /// Value of `icon_theme` (only exposed together with `config_table`).
    pub icon_theme: String,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            default_mode: "dark".into(),
            image_path: String::new(),
            closest_color: String::new(),
            config_dir: String::new(),
            config_file: String::new(),
            config_table: None,
            icon_theme: String::new(),
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RenderResult {
    pub text: String,
    pub error_count: u32,
    /// Set when the template used a feature this renderer does not implement; `text` is
    /// then not trustworthy and the caller should use the C++ renderer.
    pub unsupported: Option<&'static str>,
}

/// C++ `makeThemeData`: every token as `#rrggbb` for dark and light.
pub fn make_theme_data(dark: &HashMap<String, u32>, light: &HashMap<String, u32>) -> ThemeData {
    let conv = |m: &HashMap<String, u32>| -> HashMap<String, String> {
        m.iter().map(|(k, v)| (k.clone(), Color::from_argb(*v).to_hex())).collect()
    };
    let mut data = ThemeData::new();
    data.insert("dark".into(), conv(dark));
    data.insert("light".into(), conv(light));
    data
}

// ---- values -------------------------------------------------------------------------

#[derive(Clone, Copy, Debug)]
struct RichColor {
    color: Color,
    alpha: f64,
}

#[derive(Clone, Debug, Default)]
enum Scope {
    #[default]
    None,
    Bool(bool),
    Num(f64),
    Str(String),
    Color(RichColor),
    Array(Vec<Scope>),
    Map(BTreeMap<String, Scope>),
}

fn from_json(value: &Value) -> Scope {
    match value {
        Value::Object(map) => Scope::Map(map.iter().map(|(k, v)| (k.clone(), from_json(v))).collect()),
        Value::Array(items) => Scope::Array(items.iter().map(from_json).collect()),
        Value::String(s) => Scope::Str(s.clone()),
        Value::Number(n) => Scope::Num(n.as_f64().unwrap_or(0.0)),
        Value::Bool(b) => Scope::Bool(*b),
        Value::Null => Scope::None,
    }
}

fn is_c_space(c: char) -> bool {
    matches!(c, ' ' | '\t' | '\n' | '\r' | '\x0b' | '\x0c')
}

fn trim(s: &str) -> &str {
    s.trim_matches(is_c_space)
}

fn is_truthy(value: &Scope) -> bool {
    match value {
        Scope::None => false,
        Scope::Bool(b) => *b,
        Scope::Num(n) => *n != 0.0,
        Scope::Str(s) => {
            let lowered = trim(s).to_ascii_lowercase();
            !(lowered.is_empty() || lowered == "false" || lowered == "0" || lowered == "none")
        }
        Scope::Array(a) => !a.is_empty(),
        Scope::Map(m) => !m.is_empty(),
        Scope::Color(_) => true,
    }
}

/// `std::to_chars(double)`: shortest round-trip, scientific when it is shorter.
pub fn format_dot_decimal(value: f64) -> String {
    if value == 0.0 {
        return if value.is_sign_negative() { "-0".into() } else { "0".into() };
    }
    let fixed = format!("{value}");
    let sci = format!("{value:e}");
    // Rust: "1e-5" / "1.5e21"; C: "1e-05" / "1.5e+21".
    let sci = match sci.split_once('e') {
        Some((mantissa, exp)) => {
            let (sign, digits) = match exp.strip_prefix('-') {
                Some(d) => ('-', d),
                None => ('+', exp),
            };
            format!("{mantissa}e{sign}{digits:0>2}")
        }
        None => sci,
    };
    if sci.len() < fixed.len() {
        sci
    } else {
        fixed
    }
}

fn scope_to_string(value: &Scope) -> String {
    match value {
        Scope::Str(s) => s.clone(),
        Scope::Bool(b) => if *b { "true" } else { "false" }.to_string(),
        Scope::Num(n) => {
            let rounded = n.round();
            if (n - rounded).abs() < 1.0e-9 {
                format!("{}", rounded as i64)
            } else {
                format_dot_decimal(*n)
            }
        }
        Scope::Color(c) => c.color.to_hex(),
        _ => String::new(),
    }
}

fn parse_number(arg: &Option<String>) -> Result<f64, ()> {
    let Some(arg) = arg else { return Ok(0.0) };
    let t = trim(arg);
    if t.is_empty() || t.starts_with('+') {
        return Err(());
    }
    match t.parse::<f64>() {
        Ok(v) if v.is_finite() => Ok(v),
        _ => Err(()),
    }
}

fn format_color(c: &RichColor, format_type: &str) -> String {
    let (r, g, b) = (c.color.r, c.color.g, c.color.b);
    match format_type {
        "hex" => c.color.to_hex(),
        "hex_stripped" => c.color.to_hex().trim_start_matches('#').to_string(),
        "rgb" => format!("rgb({r}, {g}, {b})"),
        "rgb_csv" => format!("{r},{g},{b}"),
        "rgba" => format!("rgba({r}, {g}, {b}, {})", format_dot_decimal(c.alpha)),
        "red" => r.to_string(),
        "green" => g.to_string(),
        "blue" => b.to_string(),
        "alpha" => format_dot_decimal(c.alpha),
        other => {
            let (h, s, l) = c.color.to_hsl();
            let (h, s, l) = (h as i32, (s * 100.0) as i32, (l * 100.0) as i32);
            match other {
                "hsl" => format!("hsl({h}, {s}%, {l}%)"),
                "hsla" => format!("hsla({h}, {s}%, {l}%, {})", format_dot_decimal(c.alpha)),
                "hue" => h.to_string(),
                "saturation" => s.to_string(),
                "lightness" => l.to_string(),
                _ => c.color.to_hex(),
            }
        }
    }
}

const KNOWN_FORMATS: [&str; 14] = [
    "hex", "hex_stripped", "rgb", "rgb_csv", "rgba", "hsl", "hsla", "red", "green", "blue", "alpha", "hue",
    "saturation", "lightness",
];
const COLOR_FILTERS: [&str; 15] = [
    "grayscale", "invert", "set_alpha", "set_lightness", "set_hue", "set_saturation", "set_red", "set_green",
    "set_blue", "lighten", "darken", "saturate", "desaturate", "auto_lightness", "rotate_hue",
];
const COLOR_ARG_FILTERS: [&str; 2] = ["blend", "harmonize"];

// ---- text helpers -------------------------------------------------------------------

fn split_pipes(expr: &str) -> Vec<String> {
    let mut parts = Vec::new();
    let mut current = String::new();
    let mut in_quotes = false;
    let mut quote = '\0';
    for ch in expr.chars() {
        if (ch == '"' || ch == '\'') && !in_quotes {
            in_quotes = true;
            quote = ch;
            current.push(ch);
        } else if ch == quote && in_quotes {
            in_quotes = false;
            quote = '\0';
            current.push(ch);
        } else if ch == '|' && !in_quotes {
            parts.push(std::mem::take(&mut current));
        } else {
            current.push(ch);
        }
    }
    if !current.is_empty() {
        parts.push(current);
    }
    parts
}

struct Regexes {
    block: Regex,
    expr: Regex,
    filter_colon: Regex,
    camel_split: Regex,
    replace_dq: Regex,
    replace_sq: Regex,
    color_arg: Regex,
    for_cmd: Regex,
    range: Regex,
    color_expr: Regex,
}

fn re() -> &'static Regexes {
    static RE: std::sync::OnceLock<Regexes> = std::sync::OnceLock::new();
    RE.get_or_init(|| Regexes {
        block: Regex::new(r"(?s)<\*(.*?)\*>").unwrap(),
        expr: Regex::new(r"\{\{([^}\n]+?)\}\}").unwrap(),
        filter_colon: Regex::new(r"^([a-z_]+)[ \t\n\r\x0b\x0c]*:[ \t\n\r\x0b\x0c]*(.+)$").unwrap(),
        camel_split: Regex::new(r"([a-z])([A-Z])").unwrap(),
        replace_dq: Regex::new(r#"^"([^"]*?)"[ \t\n\r\x0b\x0c]*,[ \t\n\r\x0b\x0c]*"([^"]*?)"$"#).unwrap(),
        replace_sq: Regex::new(r"^'([^']*?)'[ \t\n\r\x0b\x0c]*,[ \t\n\r\x0b\x0c]*'([^']*?)'$").unwrap(),
        color_arg: Regex::new(r#"["']?(#[0-9a-fA-F]{6})["']?[ \t\n\r\x0b\x0c]*(?:,[ \t\n\r\x0b\x0c]*(.+))?"#).unwrap(),
        for_cmd: Regex::new(r"^for[ \t\n\r\x0b\x0c]+(.+?)[ \t\n\r\x0b\x0c]+in[ \t\n\r\x0b\x0c]+(.+)$").unwrap(),
        range: Regex::new(r"^(-?\d+)\.\.(-?\d+)$").unwrap(),
        color_expr: Regex::new(r"^colors\.([a-z_0-9]+)\.([a-z_0-9]+)\.([a-z_0-9]+)$").unwrap(),
    })
}

fn parse_filter(filter: &str) -> (String, Option<String>) {
    let s = trim(filter);
    if let Some(m) = re().filter_colon.captures(s) {
        return (m[1].to_string(), Some(trim(&m[2]).to_string()));
    }
    match s.find([' ', '\t']) {
        None => (s.to_string(), None),
        Some(i) => (trim(&s[..i]).to_string(), Some(trim(&s[i + 1..]).to_string())),
    }
}

fn split_words(s: &str) -> Vec<String> {
    let spaced = re().camel_split.replace_all(s, "${1}_${2}");
    let mut out = Vec::new();
    let mut current = String::new();
    for ch in spaced.chars() {
        if ch.is_ascii_alphanumeric() {
            current.push(ch);
        } else if !current.is_empty() {
            out.push(std::mem::take(&mut current));
        }
    }
    if !current.is_empty() {
        out.push(current);
    }
    out
}

fn capitalize(word: &str) -> String {
    let mut chars = word.chars();
    match chars.next() {
        Some(f) => f.to_ascii_uppercase().to_string() + chars.as_str(),
        None => String::new(),
    }
}

fn to_camel_case(value: &str) -> String {
    let words = split_words(value);
    let Some(first) = words.first() else { return value.to_string() };
    let mut out = first.to_ascii_lowercase();
    for w in &words[1..] {
        out += &capitalize(&w.to_ascii_lowercase());
    }
    out
}

fn to_pascal_case(value: &str) -> String {
    let words = split_words(value);
    if words.is_empty() {
        return value.to_string();
    }
    words.iter().map(|w| capitalize(&w.to_ascii_lowercase())).collect()
}

fn join_lower(value: &str, sep: &str) -> String {
    split_words(value).iter().map(|w| w.to_ascii_lowercase()).collect::<Vec<_>>().join(sep)
}

fn apply_replace(value: &str, arg: &Option<String>) -> String {
    let Some(arg) = arg else { return value.to_string() };
    for re in [&re().replace_dq, &re().replace_sq] {
        if let Some(m) = re.captures(arg) {
            return if m[1].is_empty() { value.to_string() } else { value.replace(&m[1], &m[2]) };
        }
    }
    value.to_string()
}

fn as_rich_color(value: &Scope) -> Result<RichColor, ()> {
    match value {
        Scope::Color(c) => Ok(*c),
        Scope::Str(s) => Color::from_hex(s).map(|color| RichColor { color, alpha: 1.0 }).ok_or(()),
        _ => Err(()),
    }
}

fn apply_color_arg_filter(mut value: RichColor, name: &str, arg: &Option<String>) -> Result<RichColor, ()> {
    let Some(arg) = arg else { return Ok(value) };
    let Some(m) = re().color_arg.captures(arg) else { return Ok(value) };
    let target = Color::from_hex(&m[1]).ok_or(())?;
    let (src_hue, src_sat, src_light) = value.color.to_hsl();
    let (target_hue, _, _) = target.to_hsl();
    let mut diff = target_hue - src_hue;
    if diff > 180.0 {
        diff -= 360.0;
    } else if diff < -180.0 {
        diff += 360.0;
    }
    let mut new_hue = src_hue;
    if name == "blend" {
        let amount_arg = m.get(2).map(|g| g.as_str().to_string());
        let amount = parse_number(&amount_arg)?.clamp(0.0, 1.0);
        new_hue = (src_hue + diff * amount + 360.0) % 360.0;
    } else if name == "harmonize" {
        let mut rotation = (diff.abs() * 0.5).min(15.0);
        if diff < 0.0 {
            rotation = -rotation;
        }
        new_hue = (src_hue + rotation + 360.0) % 360.0;
    }
    value.color = Color::from_hsl(new_hue, src_sat, src_light);
    Ok(value)
}

fn apply_color_filter(mut color: RichColor, name: &str, arg: &Option<String>) -> Result<RichColor, ()> {
    let (h, s, l) = color.color.to_hsl();
    if name == "grayscale" {
        let gray = (0.299 * f64::from(color.color.r) + 0.587 * f64::from(color.color.g)
            + 0.114 * f64::from(color.color.b))
        .round() as i32;
        color.color = Color::new(gray, gray, gray);
        return Ok(color);
    }
    if name == "invert" {
        color.color = Color::new(255 - color.color.r, 255 - color.color.g, 255 - color.color.b);
        return Ok(color);
    }
    let n = parse_number(arg)?;
    let hsl = Color::from_hsl;
    match name {
        "set_alpha" => color.alpha = n.clamp(0.0, 1.0),
        "set_lightness" => color.color = hsl(h, s, (n / 100.0).clamp(0.0, 1.0)),
        "set_hue" => color.color = hsl((n + 360.0) % 360.0, s, l),
        "rotate_hue" => color.color = hsl((h + n + 360.0) % 360.0, s, l),
        "set_saturation" => color.color = hsl(h, (n / 100.0).clamp(0.0, 1.0), l),
        "lighten" => color.color = hsl(h, s, (l + n / 100.0).clamp(0.0, 1.0)),
        "darken" => color.color = hsl(h, s, (l - n / 100.0).clamp(0.0, 1.0)),
        "saturate" => color.color = hsl(h, (s + n / 100.0).clamp(0.0, 1.0), l),
        "desaturate" => color.color = hsl(h, (s - n / 100.0).clamp(0.0, 1.0), l),
        "auto_lightness" => {
            let target = if l < 0.5 { l + n / 100.0 } else { l - n / 100.0 };
            color.color = hsl(h, s, target.clamp(0.0, 1.0));
        }
        "set_red" => color.color.r = (n.round() as i64).clamp(0, 255) as i32,
        "set_green" => color.color.g = (n.round() as i64).clamp(0, 255) as i32,
        "set_blue" => color.color.b = (n.round() as i64).clamp(0, 255) as i32,
        _ => {}
    }
    Ok(color)
}

// ---- parser -------------------------------------------------------------------------

enum Token {
    Text(String),
    Block(String),
}

enum Node {
    Text(String),
    For { variables: Vec<String>, iterable: String, body: Vec<Node> },
    If { condition: String, negated: bool, then_body: Vec<Node>, else_body: Vec<Node> },
}

fn tokenize(input: &str) -> Vec<Token> {
    let mut tokens = Vec::new();
    let mut last_end = 0;
    for m in re().block.captures_iter(input) {
        let whole = m.get(0).unwrap();
        let (start, end) = (whole.start(), whole.end());
        let line_start = input[..start].rfind('\n').map_or(0, |i| i + 1);
        let (mut adj_start, mut adj_end) = (start, end);
        if trim(&input[line_start..start]).is_empty() {
            let bytes = input.as_bytes();
            let mut after = end;
            while after < bytes.len() && (bytes[after] == b' ' || bytes[after] == b'\t') {
                after += 1;
            }
            if after == bytes.len() || bytes[after] == b'\n' {
                adj_start = line_start;
                adj_end = if after == bytes.len() { after } else { after + 1 };
            }
        }
        if adj_start > last_end {
            tokens.push(Token::Text(input[last_end..adj_start].to_string()));
        }
        tokens.push(Token::Block(trim(&m[1]).to_string()));
        last_end = adj_end;
    }
    if last_end < input.len() {
        tokens.push(Token::Text(input[last_end..].to_string()));
    }
    tokens
}

struct Parser<'a> {
    tokens: &'a [Token],
    pos: usize,
    errors: u32,
}

impl Parser<'_> {
    fn nodes(&mut self, stop: &[&str]) -> Vec<Node> {
        let mut nodes = Vec::new();
        while self.pos < self.tokens.len() {
            match &self.tokens[self.pos] {
                Token::Text(t) => {
                    if !t.is_empty() {
                        nodes.push(Node::Text(t.clone()));
                    }
                    self.pos += 1;
                }
                Token::Block(cmd) => {
                    if stop.iter().any(|kw| cmd.starts_with(kw)) {
                        return nodes;
                    }
                    if cmd.starts_with("for ") {
                        let node = self.parse_for();
                        nodes.push(node);
                    } else if cmd.starts_with("if ") {
                        let node = self.parse_if();
                        nodes.push(node);
                    } else {
                        self.pos += 1;
                    }
                }
            }
        }
        nodes
    }

    fn cmd(&self) -> String {
        match &self.tokens[self.pos] {
            Token::Block(c) => c.clone(),
            Token::Text(_) => String::new(),
        }
    }

    fn parse_for(&mut self) -> Node {
        let cmd = self.cmd();
        self.pos += 1;
        let Some(m) = re().for_cmd.captures(&cmd) else {
            self.errors += 1;
            return Node::For { variables: vec![], iterable: String::new(), body: vec![] };
        };
        // `getline(',')` drops a trailing empty field.
        let mut variables: Vec<String> = m[1].split(',').map(|v| trim(v).to_string()).collect();
        if m[1].ends_with(',') {
            variables.pop();
        }
        let iterable = trim(&m[2]).to_string();
        let body = self.nodes(&["endfor"]);
        if self.pos < self.tokens.len() {
            self.pos += 1;
        }
        Node::For { variables, iterable, body }
    }

    fn parse_if(&mut self) -> Node {
        let cmd = self.cmd();
        self.pos += 1;
        let mut negated = false;
        let mut condition = trim(&cmd[3..]).to_string();
        if condition.starts_with("not ") {
            negated = true;
            condition = trim(&condition[4..]).to_string();
        }
        if condition.starts_with("{{") && condition.ends_with("}}") && condition.len() > 4 {
            condition = trim(&condition[2..condition.len() - 2]).to_string();
        }
        let then_body = self.nodes(&["else", "endif"]);
        let mut else_body = Vec::new();
        if self.pos < self.tokens.len() && trim(&self.cmd()) == "else" {
            self.pos += 1;
            else_body = self.nodes(&["endif"]);
        }
        if self.pos < self.tokens.len() {
            self.pos += 1;
        }
        Node::If { condition, negated, then_body, else_body }
    }
}

// ---- evaluation ---------------------------------------------------------------------

struct Engine<'a> {
    data: &'a ThemeData,
    options: &'a Options,
    errors: u32,
    unsupported: Option<&'static str>,
    scopes: Vec<BTreeMap<String, Scope>>,
}

impl Engine<'_> {
    fn scope_get(&self, name: &str) -> Option<&Scope> {
        self.scopes.iter().rev().find_map(|s| s.get(name))
    }

    fn colors_map(&self) -> BTreeMap<String, Scope> {
        let mut names: Vec<&String> = self
            .data
            .values()
            .flat_map(|m| m.iter().filter(|(_, v)| !v.is_empty()).map(|(k, _)| k))
            .collect();
        names.sort();
        names.dedup();
        let mut colors = BTreeMap::new();
        for name in names {
            let mut mode_map = BTreeMap::new();
            for (mode, mode_data) in self.data {
                if let Some(v) = mode_data.get(name.as_str()) {
                    mode_map.insert(mode.clone(), Scope::Str(v.clone()));
                }
            }
            if let Some(def) = mode_map.get(&self.options.default_mode).cloned() {
                mode_map.insert("default".into(), def);
            }
            colors.insert(name.clone(), Scope::Map(mode_map));
        }
        colors
    }

    fn resolve_from_scope(&self, base: &str) -> Scope {
        let mut parts: Vec<&str> = base.split('.').collect();
        if parts.last() == Some(&"") && base.ends_with('.') {
            parts.pop();
        }
        if base.is_empty() || parts.is_empty() {
            return Scope::None;
        }
        let Some(first) = self.scope_get(parts[0]) else { return Scope::None };
        let mut value = first.clone();
        for part in &parts[1..] {
            value = match value {
                Scope::Map(map) => match map.get(*part) {
                    Some(v) => v.clone(),
                    None => return Scope::None,
                },
                Scope::Array(arr) => match stoul(part) {
                    Some(idx) if idx < arr.len() => arr[idx].clone(),
                    _ => return Scope::None,
                },
                Scope::Str(s) => {
                    if s.len() >= 7 && s.starts_with('#') && KNOWN_FORMATS.contains(part) {
                        return match Color::from_hex(&s) {
                            Some(color) => Scope::Str(format_color(&RichColor { color, alpha: 1.0 }, part)),
                            None => Scope::None,
                        };
                    }
                    return Scope::None;
                }
                _ => return Scope::None,
            };
        }
        value
    }

    fn resolve_expression(&mut self, expr: &str) -> Scope {
        let parts = split_pipes(expr);
        if parts.is_empty() {
            return Scope::None;
        }
        let base = trim(&parts[0]).to_string();
        let filters: Vec<String> = parts[1..].iter().map(|f| trim(f).to_string()).collect();

        let mut resolved = match base.as_str() {
            "mode" => Scope::Str(self.options.default_mode.clone()),
            "closest_color" => Scope::Str(self.options.closest_color.clone()),
            "image" => Scope::Str(self.options.image_path.clone()),
            "config_dir" => Scope::Str(self.options.config_dir.clone()),
            "config_file" => Scope::Str(self.options.config_file.clone()),
            _ => {
                let from_scope = self.resolve_from_scope(&base);
                if !matches!(from_scope, Scope::None) {
                    from_scope
                } else if base.starts_with("colors.") {
                    return Scope::Str(self.process_color_expression(&base, &filters));
                } else if base.starts_with("palettes.") {
                    self.unsupported = Some("palettes.* tonal palette expressions");
                    return Scope::Str(format!("{{{{{base}}}}}"));
                } else {
                    return Scope::Str(format!("{{{{{expr}}}}}"));
                }
            }
        };

        for filter in &filters {
            let (name, arg) = parse_filter(filter);
            let text = |v: &Scope| scope_to_string(v);
            match name.as_str() {
                "replace" => resolved = Scope::Str(apply_replace(&text(&resolved), &arg)),
                "lower_case" => resolved = Scope::Str(text(&resolved).to_ascii_lowercase()),
                "camel_case" => resolved = Scope::Str(to_camel_case(&text(&resolved))),
                "pascal_case" => resolved = Scope::Str(to_pascal_case(&text(&resolved))),
                "snake_case" => resolved = Scope::Str(join_lower(&text(&resolved), "_")),
                "kebab_case" => resolved = Scope::Str(join_lower(&text(&resolved), "-")),
                "to_color" => resolved = Scope::Str(text(&resolved)),
                n if COLOR_ARG_FILTERS.contains(&n) => {
                    match as_rich_color(&resolved).and_then(|c| apply_color_arg_filter(c, n, &arg)) {
                        Ok(c) => resolved = Scope::Color(c),
                        Err(()) => self.errors += 1,
                    }
                }
                n if COLOR_FILTERS.contains(&n) => {
                    match as_rich_color(&resolved).and_then(|c| apply_color_filter(c, n, &arg)) {
                        Ok(c) => resolved = Scope::Color(c),
                        Err(()) => self.errors += 1,
                    }
                }
                _ => {}
            }
        }
        resolved
    }

    /// Shared tail of `colors.*` / `palettes.*`: text filters short-circuit, colour filters chain.
    fn finish_color(&mut self, mut color: RichColor, format_type: &str, filters: &[String], base: &str) -> String {
        for filter in filters {
            let (name, arg) = parse_filter(filter);
            let fmt = |c: &RichColor| format_color(c, format_type);
            match name.as_str() {
                "replace" => return apply_replace(&fmt(&color), &arg),
                "lower_case" => return fmt(&color).to_ascii_lowercase(),
                "camel_case" => return to_camel_case(&fmt(&color)),
                "pascal_case" => return to_pascal_case(&fmt(&color)),
                "snake_case" => return join_lower(&fmt(&color), "_"),
                "kebab_case" => return join_lower(&fmt(&color), "-"),
                "to_color" => continue,
                n if COLOR_ARG_FILTERS.contains(&n) => match apply_color_arg_filter(color, n, &arg) {
                    Ok(c) => color = c,
                    Err(()) => {
                        self.errors += 1;
                        return format!("{{{{{base}}}}}");
                    }
                },
                n if COLOR_FILTERS.contains(&n) => match apply_color_filter(color, n, &arg) {
                    Ok(c) => color = c,
                    Err(()) => {
                        self.errors += 1;
                        return format!("{{{{{base}}}}}");
                    }
                },
                _ => {}
            }
        }
        format_color(&color, format_type)
    }

    fn process_color_expression(&mut self, base: &str, filters: &[String]) -> String {
        let Some(m) = re().color_expr.captures(base) else {
            self.errors += 1;
            return format!("{{{{{base}}}}}");
        };
        let mut color_name = m[1].to_string();
        let mode = m[2].to_string();
        let format_type = m[3].to_string();
        match color_name.as_str() {
            "hover" => color_name = "surface_container_high".into(),
            "on_hover" => color_name = "on_surface".into(),
            _ => {}
        }
        let mode_key = if mode == "default" { self.options.default_mode.as_str() } else { mode.as_str() };
        let Some(hex) = self.data.get(mode_key).and_then(|m| m.get(&color_name)) else {
            self.errors += 1;
            return format!("{{{{UNKNOWN:{color_name}.{mode}}}}}");
        };
        let Some(color) = Color::from_hex(hex) else {
            self.errors += 1;
            return format!("{{{{{base}}}}}");
        };
        self.finish_color(RichColor { color, alpha: 1.0 }, &format_type, filters, base)
    }

    fn resolve_text(&mut self, text: &str) -> String {
        let mut out = String::new();
        let mut last = 0;
        for m in re().expr.captures_iter(text) {
            let whole = m.get(0).unwrap();
            out.push_str(&text[last..whole.start()]);
            let expr = trim(&m[1]).to_string();
            let value = self.resolve_expression(&expr);
            out.push_str(&scope_to_string(&value));
            last = whole.end();
        }
        out.push_str(&text[last..]);
        out
    }

    fn resolve_iterable(&mut self, expr: &str) -> Vec<Scope> {
        if let Some(m) = re().range.captures(expr) {
            let (Ok(start), Ok(end)) = (m[1].parse::<i32>(), m[2].parse::<i32>()) else { return vec![] };
            return (start..end).map(|i| Scope::Num(f64::from(i))).collect();
        }
        let pair = |k: &str, v: Scope| {
            Scope::Map(BTreeMap::from([("key".to_string(), Scope::Str(k.to_string())), ("value".to_string(), v)]))
        };
        if expr == "colors" {
            return self.colors_map().into_iter().map(|(k, v)| pair(&k, v)).collect();
        }
        if expr.starts_with("palettes.") {
            self.unsupported = Some("palettes.* iteration");
            return vec![];
        }
        match self.scope_get(expr) {
            Some(Scope::Array(a)) => a.clone(),
            Some(Scope::Map(m)) => m.iter().map(|(k, v)| pair(k, v.clone())).collect(),
            _ => vec![],
        }
    }

    fn eval_nodes(&mut self, nodes: &[Node]) -> String {
        let mut out = String::new();
        for node in nodes {
            match node {
                Node::Text(t) => out += &self.resolve_text(t),
                Node::For { variables, iterable, body } => out += &self.eval_for(variables, iterable, body),
                Node::If { condition, negated, then_body, else_body } => {
                    let value = self.resolve_expression(condition);
                    let truthy = is_truthy(&value) != *negated;
                    out += &self.eval_nodes(if truthy { then_body } else { else_body });
                }
            }
        }
        out
    }

    fn eval_for(&mut self, variables: &[String], iterable: &str, body: &[Node]) -> String {
        let items = self.resolve_iterable(iterable);
        let total = items.len();
        let mut out = String::new();
        for (index, item) in items.into_iter().enumerate() {
            let meta = BTreeMap::from([
                ("index".to_string(), Scope::Num(index as f64)),
                ("first".to_string(), Scope::Bool(index == 0)),
                ("last".to_string(), Scope::Bool(index + 1 == total)),
            ]);
            let mut frame = BTreeMap::from([("loop".to_string(), Scope::Map(meta))]);
            match &item {
                Scope::Map(m) if m.contains_key("key") && m.contains_key("value") => {
                    if variables.len() >= 2 {
                        frame.insert(variables[0].clone(), m["key"].clone());
                        frame.insert(variables[1].clone(), m["value"].clone());
                    } else if let Some(v) = variables.first() {
                        frame.insert(v.clone(), m["key"].clone());
                    }
                }
                _ => {
                    if let Some(v) = variables.first() {
                        frame.insert(v.clone(), item.clone());
                    }
                }
            }
            self.scopes.push(frame);
            out += &self.eval_nodes(body);
            self.scopes.pop();
        }
        out
    }
}

/// `std::stoul` prefix semantics (leading whitespace, optional sign, digit run).
fn stoul(s: &str) -> Option<usize> {
    let t = s.trim_start_matches(is_c_space);
    let (neg, t) = match t.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, t.strip_prefix('+').unwrap_or(t)),
    };
    let digits: String = t.chars().take_while(char::is_ascii_digit).collect();
    if digits.is_empty() {
        return None;
    }
    let v: usize = digits.parse().ok()?;
    if neg {
        // Negation wraps; only zero stays a small index.
        (v == 0).then_some(0)
    } else {
        Some(v)
    }
}

pub fn render(data: &ThemeData, options: &Options, template: &str) -> RenderResult {
    let tokens = tokenize(template);
    let mut parser = Parser { tokens: &tokens, pos: 0, errors: 0 };
    let nodes = parser.nodes(&[]);
    let parse_errors = parser.errors;

    let mut engine = Engine { data, options, errors: parse_errors, unsupported: None, scopes: Vec::new() };
    if let Some(table) = &options.config_table {
        let mut root = BTreeMap::new();
        root.insert("config".to_string(), from_json(table));
        root.insert("icon_theme".to_string(), Scope::Str(options.icon_theme.clone()));
        engine.scopes.push(root);
    }
    let text = engine.eval_nodes(&nodes);
    RenderResult { text, error_count: engine.errors, unsupported: engine.unsupported }
}
