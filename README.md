# Fuente modificada de 3Dmigoto para los mods TrueTriple

Los mods TrueTriple que funcionan con 3Dmigoto distribuyen un `d3d11.dll` que es
**[3Dmigoto](https://github.com/bo3b/3Dmigoto)** (bo3b, DarkStarSword, Flugan y
colaboradores) compilado desde fuente **modificada**. 3Dmigoto se publica bajo la
**GNU GPL v3**, así que esa fuente modificada se publica acá, bajo la misma licencia.

Este repo contiene **solo eso**: los archivos de 3Dmigoto que se modificaron o se
agregaron, uno por mod. La app de configuración, los `.ini` y los `ShaderFixes/` no
están: son programas o datos aparte que no se enlazan con 3Dmigoto.

## Mods

| Carpeta | Juego | Versión del mod | SHA-256 del `d3d11.dll` distribuido | Fuente exacta |
|---|---|---|---|---|
| [`dirt-rally2/`](dirt-rally2/) | DiRT Rally 2.0 | paquete del launcher 1.8.9 (dll del 10/11/2026, con la marca de sesión) | `486d34f70bbe0adc3686953796a50a05ad12a5b2cabb93762eb69e4a6de0f9cb` | etiqueta `dr2-dll-d07fc901` (commit `1284106`) |
| [`dirt-rally1/`](dirt-rally1/) | DiRT Rally (x86) | paquete del launcher 1.7.1 (dll del 10/11/2026, sin el diagnóstico de la niebla) | `f7c969bce5782fab8887cd8988fb167aeba0539f5a35a548e496236214c4239b` | etiqueta `dr1-dll-f7c969bc` (anteriores en la tabla de abajo |

**La fuente exacta de cada dll que se distribuyó** es una etiqueta de este repo; `main`
avanza con lo que va a compilar la próxima dll. Todas las dll de DiRT Rally 2.0 entregadas
hasta hoy:

| SHA-256 del `d3d11.dll` | Con qué se entregó | Fuente exacta |
|---|---|---|
| `486d34f7…f9cb` | paquete del launcher 1.8.9 (10/11/2026) en adelante: marca de sesión (`TrueTripleMarca.h`); `nvapi64.dll` `dc4b96a8…5f46` | etiqueta `dr2-dll-486d34f7` |
| `d07fc901…9421` | app 1.7.2 (instaladores del 8/10 y 24/10/2026) y paquetes del launcher 1.8.2 a 1.8.8 | etiqueta `dr2-dll-d07fc901` |
| `69d760b0…5017` | paquetes del launcher 1.8.0 y 1.8.1 (retirados el 8/11/2026) | etiqueta `dr2-dll-69d760b0` |
| anteriores a 1.7.0 | instaladores 1.5.x y 1.6.x | pendiente de ubicar (se agregan acá a medida que se encuentran) |

Todas las dll de DiRT Rally (DR1, x86) entregadas hasta hoy:

| SHA-256 del `d3d11.dll` | Con qué se entregó | Fuente exacta |
|---|---|---|
| `f7c969bc…239b` | paquete del launcher 1.7.1 (10/11/2026) en adelante: fuera el diagnóstico de la niebla retirada; `nvapi.dll` `18bd4d54…af32` | etiqueta `dr1-dll-f7c969bc` |
| `b1d6e9aa…a0da` | paquetes del launcher 1.6.2 a 1.7.0 (10/11/2026): marca de sesión (`TrueTripleMarca.h`); `nvapi.dll` `75bedde1…5c9e` | etiqueta `dr1-dll-b1d6e9aa` |
| `ee23d9df…52c70` | paquete del launcher 1.6.1 (10/11/2026); `nvapi.dll` `35727ae4…bc6fb` | etiqueta `dr1-dll-ee23d9df` |
| `f876672f…ddb9f` | app suelta 1.5.3 (5/9/2026) y paquete del launcher 1.6.0 | etiqueta `dr1-dll-f876672f` |

Nota DR1: el cambio `cb2010e` del repo del mod (10/10/2026, `IniHandler.cpp`: el eco del `.ini` al
log pasa a `LogDebug`) entró en la dll `ee23d9df` compilada el 10/11/2026 (etiqueta `dr1-dll-ee23d9df`).

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
(los nombres son los mismos que en 3Dmigoto, `profiling.cpp` incluido).

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

## `nvapi.dll` y `d3dcompiler_47.dll`

El `nvapi.dll` que acompaña al `d3d11.dll` sale **de esta misma fuente**: se compila con el
proyecto `NVAPI` del árbol de 3Dmigoto (GPLv3, sin modificaciones nuestras) y enlaza las
librerías `nvapi.lib` / `nvapi64.lib` de NVIDIA, que se publican bajo licencia **MIT**. El texto
de esa licencia, copiado textual de `ThirdPartyLibs/nvapi-lib/nvapi-main/License.txt` del árbol de
3Dmigoto, está en [`LICENSE.NVAPI.txt`](LICENSE.NVAPI.txt). Para compilarlo, mismo comando que el
`d3d11.dll` pero con `/t:NVAPI`.

El `d3dcompiler_47.dll` **no es nuestro ni de 3Dmigoto**: es de Microsoft, redistribuible del
Windows SDK (`Redist\D3D\x86` o `x64`). No está cubierto por la GPL y no se publica acá.

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

## La marca de sesión (10/11/2026)

Desde las dll `486d34f7` (DR2) y `b1d6e9aa` (DR1), `TrueTripleMarca.h` (en cada `DirectX11/`)
hace que el dll lea una vez por proceso `%LOCALAPPDATA%\TrueTriple\sesion-<juego>.json`, que la
app escribe al Jugar (vale 2 horas, se renueva cada 10 minutos, se borra al cerrar). Sin marca
fresca el dll no carga el `d3dx.ini` de la carpeta y queda con el ini inerte embebido. Es parte
de la fuente GPL publicada acá, como todo lo demás.
