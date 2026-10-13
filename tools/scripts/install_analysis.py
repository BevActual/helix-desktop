#!/usr/bin/env python3
"""Install Mobius's optional, pinned analysis tools without changing agent configs."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile

REA_VERSION = "6.4.0"
GHIDRA_VERSION = "12.1.4"
GHIDRA_ARCHIVE = "ghidra_12.1.4_PUBLIC_20260921.zip"
GHIDRA_SHA256 = "ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db"
GHIDRA_URL = f"https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/{GHIDRA_ARCHIVE}"
JDK_URL = "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jdk_x64_linux_hotspot_21.0.12.1_1.tar.gz"
JDK_SHA256 = "ce79869e1307ed8ee1e2baa86a412b1eb5b75d10a01006d788a6f968bcfaee94"


def download(url, destination, expected):
    digest = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=60) as source, destination.open("wb") as output:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
            output.write(chunk)
    if digest.hexdigest() != expected:
        raise SystemExit("Download checksum mismatch; nothing was installed.")


def main():
    root = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "helix/analysis-tools"
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    for command in ("npm", "node", "bwrap"):
        if not shutil.which(command):
            raise SystemExit(f"Install {command} first. Analysis needs Node 22.19+ and a full JDK 21+.")
    node = Path(shutil.which("node")).resolve()
    compiler = shutil.which("javac")
    java_home = Path(compiler).resolve().parent.parent if compiler else root / "jdk-21.0.12.1+1"
    if not compiler and not (java_home / "bin/javac").is_file():
        if os.uname().machine != "x86_64":
            raise SystemExit("Install a full JDK 21+ on this architecture first.")
        print("Installing a private Temurin JDK 21 (207 MB); system Java is unchanged…", flush=True)
        with tempfile.TemporaryDirectory(prefix=".jdk-", dir=root) as staging:
            staging = Path(staging)
            archive = staging / "jdk.tar.gz"
            download(JDK_URL, archive, JDK_SHA256)
            with tarfile.open(archive) as bundle:
                bundle.extractall(staging, filter="data")
            (staging / java_home.name).rename(java_home)
    ghidra = root / f"ghidra_{GHIDRA_VERSION}_PUBLIC"
    if not (ghidra / "support/analyzeHeadless").is_file():
        print("Downloading Ghidra 12.1.4 (570 MB) from its official GitHub release…", flush=True)
        with tempfile.TemporaryDirectory(prefix=".install-", dir=root) as staging:
            staging = Path(staging)
            archive = staging / GHIDRA_ARCHIVE
            download(GHIDRA_URL, archive, GHIDRA_SHA256)
            with zipfile.ZipFile(archive) as bundle:
                for member in bundle.infolist():
                    target = (staging / member.filename).resolve()
                    if not target.is_relative_to(staging.resolve()):
                        raise SystemExit("Unsafe archive path.")
                    bundle.extract(member, staging)
                    mode = member.external_attr >> 16
                    if mode:
                        target.chmod(mode & 0o777)
            (staging / ghidra.name).rename(ghidra)
    packages = root / "node"
    subprocess.run(["npm", "install", "--prefix", str(packages), "--ignore-scripts", "--no-audit", "--no-fund", f"rea-agents@{REA_VERSION}"], check=True)
    # No setup command: Mobius owns its connection; IDE/agent configurations stay independent.
    config = {"node": str(node), "packages": str(packages), "ghidra": str(ghidra), "java_home": str(java_home)}
    pending = root / "config.json.tmp"
    pending.write_text(json.dumps(config, indent=2) + "\n")
    pending.chmod(0o600)
    pending.replace(root / "config.json")
    print(f"Installed. Mobius reads {root / 'config.json'}.", flush=True)


if __name__ == "__main__":
    main()
