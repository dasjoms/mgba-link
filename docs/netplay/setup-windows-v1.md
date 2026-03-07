# Windows Setup Guide (Link Net v1)

This guide provides copy-pasteable install and smoke-check steps for clean Windows machines running Link Net v1 in three common roles.

- Relay protocol and behavior reference: `docs/netplay/protocol-v1.md`
- Scenario-based validation matrix: `docs/netplay/validation-v1.md`

## Automation Scripts (PowerShell)

For repeatable setup on developer machines, use scripts in `scripts/windows/` from repository root:

```powershell
# Relay only
powershell -ExecutionPolicy Bypass -File .\scripts\windows\bootstrap-relay.ps1

# Client (MSYS2 + mGBA Qt) only
powershell -ExecutionPolicy Bypass -File .\scripts\windows\bootstrap-client-msys2.ps1

# Combined relay + client setup
powershell -ExecutionPolicy Bypass -File .\scripts\windows\bootstrap-all.ps1
```

Optional flags for all three scripts:

- `-WhatIf`: dry-run; prints actions without executing commands.
- `-SkipBuild`: performs verification/setup only and skips compile steps.

Predictable outputs:

- Relay binary: `out/relay/relay.exe`
- mGBA build tree: `out/mgba/build` (Qt binary expected at `out/mgba/build/mgba-qt.exe`)

## Server Hoster (relay-only)

### Prerequisites

- Windows 10/11 with PowerShell 5.1+.
- Administrator rights for first-time package install.
- TCP inbound firewall allowance for relay port `41000`.

### Exact install commands

#### PowerShell

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
winget install --id Git.Git -e --source winget
winget install --id GoLang.Go -e --source winget

$env:Path = "C:\Program Files\Git\cmd;C:\Program Files\Go\bin;$env:Path"

git clone https://github.com/mgba-emu/mgba.git
cd mgba\server\relay
go build -o relay.exe .
```

#### MSYS2 MinGW 64-bit shell

```bash
pacman -Sy --needed --noconfirm git mingw-w64-x86_64-go
export PATH="/mingw64/bin:$PATH"

git clone https://github.com/mgba-emu/mgba.git
cd mgba/server/relay
go build -o relay.exe .
```

### Expected output / artifacts

- `relay.exe` exists in `mgba\server\relay`.
- `go build` exits with status `0` and no compile errors.

### Smoke-check commands

#### PowerShell

```powershell
cd mgba\server\relay
.\relay.exe --help
.\relay.exe --bind 0.0.0.0 --port 41000 --secret my-secret
```

Expected smoke signals:

- Help output includes relay flags such as `--bind`, `--port`, `--secret`.
- Startup logs indicate listener is active on `0.0.0.0:41000`.

#### MSYS2 MinGW 64-bit shell

```bash
cd mgba/server/relay
./relay.exe --help
./relay.exe --bind 0.0.0.0 --port 41000 --secret my-secret
```

### Common failure cases

- `winget` not found: install App Installer from Microsoft Store, then re-run PowerShell steps.
- `go: command not found` or `go version` fails: re-open shell so PATH refreshes, or prepend `C:\Program Files\Go\bin`.
- `bind: Only one usage of each socket address`: another process already uses `41000`; pick a different `--port`.
- Clients can’t connect from WAN/LAN: open inbound TCP `41000` in Windows Defender Firewall and upstream router/NAT.

## Emulator Client User (Qt client-only)

### Prerequisites

- Windows 10/11 with PowerShell 5.1+.
- A legal local GBA ROM.
- Reachability to relay host and port (default `41000`).

### Exact install commands

#### PowerShell (prebuilt mGBA release)

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force

$release = Invoke-RestMethod https://api.github.com/repos/mgba-emu/mgba/releases/latest
$asset = $release.assets |
  Where-Object { $_.name -match 'win64' -and $_.name -match '\.zip$' } |
  Select-Object -First 1
if (-not $asset) { throw 'No win64 .zip release asset found.' }

New-Item -ItemType Directory -Force -Path C:\mgba | Out-Null
$zip = "C:\mgba\$($asset.name)"
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip
Expand-Archive -Path $zip -DestinationPath C:\mgba -Force
```

#### MSYS2 shell variant (launch prebuilt from MSYS2 terminal)

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
$release = Invoke-RestMethod https://api.github.com/repos/mgba-emu/mgba/releases/latest;
$asset = $release.assets | Where-Object { $_.name -match 'win64' -and $_.name -match '\\.zip$' } | Select-Object -First 1;
if (-not $asset) { throw 'No win64 .zip release asset found.' };
New-Item -ItemType Directory -Force -Path C:\\mgba | Out-Null;
$zip = 'C:\\mgba\\' + $asset.name;
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip;
Expand-Archive -Path $zip -DestinationPath C:\\mgba -Force
"
```

### Expected output / artifacts

- `C:\mgba\...\mgba-qt.exe` is present (inside extracted release folder).
- Release archive remains at `C:\mgba\*.zip`.

### Smoke-check commands

#### PowerShell

```powershell
$exe = Get-ChildItem C:\mgba -Filter mgba-qt.exe -Recurse | Select-Object -First 1 -ExpandProperty FullName
& $exe --version
Start-Process $exe
```

Expected smoke signals:

- `--version` prints an mGBA version string.
- Qt UI opens successfully.

#### MSYS2 MinGW 64-bit shell

```bash
EXE=$(find /c/mgba -iname 'mgba-qt.exe' | head -n 1)
"$EXE" --version
"$EXE"
```

### Common failure cases

- `Invoke-RestMethod` GitHub API rate limit hit: retry later or use an authenticated GitHub token in headers.
- No `win64 .zip` asset found: release may be repackaged; download manually from the release page and extract to `C:\mgba`.
- `mgba-qt.exe` missing after extract: verify archive extraction path; re-run `Expand-Archive -Force`.
- Black/blank render window: update graphics drivers and ensure OpenGL support is available.

## Server Host & Client Installation

### Prerequisites

- Same prerequisites as both prior sections.
- Recommended: at least 2 CPU cores and 4 GB RAM so relay and Qt client run together cleanly.

### Exact install commands

#### PowerShell (single-machine combined setup)

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
winget install --id Git.Git -e --source winget
winget install --id GoLang.Go -e --source winget
$env:Path = "C:\Program Files\Git\cmd;C:\Program Files\Go\bin;$env:Path"

git clone https://github.com/mgba-emu/mgba.git
cd mgba\server\relay
go build -o relay.exe .

$release = Invoke-RestMethod https://api.github.com/repos/mgba-emu/mgba/releases/latest
$asset = $release.assets |
  Where-Object { $_.name -match 'win64' -and $_.name -match '\.zip$' } |
  Select-Object -First 1
if (-not $asset) { throw 'No win64 .zip release asset found.' }

New-Item -ItemType Directory -Force -Path C:\mgba | Out-Null
$zip = "C:\mgba\$($asset.name)"
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip
Expand-Archive -Path $zip -DestinationPath C:\mgba -Force
```

#### MSYS2 MinGW 64-bit shell (relay build + Qt launch)

```bash
pacman -Sy --needed --noconfirm git mingw-w64-x86_64-go
export PATH="/mingw64/bin:$PATH"

git clone https://github.com/mgba-emu/mgba.git
cd mgba/server/relay
go build -o relay.exe .
./relay.exe --bind 127.0.0.1 --port 41000 --secret my-secret
```

(Then launch `mgba-qt.exe` from the PowerShell install path `C:\mgba\...`.)

### Expected output / artifacts

- Relay binary: `mgba\server\relay\relay.exe`.
- Client binary: `C:\mgba\...\mgba-qt.exe`.
- Optional relay log file if started with redirection (for example, `relay.log`).

### Smoke-check commands

#### PowerShell (two terminals)

Terminal 1 (relay):

```powershell
cd mgba\server\relay
.\relay.exe --bind 127.0.0.1 --port 41000 --secret my-secret
```

Terminal 2 (client):

```powershell
$exe = Get-ChildItem C:\mgba -Filter mgba-qt.exe -Recurse | Select-Object -First 1 -ExpandProperty FullName
& $exe --version
Start-Process $exe
```

Expected smoke signals:

- Relay remains running and listening on `127.0.0.1:41000`.
- Qt client opens and can target `127.0.0.1` / port `41000` in netplay UI.

### Common failure cases

- `git clone` fails due to TLS/proxy interception: configure corporate proxy or use a pre-cloned internal mirror.
- Relay starts but client cannot connect on localhost: verify client relay host is `127.0.0.1`, not an external hostname.
- `secret` mismatch between host and client token causes join rejection: ensure both use same shared secret/token.
- Antivirus quarantines downloaded archive/binary: restore file and add local exception for trusted mGBA path.
