## Transmission Repository – Agent Guide

### Overview
- Core library: **libtransmission** (C/C++ static library). 
- Additional static lib: **libtransmission‑app** (app‑specific helpers). 
- UI front‑ends: `gtk/`, `qt/`, `macosx/`, `cli/`, `web/` (JS). 
- Utilities: `utils/` (command‑line tools).

### Project Layout
| Directory | Purpose |
|-----------|---------|
| `libtransmission/` | Core BitTorrent engine implementation. |
| `libtransmission-app/` | Small wrapper library used by the GUI front‑ends. |
| `gtk/`, `qt/`, `macosx/` | GUI clients (GTK, Qt, macOS). |
| `cli/` | Legacy `transmission-cli` (deprecated). |
| `utils/` | `transmission-show/create/edit/remote` command‑line helpers. |
| `web/` | Web UI source (Node.js, ESBuild). |
| `tests/` | Unit tests for the core library. |
| `CMakeLists.txt` | Top‑level build configuration; defines options via `tr_auto_option`. |

### Build & Install (Linux/macOS)
```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/transmission/transmission Transmission
cd Transmission
# Configure (choose build type, enable components)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DENABLE_GTK=ON -DENABLE_QT=ON -DENABLE_UTILS=ON
# Build
cmake --build build -- -j$(nproc)
# Install (requires sudo for system dirs)
sudo cmake --install build
```
- Use `-DENABLE_DAEMON=ON` to build the daemon. 
- `-DENABLE_MAC=ON` builds the macOS app (Xcode required). 
- `-DENABLE_QT=ON` builds the Qt client; requires Qt dev packages. 
- `-DENABLE_GTK=ON` builds the GTK client; requires GTK3/4 dev packages.

### Building the Web UI
```bash
cd web
npm ci                # install dev deps
npm run build         # produce web/public_html/*
# Optional: lint / fix
git diff --exit-code web   # CI will reject changes to generated files
```
The web UI is generated into `web/public_html/`; CI disallows direct changes – use the `webapp` workflow to create a PR that updates them.

### Testing
```bash
# After building (see above)
cd build
ctest -j$(nproc) --output-on-failure   # runs all libtransmission unit tests
```
The GitHub Actions `run-tests` job sets up a crash‑friendly environment, using `catchsegv` on Linux when sanitizers are disabled.

### Code Style
- C/C++: `./code_style.sh` (auto‑fixes) or `./code_style.sh --check`.
- JavaScript/TypeScript: `npm run lint:fix` (CI runs `npm run lint`).
- CI enforces style and fails on any diff.

### Key CMake Options (excerpt)
- `ENABLE_DAEMON` – build the daemon binary.
- `ENABLE_UTILS` – build `transmission-show`, `transmission-create`, `transmission-edit`, `transmission-remote`.
- `ENABLE_GTK` / `ENABLE_QT` – build corresponding GUI.
- `ENABLE_MAC` – build macOS bundle.
- `ENABLE_WERROR` – treat warnings as errors.
- `TR_AUTO_OPTION` mechanism: options default to `AUTO` (enabled if dependencies are found). Use `-DENABLE_<COMP>=ON|OFF` to force.

### Common Gotchas
- **Submodules**: Many third‑party libraries are included as submodules; always clone with `--recurse-submodules`.
- **Dependency versions**: The library checks minimum versions (e.g., OpenSSL ≥ 1.1.0, libevent ≥ 2.1.0). Mismatched versions cause CMake configuration errors.
- **Web UI generation**: Direct edits to generated files are rejected; make changes in source files under `web/` and let CI regenerate.
- **macOS builds**: Xcode 12+ required; `objectVersion = 54` and `compatibilityVersion = "Xcode 12.0"` must be present in the project file (enforced by `code_style.sh`).
- **Sanitizer builds**: CI runs tests without sanitizers by default; enable them via `-DENABLE_SANITIZERS=ON` for local debugging.

### Frequently Used Commands
| Task | Command |
|------|---------|
| Configure (debug) | `cmake -B build -DCMAKE_BUILD_TYPE=Debug` |
| Build all | `cmake --build build -j$(nproc)` |
| Run unit tests | `cd build && ctest --output-on-failure` |
| Reformat C/C++ code | `./code_style.sh` |
| Lint/fix JS | `npm run lint:fix` |
| Generate web assets | `npm run build && npm run generate-buildonly` |

### Agent Tips
- Run `grep -R "tr_auto_option" -n CMakeLists.txt` to see all optional components.
- Use `ctest -N` to list test names without executing.
- CI environment variables: `OS_FAMILY` (linux/macOS) used by test runner.
- The repository does **not** contain a top‑level `AGENTS.md`; this file should be added to assist future automation.
