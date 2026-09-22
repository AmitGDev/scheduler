"""Project-specific external dependency setup.

Installs GCC 16 and G++ 16 on Linux to provide the required compiler
and libstdc++ runtime.

Requires a distribution that carries gcc-16 in its archive (Ubuntu 26.04
or later). Retries transient repository and command failures with bounded
backoff.

The script safely skips installation on non-Linux platforms.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import time

GCC_VERSION = "16"

APT_DOWNLOAD_RETRIES = 5
COMMAND_ATTEMPTS = 3
DOWNLOAD_ATTEMPTS = 3
INDEX_AVAILABILITY_ATTEMPTS = 5
INITIAL_BACKOFF_SECONDS = 5
MAX_BACKOFF_SECONDS = 30

# A hung repository connection must not consume the whole job timeout.
# Ceilings are sized at a few times the healthy duration of each step (the
# full toolchain installs in under 3 minutes), so a real hang is retried
# quickly. The unpack phase is purely local, so a long run there indicates a
# real problem rather than a slow network.
UPDATE_TIMEOUT_SECONDS = 60
DOWNLOAD_TIMEOUT_SECONDS = 300
UNPACK_TIMEOUT_SECONDS = 180

# The root-side `timeout` is the primary killer; Python's timeout is only a
# backstop, so it must outlast the former or it would pre-empt a clean stop.
KILL_AFTER_SECONDS = 10
BACKSTOP_GRACE_SECONDS = 60

# coreutils `timeout` exits 124 on a normal expiry and 137 (128 + SIGKILL)
# when it had to escalate after --kill-after.
TIMEOUT_EXIT_STATUSES = (124, 137)


def _run(command: list[str]) -> None:
    """Run a command and raise if it fails."""
    print(f"Running: {' '.join(command)}", flush=True)
    subprocess.run(command, check=True)


def _is_root() -> bool:
    # The sys.platform check lets mypy skip os.geteuid on Windows runners.
    return sys.platform != "win32" and os.geteuid() == 0


def _privileged(command: list[str]) -> list[str]:
    """Prefix sudo only when needed, so container jobs running as root work."""
    return command if _is_root() else ["sudo", *command]


def _timed(command: list[str], timeout_seconds: int) -> list[str]:
    """Wrap a command in coreutils timeout.

    Must run inside the privileged context: killing only a sudo wrapper
    would leave apt-get alive as an orphan holding the dpkg lock, which
    turns the next retry into a lock failure instead of a network retry.
    """
    return [
        "timeout",
        f"--kill-after={KILL_AFTER_SECONDS}",
        str(timeout_seconds),
        *command,
    ]


def _backoff_delay(attempt: int) -> int:
    """Return the bounded backoff delay for a failed attempt."""
    return min(
        INITIAL_BACKOFF_SECONDS * (2 ** (attempt - 1)),
        MAX_BACKOFF_SECONDS,
    )


def _backstop_timeout(timeout_seconds: int) -> int:
    """Return Python's own timeout, which must outlast the inner one."""
    return timeout_seconds + KILL_AFTER_SECONDS + BACKSTOP_GRACE_SECONDS


def _run_with_retry(
    command: list[str],
    *,
    timeout_seconds: int,
    attempts: int = COMMAND_ATTEMPTS,
) -> None:
    """Run a command with bounded retries; a hang counts as a failure.

    The command is expected to enforce `timeout_seconds` itself (see
    _timed); the Python timeout here only guards against that failing.
    """
    for attempt in range(1, attempts + 1):
        print(f"Attempt {attempt}/{attempts}: {' '.join(command)}", flush=True)

        try:
            returncode = subprocess.run(
                command,
                check=False,
                timeout=_backstop_timeout(timeout_seconds),
            ).returncode
        except subprocess.TimeoutExpired:
            returncode = -1

        if returncode == 0:
            return

        if returncode in (*TIMEOUT_EXIT_STATUSES, -1):
            print(f"Command timed out after {timeout_seconds} seconds.", flush=True)

        if attempt == attempts:
            raise subprocess.CalledProcessError(returncode, command)

        delay = _backoff_delay(attempt)

        print(
            f"Command failed (status {returncode}). Retrying in {delay} seconds...",
            flush=True,
        )
        time.sleep(delay)


def _apt_command(arguments: list[str], *, timeout_seconds: int) -> list[str]:
    """Build a bounded, non-interactive APT command.

    DEBIAN_FRONTEND is passed through env because sudo resets the
    environment; -y alone does not suppress debconf prompts. Translation
    indexes are skipped to shrink the request surface during an outage.
    """
    return _privileged(
        _timed(
            [
                "env",
                "DEBIAN_FRONTEND=noninteractive",
                "apt-get",
                "-o",
                f"Acquire::Retries={APT_DOWNLOAD_RETRIES}",
                "-o",
                "Acquire::Languages=none",
                *arguments,
            ],
            timeout_seconds,
        )
    )


def _as_text(output: str | bytes | None) -> str:
    """Normalize TimeoutExpired output, which may be bytes even in text mode."""
    if output is None:
        return ""
    if isinstance(output, bytes):
        return output.decode(errors="replace")
    return output


def _package_candidate(package: str) -> str | None:
    """Return APT's install candidate for a package, if one exists.

    Uses LC_ALL=C because apt-cache policy output is human-oriented and
    its field formatting should not depend on the CI environment locale.
    """
    result = subprocess.run(
        ["apt-cache", "policy", package],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
        env={
            **os.environ,
            "LC_ALL": "C",
            "LANG": "C",
        },
    )

    if result.returncode != 0:
        return None

    for line in result.stdout.splitlines():
        if not line.startswith("  Candidate:"):
            continue

        candidate = line.removeprefix("  Candidate: ").strip()

        if candidate and candidate != "(none)":
            return candidate

    return None


def _update_apt_until_installable(packages: list[str]) -> None:
    """Update APT until every requested package has an install candidate.

    apt-get update can exit 0 while failing to fetch an index, and can also
    exit non-zero or hang during a repository outage. All three are treated
    as one retryable condition; only the install candidates decide success.
    """
    missing = list(packages)
    last_output = ""
    last_failure = ""
    timeout_message = f"apt-get update timed out after {UPDATE_TIMEOUT_SECONDS}s"

    for attempt in range(1, INDEX_AVAILABILITY_ATTEMPTS + 1):
        print(
            f"APT index availability attempt {attempt}/{INDEX_AVAILABILITY_ATTEMPTS}",
            flush=True,
        )

        try:
            result = subprocess.run(
                _apt_command(["update"], timeout_seconds=UPDATE_TIMEOUT_SECONDS),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=_backstop_timeout(UPDATE_TIMEOUT_SECONDS),
            )
        except subprocess.TimeoutExpired as exc:
            last_output = _as_text(exc.output)
            last_failure = timeout_message
            missing = list(packages)
        else:
            last_output = result.stdout
            if result.returncode in TIMEOUT_EXIT_STATUSES:
                last_failure = timeout_message
                missing = list(packages)
            elif result.returncode != 0:
                last_failure = f"apt-get update exited with status {result.returncode}"
                missing = list(packages)
            else:
                last_failure = ""
                missing = [p for p in packages if _package_candidate(p) is None]

        print(last_output, end="", flush=True)

        if not missing:
            return

        print(
            last_failure
            or "APT update completed, but no install candidate is available "
            f"for: {', '.join(missing)}",
            flush=True,
        )

        if attempt == INDEX_AVAILABILITY_ATTEMPTS:
            break

        delay = _backoff_delay(attempt)

        print(f"Retrying APT update in {delay} seconds...", flush=True)
        time.sleep(delay)

    if last_failure:
        diagnosis = f"Last failure: {last_failure}."
    elif "Failed to fetch" in last_output:
        diagnosis = (
            "An index fetch failed; this is likely a transient repository "
            "availability issue."
        )
    else:
        diagnosis = (
            "No fetch failure was observed; this release or architecture "
            "may not carry the packages (gcc-16 needs Ubuntu 26.04 or later)."
        )

    raise RuntimeError(
        "APT does not provide an install candidate for the requested "
        f"packages after {INDEX_AVAILABILITY_ATTEMPTS} update attempts: "
        f"{', '.join(missing)}. {diagnosis}"
    )


def _install_packages(packages: list[str]) -> None:
    """Install packages in two phases so a timeout can never corrupt dpkg.

    Fetching never touches dpkg state, so killing it on timeout is always
    safe, and APT resumes partial downloads, so retries make progress. The
    unpack phase then runs from the local cache; a failure there is not a
    network blip, so it gets a single attempt instead of retries that could
    hide a half-configured system.
    """
    _run_with_retry(
        _apt_command(
            ["install", "-y", "--download-only", *packages],
            timeout_seconds=DOWNLOAD_TIMEOUT_SECONDS,
        ),
        timeout_seconds=DOWNLOAD_TIMEOUT_SECONDS,
        attempts=DOWNLOAD_ATTEMPTS,
    )

    _run_with_retry(
        _apt_command(
            ["install", "-y", *packages],
            timeout_seconds=UNPACK_TIMEOUT_SECONDS,
        ),
        timeout_seconds=UNPACK_TIMEOUT_SECONDS,
        attempts=1,
    )


def install_gcc() -> None:
    """Install GCC 16 and G++ 16 if they are not already available."""
    gcc_name = f"gcc-{GCC_VERSION}"
    gxx_name = f"g++-{GCC_VERSION}"
    packages = [gcc_name, gxx_name]

    gcc = shutil.which(gcc_name)
    gxx = shutil.which(gxx_name)

    if not gcc or not gxx:
        _update_apt_until_installable(packages)
        _install_packages(packages)

        gcc = shutil.which(gcc_name)
        gxx = shutil.which(gxx_name)

    if not gcc or not gxx:
        raise RuntimeError(
            f"GCC {GCC_VERSION} installation failed: "
            "compiler executables were not found in PATH."
        )

    _run([gcc, "--version"])
    _run([gxx, "--version"])

    print(f"GCC compiler: {gcc}", flush=True)
    print(f"G++ compiler: {gxx}", flush=True)


def main() -> int:
    if platform.system() != "Linux":
        print(
            f"Skipping GCC {GCC_VERSION} installation on {platform.system()}.",
            flush=True,
        )
        return 0

    install_gcc()
    return 0


if __name__ == "__main__":
    sys.exit(main())
