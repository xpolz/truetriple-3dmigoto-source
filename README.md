# TrueTriple for DiRT Rally 2.0 — GPLv3 Corresponding Source

TrueTriple ships a `d3d11.dll` that is a **modified build of [3Dmigoto](https://github.com/bo3b/3Dmigoto)**
(bo3b, DarkStarSword, Flugan and contributors), which is licensed under the GNU General Public
License v3. This folder is the corresponding source for that binary, provided under the same
license.

## What the modifications do

3Dmigoto on its own cannot render three independent camera frustums. These changes add that:
they read the game's camera matrix out of its constant buffers, rebuild an off-axis projection
per monitor, and redraw the scene once per region. They also add the in-game overlay used to
show and save seat positions.

## Base version

Everything here is a diff against **[bo3b/3Dmigoto](https://github.com/bo3b/3Dmigoto) at commit
`4ce5f2f72b2777223d2e809dcdafec514ac98295`**. Eight files under `DirectX11/` are modified;
nothing else in the upstream tree is touched. In total: 3977 lines added, 9 removed.

| File | What was changed |
|---|---|
| `CommandList.cpp` / `.h` | The new `.ini` commands that do the triple-frustum math, per-region scissor, and camera-matrix patching |
| `HackerContext.cpp` / `.h` | Captures the game's camera matrix and index buffers; tracks the scissor rects the game sets, so shadow passes can be corrected per region |
| `Overlay.cpp` / `.h` | The on-screen panel: reads its text from an external file so it can be translated and show the user's real key binds |
| `IniHandler.cpp` | Build fix for modern toolsets — the original relies on a header that newer MSVC no longer pulls in transitively |
| `profiling.cpp` | Dumps 3Dmigoto's internal performance counters to the log, to measure the cost of drawing three times |

## How to build

Two equivalent routes. The patch is the reliable one.

**With the patch:**

```
git clone https://github.com/bo3b/3Dmigoto.git
cd 3Dmigoto
git checkout 4ce5f2f72b2777223d2e809dcdafec514ac98295
git apply /path/to/truetriple.patch
```

**Or by hand:** copy the files from `src/DirectX11/` over `DirectX11/` in a clone at that commit.

Then build `StereovisionHacks.sln` in **Release x64**, toolset **v145**, SDK **10.0.22621.0**:

```
MSBuild.exe "StereovisionHacks.sln" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0.22621.0 /m
```

Two things that will otherwise cost you an hour:

- **Build Release, not Debug.** A Debug build requires the D3D11 debug layer; without it
  installed the game will not start at all with that DLL.
- **The post-build step fails if the project path contains spaces.** This is a known quirk of
  3Dmigoto's own post-build script, not of these changes. It is harmless — the DLL is produced
  anyway, only the automatic copy step fails. Copy the `.dll` out of the output folder yourself.

## What is not in this folder

- **The upstream 3Dmigoto tree** — it is public, and pinning the exact commit above is what makes
  this diff reproducible. Nothing about it was modified beyond the eight files here.
- **The configuration and shader patches** (`d3dx.ini` template, `ShaderFixes/`) — these are data
  files that 3Dmigoto reads at runtime. They are not linked into the DLL and are not derived from
  3Dmigoto's source, so the GPL does not reach them.
- **The configuration app** — a separate Rust/SolidJS program that writes those data files and
  launches the game. It does not link against 3Dmigoto and shares no code with it.

If you think something here is missing to build a working DLL, say so — that would be a
compliance gap and it would get fixed.

## License

GPLv3, full text in [`LICENSE`](LICENSE). Upstream authorship in
[`AUTHORS-3Dmigoto.txt`](AUTHORS-3Dmigoto.txt); the modifications are by the TrueTriple author.
