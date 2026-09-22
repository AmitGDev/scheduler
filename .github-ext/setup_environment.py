"""Project-specific external dependency setup.

Installs Boost and exports BOOST_ROOT for subsequent CI steps.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

BOOST_VERSION = "1.92.0"
BOOST_VERSION_UNDERSCORED = BOOST_VERSION.replace(".", "_")


def _append_github_env(name: str, value: str) -> None:
    github_env = os.environ.get("GITHUB_ENV")
    if not github_env:
        return

    with open(github_env, "a", encoding="utf-8") as handle:
        handle.write(f"{name}={value}\n")


def _download(url: str, destination: Path) -> None:
    print(f"Downloading {url}")
    urllib.request.urlretrieve(url, destination)


def _install_boost_windows(temp_dir: Path) -> Path:
    """Install Boost from the official Windows archive."""

    archive = temp_dir / f"boost_{BOOST_VERSION_UNDERSCORED}.zip"
    url = (
        "https://archives.boost.io/release/"
        f"{BOOST_VERSION}/source/boost_{BOOST_VERSION_UNDERSCORED}.zip"
    )

    _download(url, archive)

    install_root = Path(os.environ.get("RUNNER_TOOL_CACHE", temp_dir))
    install_root /= "boost"
    install_root /= BOOST_VERSION

    if install_root.exists():
        shutil.rmtree(install_root)

    install_root.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(archive) as archive_file:
        archive_file.extractall(install_root.parent)

    extracted_root = install_root.parent / f"boost_{BOOST_VERSION_UNDERSCORED}"

    if not extracted_root.is_dir():
        raise RuntimeError(
            f"Expected Boost directory was not found: {extracted_root}"
        )

    extracted_root.rename(install_root)

    return install_root


def _install_boost_linux(temp_dir: Path) -> Path:
    """Install Boost from the official source archive."""

    archive = temp_dir / f"boost_{BOOST_VERSION_UNDERSCORED}.tar.gz"
    url = (
        "https://archives.boost.io/release/"
        f"{BOOST_VERSION}/source/boost_{BOOST_VERSION_UNDERSCORED}.tar.gz"
    )

    _download(url, archive)

    install_root = Path(os.environ.get("RUNNER_TOOL_CACHE", temp_dir))
    install_root /= "boost"
    install_root /= BOOST_VERSION

    if install_root.exists():
        shutil.rmtree(install_root)

    install_root.parent.mkdir(parents=True, exist_ok=True)

    with tarfile.open(archive, "r:gz") as archive_file:
        archive_file.extractall(install_root.parent)

    extracted_root = install_root.parent / f"boost_{BOOST_VERSION_UNDERSCORED}"

    if not extracted_root.is_dir():
        raise RuntimeError(
            f"Expected Boost directory was not found: {extracted_root}"
        )

    extracted_root.rename(install_root)

    return install_root


def install_boost() -> Path:
    """Install Boost and return its root directory."""

    temp_dir = Path(os.environ.get("RUNNER_TEMP", "/tmp"))
    system = platform.system()

    if system == "Windows":
        return _install_boost_windows(temp_dir)

    if system == "Linux":
        return _install_boost_linux(temp_dir)

    raise RuntimeError(f"Unsupported platform for Boost setup: {system}")


def main() -> int:
    boost_root = install_boost()

    _append_github_env("BOOST_ROOT", str(boost_root))

    print(f"Boost {BOOST_VERSION} installed.")
    print(f"BOOST_ROOT={boost_root}")

    return 0


if __name__ == "__main__":
    sys.exit(main())