"""Start, watch and stop a local llama-server process.

The chat program runs two of them: the LFM3 chat model on the main port and
the EmbeddingGemma embedder on its own. Each instance is built by
``chat_server()`` here or by ``embedding_server()`` in ``embedder.py``, calls
``ensure_running()`` once at start-up and ``stop()`` when it exits. If a server
is already listening on the configured port (for example because you started
one yourself), it is reused and left running.
"""

import subprocess
import time
from pathlib import Path
from typing import Callable, Optional, Sequence

import requests

import config


def server_is_up(url: str) -> bool:
    """True when /health reports 200, meaning the model is loaded and ready."""
    try:
        return requests.get(f"{url}/health", timeout=2).status_code == 200
    except requests.RequestException:
        return False


def _tail(path: Path, lines: int = 30) -> str:
    """Return the last few lines of a log file, for error messages."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return "(no log file)"
    return "\n".join(text[-lines:])


class LlamaServer:
    """Owns one llama-server child process."""

    def __init__(
        self,
        model_path: Path,
        port: int,
        *,
        label: str = "llama-server",
        mmproj_path: Optional[Path] = None,
        context_size: int = config.CONTEXT_SIZE,
        extra_args: Sequence[str] = (),
        log_path: Path = config.SERVER_LOG,
        log: Callable[[str], None] = print,
    ) -> None:
        self.model_path = model_path
        self.mmproj_path = mmproj_path
        self.port = port
        self.url = f"http://{config.SERVER_HOST}:{port}"
        self.label = label
        self.context_size = context_size
        self.extra_args = list(extra_args)
        self.log_path = log_path
        self.log = log
        self.process: Optional[subprocess.Popen] = None
        self._log_file = None

    def is_up(self) -> bool:
        return server_is_up(self.url)

    def ensure_running(self) -> None:
        if self.is_up():
            self.log(f"Using {self.label} already running at {self.url}")
            return
        self.start()

    def start(self) -> None:
        missing = self._missing_files()
        if missing:
            raise SystemExit("Missing required files:\n  " + "\n  ".join(missing))

        config.LOG_DIR.mkdir(parents=True, exist_ok=True)
        self._log_file = open(self.log_path, "w", encoding="utf-8", errors="replace")

        self.log(f"Starting {self.label} ... (loading the model onto the GPU)")
        # Under pythonw there is no console to inherit, and Windows would give
        # this console-subsystem child its own visible window. Creation flags
        # keep it headless; its output already goes to the log file.
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.process = subprocess.Popen(
            self._command(),
            stdout=self._log_file,
            stderr=subprocess.STDOUT,
            creationflags=flags,
        )
        self.log(f"Server log: {self.log_path}")
        self._wait_until_ready()

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.log(f"Stopping {self.label} ...")
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process = None

        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None

    # -- internals ---------------------------------------------------------

    def _missing_files(self) -> list[str]:
        required = [
            ("llama-server", config.LLAMA_SERVER),
            ("model", self.model_path),
        ]
        if self.mmproj_path is not None:
            required.append(("mmproj", self.mmproj_path))
        return [f"{label}: {path}" for label, path in required if not path.exists()]

    def _command(self) -> list[str]:
        command = [
            str(config.LLAMA_SERVER),
            "-m", str(self.model_path),
        ]
        if self.mmproj_path is not None:
            command += ["--mmproj", str(self.mmproj_path)]
        command += [
            "--host", config.SERVER_HOST,
            "--port", str(self.port),
            "-c", str(self.context_size),
            "-ngl", str(config.GPU_LAYERS),
            *self.extra_args,
        ]
        return command

    def _wait_until_ready(self) -> None:
        deadline = time.monotonic() + config.SERVER_START_TIMEOUT
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                code = self.process.returncode
                tail = self._log_tail()
                self.stop()
                raise RuntimeError(
                    f"{self.label} exited during start-up (code {code}).\n{tail}"
                )
            if self.is_up():
                self.log("Model ready.\n")
                return
            time.sleep(0.5)

        tail = self._log_tail()
        self.stop()
        raise RuntimeError(
            f"{self.label} was not ready within {config.SERVER_START_TIMEOUT}s. "
            f"Last log lines:\n{tail}"
        )

    def _log_tail(self) -> str:
        if self._log_file is not None:
            self._log_file.flush()
        return _tail(self.log_path)


def chat_server(log: Callable[[str], None] = print) -> LlamaServer:
    """The LFM3 chat model with its vision projector."""
    return LlamaServer(
        config.MODEL_PATH,
        config.SERVER_PORT,
        label="llama-server",
        mmproj_path=config.MMPROJ_PATH,
        extra_args=config.SERVER_EXTRA_ARGS,
        log_path=config.SERVER_LOG,
        log=log,
    )
