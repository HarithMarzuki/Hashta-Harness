# Hashta UI — Win32 + Direct2D Blueprint

Status: Phases 0-6 complete. The panel is usable; avatar sprites are the one
piece intentionally left as a placeholder.

The panel is a C++ front end. The harness stays Python and becomes a local
service behind it. The only contract between them is HTTP on loopback, so the
UI can be developed and tested with `curl` before any C++ exists, and the
engine can be tested with `py main.py` without the UI.

## Verified on this machine

| Thing | State |
|---|---|
| MinGW-w64 GCC 16.2.0, cmake, ninja, windres | present at `C:\mingw64` |
| `d2d1`, `d3d11`, `dwrite`, `dxgi`, `winhttp`, `windowscodecs`, `ole32`, `uuid` import libs | all present |
| MSVC | absent, not needed |
| Python | 3.13 at `C:\Users\admin\AppData\Local\Programs\Python\Python313`, `pythonw.exe` present |
| Ports | 8099 chat, 8100 embed, 8765 UI |

## Architecture

```
Hashta.exe                      C++ / Win32 + D2D + DWrite, one process
  ├─ CreateProcess pythonw engine_server.py   hidden, inside a job object
  ├─ WinHTTP thread   GET /events            SSE, one JSON object per event
  ├─ WinHTTP calls    POST /api/send|stop|reset|shutdown
  └─ HWND             WM_PAINT -> D2D -> DWrite

engine_server.py                Python, one process
  ├─ ChatEngine (engine.py)  ->  llama-server :8099   (spawned, unchanged)
  │                          ->  embed server :8100   (spawned, unchanged)
  └─ ThreadingHTTPServer :8765
```

Why this shape: the harness logic (retrieval, tool loop, SQLite, prompt
assembly) is written, tested and tuned. Rewriting it in C++ buys nothing the
user can see. The UI process is small because it holds no model and no engine.

## Wire protocol

`GET /events` returns `text/event-stream`. Each line is `data: {json}`.

| Event | Fields | UI effect |
|---|---|---|
| `state` | `phase`, `busy`, `models`, `messages`, `stats` | sent once on connect: paint the current conversation |
| `status` | `phase` (`boot`/`warning`/`ready`/`error`), `text`, `models` | boot text, avatar `booting` |
| `user` | `id`, `at`, `text` | append user bubble |
| `recall` | `tokens`, `hits[{id,role,at,score,preview}]` | memory chip on the next reply |
| `tool` | `phase` (`call`/`result`/`limit`), `text`, `result` | trace row |
| `delta` | `text` | append to the streaming bubble |
| `reply` | `id`, `at`, `text`, `stopped` | finalize the bubble |
| `error` | `message` | error toast |
| `shutdown` | - | close the window |

Commands:

| Route | Body | Behaviour |
|---|---|---|
| `POST /api/send` | `{"text":"..."}` | queue one turn; 409 if a turn is running |
| `POST /api/stop` | - | cancel the streaming turn |
| `POST /api/reset` | - | clear the live window, keep memory |
| `POST /api/shutdown` | - | stop llama-server, close the store, exit |
| `GET /api/state` | - | snapshot: phase, messages, stats |

One engine worker thread runs turns. HTTP handlers only enqueue. This keeps
the single-writer behaviour the CLI already relies on.

## Window and rendering

- Client area 432x721 DIPs, min 380x600, no aspect lock, resizable.
- Per-monitor-v2 DPI: manifest plus `SetProcessDpiAwarenessContext`. All
  drawing in DIPs; D2D applies the target DPI.
- `ID2D1HwndRenderTarget`, recreated on `WM_SIZE` and DPI change.
- `IDWriteTextLayout` cached per message; re-laid out on width change or while
  a message is streaming.

Layout bands (DIPs):

| Band | y | Content |
|---|---|---|
| avatar | 0-267 | 220x220 rounded square, centred |
| settings | 267-323 | pills `Clear` `Trace`, right: status dot, gear |
| chat | 323-665 | bubbles, 12px gutters, max 78% width, 18px radius |
| composer | 665-721 | rounded pill + send/stop button |

The composer is a real `EDIT` child control (multiline, borderless, themed
background) positioned inside the pill. That buys IME, text selection,
clipboard and emoji input that would otherwise be hand-written. Enter sends,
Shift+Enter inserts a newline.

Repaint is coalesced: a dirty flag plus a 16 ms timer, never one paint per
token.

## Avatar states

Placeholder art: a rounded square, one colour per state, with the state name
drawn inside. A slow scale pulse while talking is the only motion.

| State | Colour |
|---|---|
| `booting` | `#6B7280` grey |
| `idle` | `#2DD4BF` teal |
| `thinking` | `#F59E0B` amber |
| `remembering` | `#A78BFA` violet |
| `talking` | `#34D399` green |
| `error` | `#F87171` red |

```
booting --ready--> idle <--reply/idle-- talking <--first delta-- thinking
                     |                     ^                        |
                     +--recall/tool--> remembering ---------------+
                     +--error--> error --2s--> idle
```

When the sprites arrive, only the draw function changes. The enum, the
transitions and the engine events that drive them stay as they are.

## Files

| File | Responsibility |
|---|---|
| `engine.py` | new: `ChatEngine`, the turn loop emitting events |
| `server.py` | new: HTTP + SSE, no engine knowledge |
| `engine_server.py` | new: starts servers, opens store, runs the HTTP server |
| `main.py` | modified: CLI becomes a consumer of `engine.py` |
| `ui/main.cpp` | `wWinMain`, window class, message loop, DPI, lifecycle |
| `ui/app.cpp` | app state, event queue, post-message plumbing |
| `ui/render.cpp` | D2D brushes, band drawing, bubble painting |
| `ui/chat.cpp` | message model, layout, scroll, hit-testing |
| `ui/net.cpp` | WinHTTP: SSE reader thread and POST calls |
| `ui/json.h` | minimal JSON reader + string encoder |
| `ui/avatar.cpp` | state enum, placeholder drawing, pulse |
| `ui/app.ico`, `ui/resources.rc` | the executable's icon |
| `build.ps1`, `build.cmd` | MinGW build (the `.cmd` avoids the execution policy) |

Estimated: ~2,200-2,600 lines of C++, ~350 lines of new Python.

## Phases

### Phase 0 - toolchain smoke test (scratch only) - DONE

Compile and run a 60-line Win32 + D2D window that draws one rounded rect and
one string, at 432x721. Confirms the compiler, the import libs, DPI and the
render loop before any design work. Scratch files deleted afterwards.

Accept: window appears, text is crisp at 125% scaling, closes with no console.

Measured 5 Nov 2026, GCC 16.2.0, 432x721 DIP, 120 dpi, screen 1920x1080:

| Probe | Working set |
|---|---|
| GDI-only window, same size | 14.3 MB |
| D2D + DWrite scene, same size | 56.3 MB |
| Python + numpy + requests + sqlite | 38.9 MB |

So the Direct2D/DirectWrite stack itself costs about 42 MB, and the whole
app is projected at roughly 95-115 MB: still about half of WebView2 before
the model is loaded. The 20-40 MB figure in the original sketch was too
optimistic for a hardware D2D target.

Two build notes learned here: g++ needs `TMP`/`TEMP` pointed inside the
workspace or it fails to create temporary files, and `ID2D1RenderTarget` has
no `DrawTextW` - only `DrawText` and `DrawTextLayout`.

### Phase 1 - engine extraction - DONE

`engine.py` gains `ChatEngine` with `start()`, `send(text) -> events`,
`stop()`, `reset()`, `close()`, emitting the protocol events above. `main.py`
becomes a renderer over those events and loses the turn loop.

Accept: CLI transcript is unchanged for a plain turn, a tool round, `/memory`,
`/reset`, and Ctrl+C mid-reply; no assistant row is written for a cancelled
turn.

Result: the CLI transcript is byte-identical (same SHA-256) against stub
servers seeded with fixed timestamps, covering a plain turn, a tool round,
`/memory`, `/reset` and `/help`. Cancellation verified both ways - `stop()`
from another thread and `KeyboardInterrupt` from inside the stream: the reply
is empty, a `cancelled` event is emitted, and only the user row is written.
The tool round emits `tool` twice, `call` then `result`, with the tool text in
`result`.

`LlamaServer` and `embedding_server()` gained an optional `log` callback so
boot lines are events instead of prints; the default is still `print`.

Harness kept at `.tmp/parity/` (stub servers, seeded DB, transcript diff,
cancel test) for regression on later phases.

### Phase 2 - HTTP sidecar - DONE

`server.py` + `engine_server.py`. SSE broadcast to one client, turn worker,
stop flag, shutdown path.

Accept: `curl -N http://127.0.0.1:8765/events` shows `status boot` then
`status ready`; a `POST /api/send` produces `user`, `recall`, deltas, `reply`;
`/api/stop` ends the stream with `stopped: true`; `/api/shutdown` leaves no
`llama-server.exe` behind.

Result: all 22 checks of `.tmp/parity/phase2_test.py` pass against stub
models - boot-then-ready on the stream, a tool round, busy 409, stop
(`stopped: true` plus `cancelled`), reset, 400 on empty text, clean shutdown
with exit code 0 and the port released. `curl -N` was used to confirm the
stream by hand as well.

Three things the implementation had to get right, now recorded here:

- **Chunked transfer, not close-delimited.** A response with no
  `Content-Length` is read by clients in fixed blocks, so the stream stalls
  until the connection ends. `Transfer-Encoding: chunked` delivers each event
  as written; `requests`, curl and WinHTTP all read it incrementally.
- **The stream opens with a `state` snapshot.** Events published before a
  client connects are gone, so a late client would never see `status ready`
  or the live messages. The snapshot closes that race.
- **One SQLite connection, two thread kinds.** The store is opened on the boot
  thread and read by HTTP handlers, which sqlite3 refuses by default. It is
  now opened with `check_same_thread=False` and guarded by a lock; turns stay
  single-writer by construction.

Route list as built: `GET /events`, `GET /api/state`, `GET /api/health`,
`POST /api/send|stop|reset|shutdown`.

### Phase 3 - C++ shell - DONE

Window, DPI, D2D target, the four bands, static sample bubbles, working
composer `EDIT`. No networking yet.

Accept: 432x721 window, resize reflows, wheel scrolls the sample list, Enter
fires a stub handler, IME input works.

Result: `build\hashta_ui.exe` (647 KB, static runtime). All 11 checks of
`.tmp/parity/ui_test.py` pass, driving the real exe through user32: window at
432x721 DIP, composer is a real `EDIT` holding focus, resize reflows, wheel
scrolls, typing reaches the control, Enter clears it and appends bubbles,
clean exit.

Built with a single g++ command rather than CMake: one target, five files, and
CMake would add a layer that buys nothing yet. Link command and flags live in
`build.ps1`; `build.cmd` calls it with `-ExecutionPolicy Bypass` because the
local policy refuses an unsigned script.

Two things learned:

- **A second manifest breaks the link.** MinGW's linker already injects a
  default manifest, and `.rsrc merge failure: multiple non-default manifests`
  is what an embedded `resources.rc` manifest produces. DPI awareness is set
  with `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` before the window
  exists instead, which is equivalent for this app; comctl32 v6 is not needed
  because the composer is a plain `EDIT` and the rest is D2D.
- **The composer has no placeholder text.** `EM_SETCUEBANNER` is ignored on
  multiline edits, so "Enter message here..." would need custom painting.
  Deferred to Phase 6 with the rest of the polish.

### Phase 4 - wire the wire - DONE

Spawn the Python child inside a job object, read SSE on a worker thread,
`PostMessage` events to the UI thread, send/stop/reset, live streaming
bubbles, scroll-to-bottom unless the user has scrolled up.

Accept: full turn end to end; stop mid-reply keeps the partial bubble marked
stopped; closing the window kills Python and both llama-servers (checked in
Task Manager).

Result: `ui/net.cpp` starts `engine_server.py` hidden in a kill-on-close job,
connects a WinHTTP SSE reader on a worker thread, and posts parsed events to
the window thread. The panel now streams replies, sends stop, and makes `New`
call `/api/reset` without touching long-term memory. The build includes
WinHTTP. `.tmp/parity/ui_phase4_test.py` passes all seven checks against
disposable chat and embedding stubs: ready, streamed reply, stop
(`stopped: true`), reset, clean panel exit, and released UI-service port. It
deliberately uses stubs, so it does not load the real model or modify
`memory.db`.

What that run does not prove: with stub models there is no `llama-server`
grandchild to kill, so "no orphaned model server" rests on the job object plus
`/api/shutdown`, not on an observed kill. What is verified is that the Python
child exits and releases the port. Re-check the grandchild case once the real
model has been driven from the panel.

Two things learned:

- **Inherited handles must be created inheritable.** `STARTF_USESTDHANDLES`
  with `bInheritHandles = TRUE` only works if the log file and `NUL` handles
  carry `SECURITY_ATTRIBUTES { bInheritHandle = TRUE }`; without it the child
  gets invalid std handles and its output is silently lost.
- **Benign disconnects were spamming the engine log.** WinHTTP closes
  keep-alive sockets between requests and `socketserver` printed a traceback
  for each one. `UiServer.handle_error` now ignores reset/abort/broken-pipe,
  so `logs\panel-engine.log` holds the listening line and real failures only.


### Phase 5 - avatar, trace, settings - DONE

Colour states driven by events, pulse while talking; memory chip under each
reply expanding to scored hits; tool rows; `New`, `Memory`, status dot, gear.

Accept: all six states reachable and visually distinct; chip numbers match
`/memory`; `New` clears the window without touching `memory.db`.

Result: all fourteen checks of `.tmp/parity/ui_phase5_test.py` pass against a
seeded sandbox database and a stub that performs one `search_memory` round.
Screenshots captured every state (booting, thinking, remembering, talking,
idle, error via a missing model path). The chip reads "3 memories - 103
tokens", matching the engine's `recall` event; expanding it lists the three
hits with their scores, local timestamps and roles. `Memory` hides the trace,
the gear opens a live stats sheet (engine, link, avatar, turns, stored,
embedded), and `New` left the database at five rows before and after.

Three bugs this phase produced, all caught by looking at the pixels:

- **Attaching twice wiped the trace.** `AttachTrace` moves the pending hits
  into the message when the first delta creates it, then ran again on `reply`;
  moving from the now-empty pending vectors replaced the attached hits with
  nothing. It now returns early when there is nothing pending.
- **`%s` in a wide printf is a multibyte string.** MinGW follows ISO C here,
  so `%s` with a `wchar_t*` read the bytes as `char*` and stopped at the first
  NUL: the header rendered as "1.00 2 u". Wide fields need `%ls`.
- **A traced bubble kept the width of its one-line text.** The chip and tool
  rows wrapped into a narrow column; bubbles carrying a trace now have a
  minimum width (`theme::trace_min_width`).

### Phase 6 - polish - DONE

Emoji, scroll easing, keyboard shortcuts, the composer hint, an icon, and a
build that can place a shortcut.

Accept: emoji render, the panel survives a keyboard-driven pass (send, stop,
clear), one command rebuilds and can make a shortcut.

Result: all ten checks of `.tmp/parity/ui_phase6_test.py` pass.

- **Emoji needed no work.** DWrite already falls back to Segoe UI Emoji for
  text layouts; a reply containing a skin-toned emoji, a check mark, accented
  Latin and CJK rendered correctly with no code. The item is closed, not
  implemented.
- **Session restore was rejected** after review. Recall is the memory
  mechanism: turns loaded into the live window would be excluded from the
  memory block by `tracked_ids()` and would occupy the 6000-token history
  budget instead of the 1200-token block. The panel opening empty is correct.
- **Scroll eases** toward a `scroll_target` at 0.35 per 33 ms tick. Pinning to
  the bottom during streaming stays instant; only the wheel animates.
- **Shortcuts**: Esc stops the running turn, Ctrl+N clears the conversation,
  Enter sends, Shift+Enter breaks the line. Esc and Ctrl+N are handled in the
  edit subclass and again in the window proc.
- **Composer hint** is painted by the edit's subclass after the default paint,
  because `EM_SETCUEBANNER` is ignored on multiline edits.
- **Icon**: `ui/app.ico` compiled through `ui/resources.rc`.
- **Shortcut**: `build.cmd -Shortcut` writes `Hashta.lnk` to the Desktop, and
  falls back to the project root when the Desktop is not writable.

One build gotcha: windres forwards `-I` to the preprocessor without quoting,
so an absolute include path containing a space ("Local Agent") splits and the
resource build fails. `build.ps1` runs windres from the project root with
relative arguments instead.

## Feedback round (after first real use)

1. **The composer grows to five wrapped lines**, then scrolls inside itself.
   `Metrics.composer_h` drives the band and the chat viewport shrinks with it.
   `EM_GETLINECOUNT` on the edit gives the wrapped line count. One trap:
   `SetWindowText` does not raise `EN_CHANGE`, so the programmatic clear after
   sending must call `AppUpdateComposer` itself or the box stays tall.
2. **Two model status lines** at the top of the avatar band: "Language model:"
   and "Embedder:", coloured by state - `Waiting` red, `Loading` amber,
   `Ready` green, and `Failed` / `Disabled` red. The engine publishes `models`
   on every status event and in the connect snapshot.

   Nothing is ever drawn inside the avatar square except its state name. Boot
   chatter stays in the log; errors appear in a **toast** above the composer,
   a red-tinted box sized to the message (34-72 DIP) that is dismissed by a
   click, replaced by the next error, or cleared by a successful reply.
3. **The model servers start without a console window.** `llama_server.start`
   passes `CREATE_NO_WINDOW`. Under `pythonw` there is no console to inherit,
   so a console-subsystem child would otherwise allocate its own visible
   window. Verified with a fake `Popen` asserting the flag rather than by
   watching the desktop.
4. **The settings pills were renamed**: `New` -> `Clear`, `Memory` -> `Trace`.
   Behaviour is unchanged; the labels now say what the buttons do. The earlier
   phase records above still use the old names.

## Sprites

The avatar is an animated sprite sheet per state, four 512x512 frames in a
horizontal strip (2048x512), except `error` which is a single 512x512 still.

| State | File | Frames | fps |
|---|---|---|---|
| booting | `sprites/booting.png` | 4 | 2 |
| idle | `sprites/idling.png` | 4 | 2 |
| thinking | `sprites/thinking.png` | 4 | 2.5 |
| remembering | `sprites/reading.png` | 4 | 2.5 |
| talking | `sprites/idling.png` | 4 | 2 |
| error | `sprites/error.png` | 1 | - |

Talking reuses the idle strip by the artist's choice: replies animate with the
idle pose. The sheets are opaque, so each frame is drawn clipped to the same
rounded-square slot the placeholder used (a white portrait card); the art's
framing differs slightly per state, which is the artist's composition.

Loading is lazy per state on first paint, through WIC into an
`ID2D1Bitmap`. The frame index comes from `GetTickCount64` at the state's fps,
reset on every state change, and the 33 ms timer only invalidates when the
index actually changes. A missing or malformed strip (width not a whole number
of frames) falls back to the coloured placeholder with the state name, and a
D2D device loss drops every bitmap so they reload on the next paint.

Two repairs after first use of the sprites:

- **The composer hint blinked at the animation rate.** Every frame repainted
  the whole window, and the parent's present covered the child `EDIT` area, so
  the hint the edit paints for itself was wiped and only came back on its next
  repaint. Measured as two variants in the composer band; fixed by
  `WS_CLIPCHILDREN`, invalidating only the avatar band on a frame change, and
  re-invalidating the edit in the same cycle. Re-measured: one variant across
  168 samples.
- **The animations ran at double the intended speed**; fps is now half of the
  first values (see the table).

## Open decisions

1. JSON in C++: hand-written `ui/json.h` (about 250 lines, tested against
   emoji, quotes and newlines) rather than a 1 MB header. Fall back to
   `nlohmann/json.hpp` if it grows.
2. Text input: native `EDIT` control, not hand-drawn. Decided.
3. Theme: dark by default, colours in one header so a light theme is a
   constant swap.
4. Session restore: rejected. Recall is the memory mechanism; turns restored
   into the live window would be excluded from the block and would spend the
   6000-token budget instead of 1200.

## Risks

- Orphaned model server: mitigated by the job object plus `/api/shutdown`.
  The child-exit path is verified; the grandchild kill is not, because the
  Phase 4 harness uses stub models.
- Bubble text stays plain. Markdown was never promised and is not implemented;
  the renderer treats every reply as wrapped text.
- No git repo. The harness and the panel live in one folder with no history,
  so a bad edit still costs the working state.
