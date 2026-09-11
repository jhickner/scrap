# mux

A coding harness that wraps existing harnesses in headless mode, allowing
subscription use.

- wraps claude, codex, grok, and pi
- can switch backends at runtime, preserving context
- can run multiple sessions with different backends in one window and switch
between them
- voice control
- kitty image support, useful for iterating on graphical projects
- kanban-based software factory

## Build

`mux` requires a C11 compiler, POSIX threads, and libcurl development headers.
On Linux it also links libutil for `forkpty`. For example, on Debian or Ubuntu:

```sh
sudo apt-get install build-essential libcurl4-openssl-dev pkg-config
make
```

On macOS, the command-line developer tools provide the compiler and system
libraries. Homebrew's `curl` may be used if the system SDK is unavailable.

Install under `~/.local/bin` by default, or choose another prefix:

```sh
make install
make install PREFIX=/usr/local
```

### Optional JPEG acceleration

JPEG support always has a fallback decoder. If `pkg-config` can find libjpeg,
or Homebrew's `jpeg-turbo` is installed, the build uses it to decode large JPEGs
at reduced resolution and links its static archive when available. Override
detection with `JPEG_PREFIX=/path/to/jpeg-turbo`.

## Tests and diagnostics

To trace voice input, set `voice_trace=1` in `~/.config/mux/settings` and restart mux; transcript text, draft positions, and sends are appended to `~/.config/mux/voice-events.log` (off by default).

Run the unattended checks with:

```sh
make check
```

Compile every unattended and manual harness without running the manual ones:

```sh
make tests
```

The manual harnesses are built together by `make manual`:

- `build/imagetest <image> [more...]` renders images in a capable terminal.
- `build/keydump` displays the terminal input sequences it receives.
- `build/palette` prints the available terminal color palettes.
- `build/pastetest [image]` inspects the clipboard; supplying an image first
  places it on the macOS clipboard.
- `build/spintest` exercises the live spinner and prompt UI interactively.

Build products and dependency files are removed by `make clean`.
