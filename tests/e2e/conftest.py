"""Fixtures for the end-to-end suite.

These tests are black-box: they start the real `ledger_engine` binary, talk to it
over a real TCP socket, and know nothing about its internals. That is the whole
point — they exercise exactly the surface a browser, a load script, or a person
with `nc` would use, so they keep working when the storage layer underneath is
replaced in Step 6.

The engine only runs on Linux (epoll). On macOS, run them inside the container:

    make shell
    make e2e
"""

from __future__ import annotations

import os
import pathlib
import socket
import subprocess
import time
from collections.abc import Iterator

import pytest

from client import LedgerClient

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPO_ROOT / "build" / "src" / "ledger_engine"

# How long to wait for a freshly spawned engine to start listening.
STARTUP_TIMEOUT_SECONDS = 10.0


def _free_port() -> int:
    """Ask the OS for an unused port.

    There is a small race between closing this socket and the engine binding it,
    but the alternative — hard-coded ports — makes tests collide with each other
    and with whatever is already running on the machine. That failure is common;
    this one is rare and obvious.
    """
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _wait_until_listening(port: int, process: subprocess.Popen[bytes]) -> None:
    """Poll the port rather than parsing stdout — the banner text is localised."""
    deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"engine exited during startup with code {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError(f"engine did not start listening on port {port}")


@pytest.fixture(scope="session")
def engine_binary() -> pathlib.Path:
    """Locate the built binary, or skip the whole suite with a useful message."""
    override = os.environ.get("LEDGER_ENGINE_BINARY")
    binary = pathlib.Path(override) if override else DEFAULT_BINARY
    if not binary.exists():
        pytest.skip(
            f"{binary} not found — build it first (`make build`), "
            "and remember the server needs Linux"
        )
    return binary


@pytest.fixture
def engine(engine_binary: pathlib.Path) -> Iterator[dict[str, int]]:
    """A freshly started engine, one per test.

    Per-test rather than per-session on purpose: the engine seeds its demo
    accounts at startup, so a new process is a clean ledger. Tests that move
    money therefore cannot leak state into each other, and each one can assert
    against the documented starting balances.
    """
    json_port = _free_port()
    binary_port = _free_port()

    process = subprocess.Popen(
        [
            str(engine_binary),
            "--port",
            str(binary_port),
            "--json-port",
            str(json_port),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    try:
        _wait_until_listening(json_port, process)
        yield {"json": json_port, "binary": binary_port}
    finally:
        # SIGINT, not SIGKILL: the graceful path is part of what we are testing.
        # It drains the queue, prints the run summary, and re-checks invariants.
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


@pytest.fixture
def client(engine: dict[str, int]) -> Iterator[LedgerClient]:
    """One connected client on the JSON port."""
    with LedgerClient(engine["json"]) as connection:
        yield connection
