//! What About shows about the computer: its name, the operating system, the kernel, the processor, the memory and the compositor. Read from the
//! system's own files; parsing is separate from reading so it can be tested.

/// `PRETTY_NAME` of an `os-release` file.
pub fn os_name(os_release: &str) -> Option<String> {
    os_release.lines().find_map(|line| {
        let value = line.strip_prefix("PRETTY_NAME=")?.trim().trim_matches('"');
        (!value.is_empty()).then(|| value.chars().filter(|c| !c.is_control()).take(80).collect())
    })
}

/// The first `model name` of `/proc/cpuinfo`, with runs of spaces made single.
pub fn cpu_name(cpuinfo: &str) -> Option<String> {
    cpuinfo.lines().find_map(|line| {
        let (key, value) = line.split_once(':')?;
        (key.trim() == "model name").then(|| value.split_whitespace().collect::<Vec<_>>().join(" "))
    })
}

/// `MemTotal` of `/proc/meminfo` as "15.5 GB" (the way a person counts it: gibibytes, one decimal).
pub fn memory_text(meminfo: &str) -> Option<String> {
    let kib: f64 = meminfo
        .lines()
        .find_map(|line| line.strip_prefix("MemTotal:"))?
        .split_whitespace()
        .next()?
        .parse()
        .ok()?;
    Some(format!("{:.1} GB", kib / 1024.0 / 1024.0))
}

fn read(path: &str) -> String {
    std::fs::read_to_string(path).unwrap_or_default()
}

/// Label and value pairs, in the order they are shown; a line that could not be read is left out.
pub fn system_lines() -> Vec<(String, String)> {
    let one = |text: String| Some(text.trim().to_string()).filter(|t| !t.is_empty());
    let compositor = helix_compositor::compositor_ipc::compositor_version();
    [
        ("Computer name", one(read("/proc/sys/kernel/hostname"))),
        ("Operating system", os_name(&read("/etc/os-release"))),
        ("Kernel", one(read("/proc/sys/kernel/osrelease"))),
        ("Processor", cpu_name(&read("/proc/cpuinfo"))),
        ("Memory", memory_text(&read("/proc/meminfo"))),
        ("Compositor", compositor),
    ]
    .into_iter()
    .filter_map(|(label, value)| value.map(|v| (label.to_string(), v.chars().take(100).collect())))
    .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_system_files_are_read_the_way_they_are_written() {
        assert_eq!(
            os_name(
                "NAME=Fedora\nPRETTY_NAME=\"Fedora Linux 44 (Workstation Edition)\"\nID=fedora\n"
            )
            .as_deref(),
            Some("Fedora Linux 44 (Workstation Edition)")
        );
        assert_eq!(os_name("NAME=x\n"), None);
        assert_eq!(
            cpu_name("processor\t: 0\nmodel name\t: AMD  Ryzen 7   7840U\ncpu MHz\t: 1\n")
                .as_deref(),
            Some("AMD Ryzen 7 7840U")
        );
        assert_eq!(cpu_name("flags: x\n"), None);
        assert_eq!(
            memory_text("MemTotal:       16253952 kB\nMemFree: 1 kB\n").as_deref(),
            Some("15.5 GB")
        );
        assert_eq!(memory_text("nothing"), None);
    }
}
