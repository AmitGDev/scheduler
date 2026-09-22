"""Project-specific CI dependency setup.

Runs the individual dependency installation scripts in order:
1. boost_1.92.0.py - installs Boost and exports BOOST_ROOT.
2. gcc_16.py - installs GCC 15 and exports its toolchain environment.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

SCRIPTS = (
    "boost_1.92.0.py",
    "gcc_16.py",
)


def run_script(script_name: str) -> None:
    script_path = Path(__file__).with_name(script_name)

    if not script_path.is_file():
        raise FileNotFoundError(f"Dependency script not found: {script_path}")

    print(f"=== Running {script_name} ===")
    completed = subprocess.run(
        [sys.executable, str(script_path)],
        check=False,
    )

    if completed.returncode != 0:
        raise RuntimeError(
            f"{script_name} failed with exit code {completed.returncode}"
        )


def main() -> int:
    try:
        for script_name in SCRIPTS:
            run_script(script_name)

    except (FileNotFoundError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print("All dependency setup scripts completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
