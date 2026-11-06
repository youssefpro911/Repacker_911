# RPK — Lossless Windows Compression Workspace

RPK is a lossless folder archiver for Windows with a versioned binary container, LZMA2/XZ, Zstandard long-distance compression, content-defined deduplication, and SHA-256 integrity checks. The portable Windows package is built and tested by GitHub Actions, so users can download and run the prebuilt executables without installing a compiler or CMake.

The native Windows Builder has a dark, resizable workspace for scanning, optimizing, packing, verifying, extracting, and creating a self-extracting archive wrapper. The optional AI connection uses Jan Desktop's local OpenAI-compatible API; it does not require Ollama or a separate llama.cpp command-line app.

## Implemented in this version

- Version 4 archive writer with a codec identifier for every solid block. Version 1, 2, and 3 archives remain readable.
- Per-block codecs: LZMA2/XZ with CRC64, Zstandard with long-distance matching and frame checksum, or Store for already-compressed groups. Every block and restored file is SHA-256 verified.
- 64-bit file sizes, offsets, and extents.
- Recursive folder scanning, extension/magic-byte classification, and Shannon entropy estimates from the first 32 KiB of each file.
- Deterministic content-defined chunking using a fixed Gear-hash table (64 KiB minimum, 256 KiB target, 1 MiB maximum by default).
- Cross-file and within-file chunk deduplication. SHA-256 selects candidate matches; bytes are compared against a temporary raw-chunk spool before a reference is reused.
- Exact shared extents in the archive manifest. Extraction reconstructs every file and verifies its SHA-256.
- Conservative precompression for embedded RFC 1950 zlib streams: an embedded stream is replaced by decoded bytes only if the current zlib runtime can reproduce its exact original compressed bytes. Re-encoding parameters, stream checksum, transformed offset, sizes, and zlib runtime version are recorded in format 3.
- Adaptive codec selection: Auto aggregates up to 512 KiB of representative sampled bytes per file classification, trial-compresses those samples with LZMA2 and Zstandard long-range, compares against Store, and chooses the smallest measured result for each group. Explicit codec selections remain available.
- Bounded parallel block compression. Workers write to separate temporary block files; the main thread assembles them in deterministic block order. `--memory-mib` caps the scheduler's estimated in-flight memory envelope (it is a conservative estimate, not an operating-system hard limit).
- CLI commands: `scan`, `pack`, `list`, `test`, and `extract`.
- Native Win32 desktop GUI with a dark, resizable workspace, Segoe UI typography, live operation log, progress state, and Analyze, Optimize with Jan, Build, Verify, Extract, and Create Setup actions.
- Self-extracting Windows setup stub that can be embedded with a single-file RPK archive and extracts/verifies the archive to a chosen destination.
- Optional Jan integration: Jan runs your selected local model; RPK sends aggregate folder statistics to Jan and validates its structured compression recommendations before applying settings. No file contents or file names are sent by the optimizer.
- Unit/integration tests for SHA-256, deterministic chunk boundaries, scanner classification/entropy, deduplication, zlib precompression round-trip, archive round-trip, Unicode paths, empty files/directories, read-only attributes, legacy formats, and corruption rejection.

## Source layout

```text
rpk_milestone4/
  CMakeLists.txt
  README.md
  FORMAT.md
  BUILD_STATUS.txt
  src/
    archive.hpp       Archive API and binary data structures
    archive.cpp       Container, per-block codec encoders/decoders, bounded parallel compressor, pack/list/verify/extract
    hash.hpp          SHA-256 interface
    hash.cpp          OpenSSL EVP SHA-256 implementation
    chunker.hpp       Streaming content-defined chunker API
    chunker.cpp       Deterministic Gear-hash chunk boundaries
    scanner.hpp       Scan report and classification API
    scanner.cpp       Folder scanner, magic-byte classification, entropy sampling
    precomp.hpp       Exact-match reversible zlib transform structures/API
    precomp.cpp       Bounded zlib stream detection, exact encode matching, rebuild
    main.cpp          CLI
    jan_client.cpp    Loopback-only Jan Local API client
    jan_client.hpp    Jan API settings and chat interface
    gui_win32.cpp     Dark native Windows Builder GUI
    ai_gui_win32.cpp  Jan connection and local model assistant
    rpk.manifest      Windows long-path-aware application manifest
    rpk.rc.in         Manifest resource template
  tests/
    test_main.cpp     Unit and integration tests
```

## Requirements

- C++17 compiler
- CMake 3.20+
- liblzma (XZ Utils)
- OpenSSL Crypto
- zlib (RFC 1950/1951 support)
- Zstandard (libzstd 1.5+ recommended)
- Windows GUI: Windows 10/11 SDK and MSVC (the GUI is a native Win32 application and has no Qt/Tauri runtime dependency)

## Easy Windows setup (no manual CMake commands)

For Windows 10/11 x64, extract this project folder and double-click `Install-RPK.cmd`.
The setup script automatically:

1. Detects your installed Visual Studio C++ toolchain and CMake generator.
2. Downloads and bootstraps vcpkg in your user profile (not `C:\vcpkg`).
3. Installs liblzma, OpenSSL, zlib, and Zstandard for x64.
4. Configures, compiles, and tests the project automatically.
5. Copies the GUI, CLI, and runtime DLLs into `%LOCALAPPDATA%\Programs\RPK`.
6. Creates a desktop shortcut and Start Menu entry, registers an Apps & Features uninstall entry, and launches the GUI.

**First-run requirements:** Visual Studio 2022 or 2026 with the **Desktop development with C++** workload and a Windows 10/11 SDK, CMake, Git for Windows, and an internet connection to download dependencies. The setup script does the CMake/vcpkg work for you; you do not need to type build commands. Setup diagnostics are written to `%TEMP%\RPK-Setup.log`.

If a prerequisite is missing, install it from:
- Visual Studio: https://visualstudio.microsoft.com/vs/community/
- CMake: https://cmake.org/download/
- Git: https://git-scm.com/download/win

After installation, use the **RPK** desktop shortcut. The command-line executable is installed beside the GUI as `rpk.exe`.

### Manual Windows build (optional)

If you prefer to build manually, use an explicit Visual Studio generator (not the default NMake generator):

```powershell
cmake --help | Select-String "Visual Studio"
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

For Visual Studio 2022, use `-G "Visual Studio 17 2022"` instead.

## Build on Linux

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config liblzma-dev libssl-dev zlib1g-dev libzstd-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The CLI and core tests build on Linux. `rpk_gui` is only defined for Windows.

## CLI usage

### Inspect a source folder

```powershell
rpk scan "D:\Games\Example"
```

The scan groups files by the best available extension/magic-byte classification and reports sampled Shannon entropy (0–8 bits per byte). Entropy is a diagnostic estimate; it is not a guarantee of the compression ratio.

### Build an archive

```powershell
rpk pack "D:\Games\Example" -o "D:\Backups\Example.rpk" --preset balanced
```

Higher compression effort and a larger dictionary:

```powershell
rpk pack "D:\Games\Example" -o "D:\Backups\Example-max.rpk" --preset max --dict-mib 128 --block-size-mib 64
```

Disable deduplication for a comparison run:

```powershell
rpk pack "D:\Games\Example" -o "D:\Backups\Example-no-dedup.rpk" --no-dedup
```

Options:

| Option | Meaning |
|---|---|
| `--preset fast\|balanced\|max` | LZMA preset levels 1, 6, or 9 |
| `--dict-mib N` | Dictionary size, 1–1024 MiB; default 64 MiB |
| `--block-size-mib N` | Solid block size, 1–1024 MiB; default 64 MiB |
| `--codec auto\|lzma2\|zstd-long\|store` | Select automatic type-aware grouping or force one codec |
| `--threads N\|auto` | Maximum parallel block workers; 0/auto uses a safe system-derived count |
| `--memory-mib N` | Approximate memory budget used to cap in-flight workers, 128–65536 MiB |
| `--volume-size-mib N` | Optional split size: 1–4096 MiB per part; 0 keeps one `.rpk` file |
| `--no-dedup` | Disable deduplication (content-defined chunking is still used) |
| `--no-precomp` | Disable conservative exact-match zlib precompression |
| `--force` | Replace an existing output archive |

Precompression is enabled by default for files up to 64 MiB. It scans files up to 64 MiB for RFC 1950 zlib-wrapped DEFLATE streams, validates the zlib checksum, and tries standard zlib compression levels 0–9 (default strategy, memory level 8). The scan is bounded to eight candidate streams per file, at most 16 MiB compressed input per candidate, and 128 MiB total decoded candidate data per file. It converts a stream only when the current zlib runtime regenerates the exact byte sequence. Candidate scanning is bounded; large files and unsupported streams are passed through unchanged. Version 3 and 4 archives record `zlibVersion()` and extraction refuses a different runtime version rather than risk generating different bytes. This is deliberately conservative: gzip-wrapped streams, raw DEFLATE streams, unusual zlib strategies, and streams that do not reproduce byte-for-byte are not transformed. Precompression can make the intermediate data larger; its purpose is to expose highly compressible decoded content to the outer solid codec, and it does not guarantee a smaller archive. Use `--no-precomp` for a baseline comparison.

Auto groups files by scanner classification before building solid blocks. It collects up to 512 KiB of representative sampled bytes per group and measures LZMA2 and Zstandard long-range output sizes; Store is selected when neither codec meaningfully reduces the sample. The selected codec is used for the group's blocks. This extra probe happens during the source analysis phase; the activity log reports each selected group codec. Use `--codec` to force a codec for every group. Sample results guide selection but cannot guarantee a smaller result for every individual file. The CDC settings are currently fixed: 64 KiB minimum, 256 KiB target, and 1 MiB maximum chunk size. A duplicate candidate is accepted only after a byte-for-byte comparison, even when its SHA-256 matches. Unique raw chunks are staged in a temporary file beside the archive while building; allow extra temporary disk space approximately equal to the unique source data.

### Self-extracting Windows setup

Place `rpk_setup.exe` beside `rpk.exe`, then run:

```powershell
rpk sfx "D:\Backups\Example.rpk" -o "D:\Backups\Example-Setup.exe"
```

The resulting EXE contains the Windows setup stub plus the RPK archive. Run it to choose an extraction destination; all restored files are verified with SHA-256. SFX creation currently accepts a single-file archive, not a numbered volume set. The setup program is an extractor and verifier, not a full MSI installer: it does not register programs, services, shortcuts, or an uninstaller.

### Split archives across multiple files

Use `--volume-size-mib` when the output needs to fit a file-size limit or be copied in smaller pieces:

```powershell
rpk pack "D:\Games\Example" -o "D:\Backups\Example.rpk" --volume-size-mib 1900
rpk test "D:\Backups\Example.rpk"
rpk extract "D:\Backups\Example.rpk" -C "D:\Games\Restored"
```

The parts are named `Example.rpk.001`, `Example.rpk.002`, and so on. Keep every part together. The reader automatically joins the complete set for listing, verification, and extraction; you can pass either the base name or the first `.001` part. The default value `0` keeps a normal single-file archive.

### List, verify, and extract

```powershell
rpk list "D:\Backups\Example.rpk"
rpk test "D:\Backups\Example.rpk"
rpk extract "D:\Backups\Example.rpk" -C "D:\Games\Restored"
```

Extraction does not overwrite existing files unless `--overwrite` is specified. Block and file hashes are checked during extraction. The `test` command performs a full verification without leaving restored files behind.

## Windows GUI

Launch `rpk_gui.exe` to open the modern Windows workspace. The interface is arranged into five clear areas: **Create archive**, **Compression**, **Actions**, **Restore**, and **Activity**.

1. Choose the source folder and the output archive path. Use **Analyze source** first when you want to inspect file types and entropy estimates.
2. Keep **Balanced (recommended)** for normal use, or choose Fast/Maximum.
3. Build the archive. Deduplication and exact-match zlib precompression are enabled by default.
4. To inspect an existing archive, use **Open**, then choose **Verify archive** or select an extraction folder and click **Extract archive**.

The redesigned workspace uses a dark theme and resizes with the window. The advanced controls include dictionary size, solid block size, adaptive/forced codec selection, worker count, memory budget, deduplication, and reversible precompression. **Optimize with Jan** scans the source and asks the model for validated settings; Jan receives aggregate metadata only, not file names or file contents. In Adaptive codec mode, RPK also runs its own representative sample trials for each file classification and selects the measured best codec (or Store). The activity panel shows the selected codec and operation messages. The progress indicator is still indeterminate, and pause/resume/cancel is not implemented yet. Create Setup embeds a single-file archive in the Windows setup stub; numbered multi-volume archive sets are not supported by SFX creation.

## Archive format

The fixed header is 56 bytes. Version 1 and 2 use the legacy manifest layout; version 2 allows exact shared extents for deduplicated chunks. Version 3 extends each file record with its stored/intermediate size and a list of reversible zlib transforms, and stores the required zlib runtime version once in the manifest. The reader accepts versions 1, 2, 3, and 4.

Version 3 and 4 extraction reverse every transform before validating the original file size and SHA-256. It checks the re-encoded zlib stream SHA-256 and refuses to continue if the local zlib runtime version differs from the version recorded in the archive. This protects byte identity rather than silently accepting a different DEFLATE encoding.

See [FORMAT.md](FORMAT.md) for the binary field layout.

## Benchmarking

Use the same source, output drive, machine, and compression preset for comparisons. Include duplicate-heavy content, text/config data, executable files, and already-compressed media. Do not infer game-folder results from a highly repetitive synthetic file alone.

```powershell
Measure-Command { .\build\Release\rpk.exe pack "D:\Games\Example" -o "D:\Bench\with-dedup.rpk" --preset balanced }
Measure-Command { .\build\Release\rpk.exe pack "D:\Games\Example" -o "D:\Bench\without-dedup.rpk" --preset balanced --no-dedup }
Measure-Command { .\build\Release\rpk.exe test "D:\Bench\with-dedup.rpk" }
Get-Item "D:\Bench\with-dedup.rpk", "D:\Bench\without-dedup.rpk" | Select-Object Name,Length
```

Record source bytes, archive bytes, build time, verify/extract time, peak RAM, CPU, preset, dictionary, block size, and temporary disk usage. Calculate ratio as `source bytes / archive bytes` and saved space as `1 - archive bytes / source bytes`.

## Tests

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The integration test packs a tree with repeated content, Unicode filenames, empty files/directories, and a read-only file; it checks that deduplication actually reduces stored raw block bytes, extracts the archive, compares every file byte-for-byte and by SHA-256, and rejects a corrupted payload.

## Current limitations

- The block-level compressor uses bounded parallel workers. File scanning, content-defined chunking, deduplication lookup, and final archive assembly remain sequential.
- Type classification selects a lossless codec heuristic; it is not a learned optimizer and does not guarantee the smallest result for every group.
- No gzip/raw-DEFLATE precompression, brotli, neural compression, or AI optimizer yet.
- No multi-volume splitting, password/encryption, installer stub, setup wizard, optional components, pause/resume/cancel, post-install actions, or uninstaller registration yet.
- No preservation of ACLs, ownership, timestamps, alternate data streams, hardlink relationships, or sparse-file layout. Symlinks and non-regular filesystem objects are rejected.
- The GUI is a native Win32 application with a modern card-based workspace, themed action buttons, and clearer operation status. It is compiled and tested by the Windows x64 GitHub Actions workflow. The progress indicator is indeterminate and safe cancellation is not implemented yet.
- A >4 GiB end-to-end stress test has not been completed in this environment, although file sizes, offsets, and extents are 64-bit.

## Milestone roadmap

1. **M1 — Versioned container + solid LZMA2 + CLI extract:** complete.
2. **M2 — Scanner + content-defined chunking + cross-file dedup + initial Windows builder GUI:** implemented; Windows-native build/runtime verification remains.
3. **M3 — Conservative embedded zlib precompression with exact round-trip tests:** implemented with intentionally narrow matching rules.
4. **M4 — Type-aware codec groups, Zstandard long-range, parallel blocks, memory-budget scheduler:** implemented in this version.
5. M5 — Multi-volume archives and installer-oriented archive APIs.
6. M6 — Windows installer stub/setup wizard, component selection, verification, progress controls, and uninstaller registration.
7. M7 — Expanded builder GUI, JSON projects, size/time estimates, cancellation, and controlled post-install actions.
8. M-AI-1 through M-AI-5 — Offline model loader and grammar-constrained decisions, sample-verified strategy advisor, classifier/script generator, log explainer, and optional neural mode.

M4 is implemented in this source release. M5 and later milestones remain unimplemented.

## Get a ready-to-run Windows ZIP without compiling locally

For a prebuilt Windows package, upload this project to a private GitHub repository and run the included **Build Windows Release** workflow under the Actions tab. The workflow builds on a hosted Windows 2022 runner, runs tests, and uploads `RPK-Windows-x64.zip`. Extract it and launch `rpk_gui.exe`; the CLI is `rpk.exe`. The ZIP also contains `rpk_setup.exe` (SFX stub) and `rpk_ai.exe` (optional local-model advisor). Detailed steps are in `BUILD_ON_GITHUB.md`.

The workspace used to prepare this source release was Linux-only, so no native Windows binary is falsely represented as prebuilt or verified here. The GitHub Actions workflow is the provided route for generating genuine Windows executables without compiling on your PC.
