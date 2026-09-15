# Local AI Agent

A lightweight native C++ AI assistant for Windows that talks to **local AI models you
already have installed** (Ollama or any OpenAI-compatible server like LM Studio). It never
downloads or bundles models itself.

## Features

- Pure native Win32 + GDI+ dark UI (no ImGui, no web view, no framework).
- Auto-detects your local backend: **Ollama** (port 11434) or **LM Studio / OpenAI-compatible** (port 1234).
- Lists only the models already installed on your machine — pick one and chat.
- Streaming not required: single lightweight WinHTTP call per turn, runs off the UI thread.
- Includes a desktop / start menu shortcut installer (`.msi`) and a portable `.exe`.

## How to run

Prerequisites: install [Ollama](https://ollama.com) (and pull a model, e.g. `ollama pull qwen2.5:0.5b`)
or [LM Studio](https://lmstudio.ai) with a model loaded, then launch the app.

```powershell
# portable
.\dist\LocalAIAgent.exe

# installed via MSI
Start-Process "C:\Program Files\LocalAIAgent\LocalAIAgent.exe"   # or Start Menu shortcut "Local AI Agent"
```

## Headless self-test

The binary includes a `--selftest` mode that verifies backend detection and a real chat turn:

```powershell
.\LocalAIAgent.exe --selftest              # auto-detect
.\LocalAIAgent.exe --selftest ollama
.\LocalAIAgent.exe --selftest openai       # LM Studio / OpenAI-compatible
```

Exits `0` on success.

## Building from source

Requires Visual Studio Build Tools 2022 / MSVC, CMake 3.15+, and the WiX Toolset for the MSI.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# MSI (WiX 3.14 binaries in tools\wix)
.\tools\wix\candle.exe installer.wxs -out build\installer.wixobj
.\tools\wix\light.exe build\installer.wixobj -out "build\Local AI Agent.msi" -ext WixUIExtension.dll
```

CI (`.github/workflows/build.yml`) reproduces this on a `windows-latest` runner and uploads both
the `.exe` and the `.msi` as build artifacts.

## Layout

| Path | Purpose |
| ---- | ------- |
| `src/main.cpp` | Win32 UI, backend selector, `--selftest` |
| `src/ai_client.cpp` | Ollama + OpenAI-compatible transport (WinHTTP) |
| `src/json.cpp` | Tiny dependency-free JSON parser |
| `installer.wxs` | WiX installer definition |
| `assets/app.ico` | Application icon |

## License

MIT