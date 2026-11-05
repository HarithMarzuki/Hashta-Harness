"""Headless entry point: the engine behind a local HTTP service.

The panel spawns this, reads events from it and sends commands back. It owns
the engine's lifetime, so stopping this process stops both llama-servers.

Run with:  py engine_server.py
"""

import threading

import config
import engine as engine_module
from server import EventBus, EngineService, serve


def main() -> int:
    bus = EventBus()
    engine = engine_module.ChatEngine(emit=bus.publish)
    service = EngineService(engine, bus)
    shutdown = threading.Event()

    try:
        httpd = serve(service, bus, config.UI_HOST, config.UI_PORT, shutdown)
    except OSError as exc:
        print(f"[error] cannot listen on {config.UI_HOST}:{config.UI_PORT}: {exc}")
        return 1

    threading.Thread(
        target=httpd.serve_forever, name="http", daemon=True
    ).start()
    print(f"[ui] listening on http://{config.UI_HOST}:{config.UI_PORT}")

    try:
        try:
            engine.start()
        except (RuntimeError, SystemExit) as exc:
            # The panel stays up so the failure can be read on screen; the
            # service refuses turns until a restart.
            engine.phase = "error"
            bus.publish(
                {
                    "type": "status",
                    "phase": "error",
                    "text": str(exc),
                    "models": dict(engine.models),
                }
            )
        service.start()
        shutdown.wait()
    except KeyboardInterrupt:
        pass
    finally:
        service.shutdown()
        engine.close()
        bus.close()
        httpd.shutdown()
        httpd.server_close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
