# Fuente modificada de 3Dmigoto para los mods TrueTriple

Los mods TrueTriple que funcionan con 3Dmigoto distribuyen un `d3d11.dll` que es
**[3Dmigoto](https://github.com/bo3b/3Dmigoto)** (bo3b, DarkStarSword, Flugan y
colaboradores) compilado desde fuente **modificada**. 3Dmigoto se publica bajo la
**GNU GPL v3**, así que esa fuente modificada se publica acá, bajo la misma licencia.

Este repo contiene **solo eso**: los archivos de 3Dmigoto que se modificaron o se
agregaron, uno por mod. La app de configuración, los `.ini` y los `ShaderFixes/` no
están: son programas o datos aparte que no se enlazan con 3Dmigoto.

## Mods

| Carpeta | Juego | Versión del mod | SHA-256 del `d3d11.dll` distribuido |
|---|---|---|---|
| [`dirt-rally2/`](dirt-rally2/) | DiRT Rally 2.0 | 1.7.0 (fuente al 8/11/2026) | `69d760b041f8171f93c22927205ab851eb89bf93e68bb988bbb2e22531e45017` |
| [`dirt-rally1/`](dirt-rally1/) | DiRT Rally | pendiente | pendiente |

Cada carpeta trae:

- `DirectX11/...`: los archivos completos, en la misma ruta relativa que en 3Dmigoto.
- `truetriple.patch`: el mismo cambio como parche contra el commit base.
- `CAMBIOS.md`: qué archivo se tocó y para qué.

## Commit base

Todo es un cambio sobre **[bo3b/3Dmigoto](https://github.com/bo3b/3Dmigoto) en el commit
`4ce5f2f72b2777223d2e809dcdafec514ac98295`** ("CI Build - 1.4.9", 19/2/2026). El resto
del árbol de 3Dmigoto no se modifica.

## Cómo compilar

Hace falta Windows con Visual Studio (toolset **v145**) y el SDK **10.0.22621.0**.

```
git clone -c core.autocrlf=false https://github.com/bo3b/3Dmigoto.git
cd 3Dmigoto
git checkout 4ce5f2f72b2777223d2e809dcdafec514ac98295
git apply <este-repo>/dirt-rally2/truetriple.patch
```

O a mano: copiar `dirt-rally2/DirectX11/*` sobre `DirectX11/` del clon en ese commit
(`Profiling.cpp` reemplaza a `profiling.cpp`; en Windows es el mismo archivo).

Después, en la raíz del clon:

```
MSBuild.exe "StereovisionHacks.sln" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0.22621.0 /m
```

- **Release, no Debug.** La build Debug exige la capa de depuración de D3D11; sin ella
  el juego no arranca con ese dll.
- **Si la ruta del clon tiene espacios, falla el paso de post-build** (es del script de
  3Dmigoto). El dll se genera igual; copialo a mano desde la carpeta de salida.
- No hace falta tocar el `.vcxproj`: los `.cpp` nuevos se incluyen desde
  `D3D11Wrapper.cpp`.

Si falta algo para compilar un dll que funcione, avisá: sería una falta de cumplimiento
y se corrige.

## Licencia

GPL v3, texto completo en [`LICENSE`](LICENSE) (igual a `LICENSE.GPL.txt` de 3Dmigoto).
[`COPYING.txt`](COPYING.txt) es el aviso original de 3Dmigoto y
[`AUTHORS-3Dmigoto.txt`](AUTHORS-3Dmigoto.txt) sus autores. Las modificaciones son del
autor de TrueTriple.

---

## English

Modified [3Dmigoto](https://github.com/bo3b/3Dmigoto) source (GPLv3) used by the
TrueTriple mods that run on 3Dmigoto. Each folder holds the files changed or added for
one mod, plus a patch against upstream commit `4ce5f2f72b2777223d2e809dcdafec514ac98295`
and a `CAMBIOS.md` (changelog). The table above maps each folder to the SHA-256 of the
`d3d11.dll` that was shipped. To build: clone bo3b/3Dmigoto, check out that commit,
`git apply` the folder's `truetriple.patch`, and build `StereovisionHacks.sln` in
Release x64 (toolset v145, SDK 10.0.22621.0). The configuration app, the `.ini` files
and `ShaderFixes/` are separate programs or data and are not part of this repo.
