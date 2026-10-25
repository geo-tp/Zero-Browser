#!/usr/bin/env python3
"""Run the unchanged Qt guard with real timers, fake helpers and private proc fixtures."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import shutil
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--shim", type=Path, required=True)
    args = parser.parse_args()
    scenarios = ("heartbeat-timeout", "heartbeat-live", "heartbeat-reset", "no-heartbeat",
                 "memory-full", "memory-some", "transient", "low-only", "psi-only",
                 "invalid-psi", "missing-available", "memory-boundary")

    def run(scenario):
        with tempfile.TemporaryDirectory(prefix="zero-guard-") as folder:
            environment = os.environ.copy()
            local_shim = Path(folder) / "guard-proc.so"
            shutil.copyfile(args.shim, local_shim)
            environment.update(LD_PRELOAD=str(local_shim),
                               ZERO_TEST_GUARD_PROC=folder,
                               ZERO_TEST_GUARD_SCENARIO=scenario)
            result = subprocess.run([str(args.driver.resolve()), folder], env=environment,
                                    capture_output=True, text=True, timeout=45)
            assert result.returncode == 0, scenario + "\n" + result.stdout + result.stderr
            print(result.stdout.strip(), flush=True)

    with ThreadPoolExecutor(max_workers=len(scenarios)) as pool:
        list(pool.map(run, scenarios))


if __name__ == "__main__":
    main()
