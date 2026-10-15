//! Upgrades the hardcoded Helix Shell Fedora packages via dnf. Run through pkexec (polkit action `org.helix.Shell.upgrade`,
//! whose exec path is this binary). It takes no arguments beyond `--help`, checks that it is root on a non-atomic Fedora, and
//! then replaces itself with `/usr/bin/dnf upgrade -y --refresh <installed allow-listed packages>`.

use std::os::unix::process::CommandExt;
use std::process::{Command, ExitCode, Stdio};

const DNF_PATH: &str = "/usr/bin/dnf";
const RPM_PATH: &str = "/usr/bin/rpm";

/// Hardcoded RPM names the privileged helper may pass to dnf. Order is the upgrade order; names not installed are skipped.
const ALLOWLISTED_PACKAGES: [&str; 3] = ["helix-shell", "helix", "helix-greeter"];

const ATOMIC_VARIANTS: [&str; 7] = ["silverblue", "kinoite", "sericea", "onyx", "cosmic", "coreos", "iot"];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HostKind {
    Fedora,
    FedoraAtomic,
    Other,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HelperStatus {
    Ready,
    Help,
    ExtraArguments,
    NotRoot,
    NotFedora,
    Atomic,
    NoPackages,
}

#[derive(Debug)]
struct HelperPlan {
    status: HelperStatus,
    dnf_argv: Vec<String>,
}

fn trim(text: &str) -> &str {
    text.trim_start_matches([' ', '\t']).trim_end_matches([' ', '\t', '\r'])
}

fn unquote(value: &str) -> &str {
    let bytes = value.as_bytes();
    if bytes.len() >= 2
        && ((bytes[0] == b'"' && bytes[bytes.len() - 1] == b'"')
            || (bytes[0] == b'\'' && bytes[bytes.len() - 1] == b'\''))
    {
        return &value[1..value.len() - 1];
    }
    value
}

/// The value of `key` in os-release text; empty when absent.
fn os_release_value(text: &str, key: &str) -> String {
    for line in text.split('\n') {
        let line = trim(line);
        if line.len() > key.len() && line.as_bytes()[key.len()] == b'=' && line.starts_with(key) {
            return unquote(&line[key.len() + 1..]).to_string();
        }
    }
    String::new()
}

fn classify_host(os_release: &str, ostree_booted: bool) -> HostKind {
    if ostree_booted {
        return HostKind::FedoraAtomic;
    }
    if os_release_value(os_release, "ID") != "fedora" {
        return HostKind::Other;
    }
    let variant = os_release_value(os_release, "VARIANT_ID");
    if ATOMIC_VARIANTS.contains(&variant.as_str()) {
        return HostKind::FedoraAtomic;
    }
    HostKind::Fedora
}

fn select_installed_packages(installed: &[String]) -> Vec<String> {
    ALLOWLISTED_PACKAGES
        .iter()
        .filter(|allowed| installed.iter().any(|name| name == *allowed))
        .map(|name| (*name).to_string())
        .collect()
}

fn dnf_upgrade_argv(packages: &[String]) -> Vec<String> {
    let mut argv = vec![DNF_PATH.to_string(), "upgrade".into(), "-y".into(), "--refresh".into()];
    argv.extend(packages.iter().cloned());
    argv
}

/// `args` excludes the program name.
fn plan_helper(
    args: &[String],
    is_root: bool,
    os_release: &str,
    ostree_booted: bool,
    installed: &[String],
) -> HelperPlan {
    let plan = |status| HelperPlan { status, dnf_argv: Vec::new() };
    if args.len() == 1 && args[0] == "--help" {
        return plan(HelperStatus::Help);
    }
    if !args.is_empty() {
        return plan(HelperStatus::ExtraArguments);
    }
    if !is_root {
        return plan(HelperStatus::NotRoot);
    }
    match classify_host(os_release, ostree_booted) {
        HostKind::Other => return plan(HelperStatus::NotFedora),
        HostKind::FedoraAtomic => return plan(HelperStatus::Atomic),
        HostKind::Fedora => {}
    }
    let packages = select_installed_packages(installed);
    if packages.is_empty() {
        return plan(HelperStatus::NoPackages);
    }
    HelperPlan { status: HelperStatus::Ready, dnf_argv: dnf_upgrade_argv(&packages) }
}

fn os_release_text() -> String {
    for path in ["/etc/os-release", "/usr/lib/os-release"] {
        if let Ok(text) = std::fs::read_to_string(path) {
            if !text.is_empty() {
                return text;
            }
        }
    }
    String::new()
}

fn rpm_quiet_installed(name: &str) -> bool {
    Command::new(RPM_PATH)
        .args(["-q", "--quiet", name])
        .stdin(Stdio::null())
        .status()
        .is_ok_and(|status| status.success())
}

fn status_message(status: HelperStatus) -> &'static str {
    match status {
        HelperStatus::ExtraArguments => "this helper does not accept arguments",
        HelperStatus::NotRoot => "must run as root via pkexec",
        HelperStatus::NotFedora => "only Fedora is supported",
        HelperStatus::Atomic => "Fedora Atomic images are not supported; use rpm-ostree",
        HelperStatus::NoPackages => "no Helix Shell packages are installed",
        HelperStatus::Help | HelperStatus::Ready => "",
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    // Only consult the system when the arguments are acceptable; the plan decides again with the full picture.
    let installed: Vec<String> = if args.is_empty() {
        ALLOWLISTED_PACKAGES
            .iter()
            .filter(|name| rpm_quiet_installed(name))
            .map(|name| (*name).to_string())
            .collect()
    } else {
        Vec::new()
    };
    // SAFETY: geteuid has no preconditions.
    let is_root = unsafe { libc::geteuid() } == 0;
    let plan = plan_helper(
        &args,
        is_root,
        &os_release_text(),
        std::path::Path::new("/run/ostree-booted").exists(),
        &installed,
    );
    match plan.status {
        HelperStatus::Help => {
            eprintln!(
                "helix-system-upgrade\nUpgrade the hardcoded Helix Shell Fedora packages via dnf.\n\
                 Run through pkexec. Extra arguments are rejected."
            );
            ExitCode::SUCCESS
        }
        HelperStatus::Ready => {
            let error = Command::new(&plan.dnf_argv[0]).args(&plan.dnf_argv[1..]).exec();
            eprintln!("execv {}: {error}", plan.dnf_argv[0]);
            ExitCode::FAILURE
        }
        status => {
            eprintln!("{}", status_message(status));
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn strings(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| (*s).to_string()).collect()
    }

    const FEDORA: &str = "NAME=\"Fedora Linux\"\nID=fedora\nVARIANT_ID=workstation\n";
    const SILVERBLUE: &str = "ID=fedora\nVARIANT_ID=silverblue\n";

    #[test]
    fn reads_os_release_values_and_classifies_hosts() {
        assert_eq!(os_release_value(FEDORA, "ID"), "fedora");
        assert_eq!(os_release_value("ID=\"fedora\"\n", "ID"), "fedora");
        assert_eq!(classify_host(FEDORA, false), HostKind::Fedora);
        assert_eq!(classify_host(FEDORA, true), HostKind::FedoraAtomic);
        assert_eq!(classify_host(SILVERBLUE, false), HostKind::FedoraAtomic);
        assert_eq!(classify_host("ID=arch\n", false), HostKind::Other);
    }

    #[test]
    fn only_allowlisted_packages_in_allowlist_order() {
        assert_eq!(select_installed_packages(&strings(&["htop", "helix", "firefox"])), strings(&["helix"]));
        assert!(select_installed_packages(&[]).is_empty());
        let all = strings(&["helix-greeter", "helix", "helix-shell"]);
        assert_eq!(select_installed_packages(&all), strings(&["helix-shell", "helix", "helix-greeter"]));
        let argv = dnf_upgrade_argv(&strings(&["helix"]));
        assert_eq!(argv, strings(&["/usr/bin/dnf", "upgrade", "-y", "--refresh", "helix"]));
    }

    #[test]
    fn plans_follow_the_cpp_helper() {
        let installed = strings(&["htop", "helix", "firefox"]);
        let plan = |args: &[&str], root, os: &str, ostree, inst: &[String]| {
            plan_helper(&strings(args), root, os, ostree, inst).status
        };
        assert_eq!(plan(&["--help"], false, FEDORA, false, &installed), HelperStatus::Help);
        assert_eq!(plan(&["htop"], true, FEDORA, false, &installed), HelperStatus::ExtraArguments);
        assert_eq!(plan(&[], false, FEDORA, false, &installed), HelperStatus::NotRoot);
        assert_eq!(plan(&[], true, "ID=arch\n", false, &installed), HelperStatus::NotFedora);
        assert_eq!(plan(&[], true, SILVERBLUE, false, &installed), HelperStatus::Atomic);
        assert_eq!(plan(&[], true, FEDORA, false, &strings(&["htop"])), HelperStatus::NoPackages);
        let ready = plan_helper(&[], true, FEDORA, false, &installed);
        assert_eq!(ready.status, HelperStatus::Ready);
        assert_eq!(ready.dnf_argv.last().map(String::as_str), Some("helix"));
    }
}
