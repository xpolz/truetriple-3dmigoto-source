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
| [`dirt-rally2/`](dirt-rally2/) | DiRT Rally 2.0 | paquete del launcher 1.9.16 (dll del 18/11/2026: con un solo monitor captura la cámara con el aspecto de la pantalla) | `076f6282abebaa34aca12398b2823e84d7a932767ea0b58e18c037dcc9c6ea3e` | etiqueta `dr2-dll-076f6282` (anteriores en la tabla de abajo) |
| [`dirt-rally1/`](dirt-rally1/) | DiRT Rally (x86) | paquete del launcher 1.7.12 (dll del 16/11/2026 con la marca de sesión firmada: `TrueTripleMarca.h` v2 verifica una firma ed25519 y `TrueTripleReposo.h` es nuevo) | `314b989ef17c7f5c0f0fb33174eaff1ea46575130ff6cae0de85600bafb95dcb` | etiqueta `dr1-dll-314b989e` (anteriores en la tabla de abajo) |
| [`wreckfest/`](wreckfest/) | Wreckfest | paquete del launcher 0.2.11 (dll del 16/11/2026 con la marca de sesión firmada: `TrueTripleMarca.h` v2 verifica una firma ed25519 y `TrueTripleReposo.h` es nuevo) | `3dd73520a1484f2e2333c2a4dbe7068a145b562a64c1aa8bb6301df6e61c7fad` | etiqueta `wreckfest-dll-3dd73520` (anteriores en [`wreckfest/README.md`](wreckfest/README.md)) |

**La fuente exacta de cada dll que se distribuyó** es una etiqueta de este repo; `main`
avanza con lo que va a compilar la próxima dll. Todas las dll de DiRT Rally 2.0 entregadas
hasta hoy:

| SHA-256 del `d3d11.dll` | Con qué se entregó | Fuente exacta |
|---|---|---|
| `fc84c6e3…4c46` | versión de prueba de DR2 sobre la 1.9.18 (25/11/2026, canal de prueba, rama `prueba/dr2-amd-casillas` de este repo): tres opciones de rendimiento que se prenden desde la app (`TrueTriplePrueba.h`, `CommandList.cpp`, `HackerContext.cpp`, `IniHandler.cpp`) y una línea de medición cada 10 s (`TrueTripleDiag.h`, `HackerDXGI.cpp`); `nvapi64.dll` sin cambios (`ae4218ef…6b96`) | etiqueta `dr2-dll-fc84c6e3` |
| `076f6282…ea3e` | paquete del launcher 1.9.14 (18/11/2026, canal de prueba) y 1.9.16 en adelante (estable): con un solo monitor (`$mono = 1`) la cámara principal se captura con el aspecto de la pantalla (`CommandList.cpp`); `nvapi64.dll` `ae4218ef…6b96` | etiqueta `dr2-dll-076f6282` |
| `4836ed0a…0daf` | paquetes del launcher 1.9.12 a 1.9.13 (16/11/2026): marca de sesión firmada (`TrueTripleMarca.h` v2, `TrueTripleReposo.h` nuevo e `IniHandler.cpp`); `nvapi64.dll` `cc7746c4…72a8` | etiqueta `dr2-dll-4836ed0a` |
| `ed02623f…f951` | paquete del launcher 1.9.11 (16/11/2026): el eco del `d3dx.ini` al log va con `LogDebug`, y la dll ya no lleva la ruta del PDB (`IniHandler.cpp`, `CommandList.cpp`, `Hunting.cpp` y, nuevos acá, `Override.cpp`, `input.cpp` y `ShaderRegex.cpp`); `nvapi64.dll` `d11b8848…5acf` | etiqueta `dr2-dll-ed02623f` |
| `eb261ec8…d145` | paquetes del launcher 1.9.2 a 1.9.10 (11/11/2026): con sesión pero sin variables persist en el ini, no lee ni guarda `d3dx_user.ini`; `nvapi64.dll` `7d7eeefa…fe75` | etiqueta `dr2-dll-eb261ec8` |
| `854516be…7b02` | paquete del launcher 1.9.1 (11/11/2026): en reposo no lee ni guarda `d3dx_user.ini` (antes lo vaciaba al salir y borraba los presets de asiento); `nvapi64.dll` `e7ddc8e1…9321` | etiqueta `dr2-dll-854516be` |
| `486d34f7…f9cb` | paquetes del launcher 1.8.9 a 1.9.0 (10/11/2026): marca de sesión (`TrueTripleMarca.h`); `nvapi64.dll` `dc4b96a8…5f46` | etiqueta `dr2-dll-486d34f7` |
| `d07fc901…9421` | app 1.7.2 (instaladores del 8/10 y 24/10/2026) y paquetes del launcher 1.8.2 a 1.8.8 | etiqueta `dr2-dll-d07fc901` |
| `69d760b0…5017` | paquetes del launcher 1.8.0 y 1.8.1 (retirados el 8/11/2026) | etiqueta `dr2-dll-69d760b0` |
| anteriores a 1.7.0 | instaladores 1.5.x y 1.6.x | pendiente de ubicar (se agregan acá a medida que se encuentran) |

Todas las dll de DiRT Rally (DR1, x86) entregadas hasta hoy:

| SHA-256 del `d3d11.dll` | Con qué se entregó | Fuente exacta |
|---|---|---|
| `314b989e…5dcb` | paquete del launcher 1.7.12 (16/11/2026) en adelante: marca de sesión firmada (`TrueTripleMarca.h` v2, `TrueTripleReposo.h` nuevo e `IniHandler.cpp`); `nvapi.dll` `8ee053b2…6d09` | etiqueta `dr1-dll-314b989e` |
| `1065e51a…ef0b` | paquete del launcher 1.7.11 (16/11/2026): el eco del `d3dx.ini` al log va con `LogDebug`, y la dll ya no lleva la ruta del PDB (lo que faltaba: `CommandList.cpp`, `Override.cpp`, `input.cpp`, `ShaderRegex.cpp` y el resto de `IniHandler.cpp`); `nvapi.dll` `3daebb8d…5490` | etiqueta `dr1-dll-1065e51a` |
| `165accf8…66a7` | paquetes del launcher 1.7.3 a 1.7.10 (11/11/2026): con sesión pero sin variables persist en el ini, no lee ni guarda `d3dx_user.ini`; `nvapi.dll` `ef4b3fd1…8b72` | etiqueta `dr1-dll-165accf8` |
| `3b15cb3e…c725` | paquete del launcher 1.7.2 (11/11/2026): en reposo no lee ni guarda `d3dx_user.ini`; `nvapi.dll` `9f437cf7…9766` | etiqueta `dr1-dll-3b15cb3e` |
| `f7c969bc…239b` | paquete del launcher 1.7.1 (10/11/2026): fuera el diagnóstico de la niebla retirada; `nvapi.dll` `18bd4d54…af32` | etiqueta `dr1-dll-f7c969bc` |
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

Desde las dll del 16/11/2026 se compila con la variable de entorno `LINK=/PDBALTPATH:%_PDB%`
(link.exe la suma a sus opciones): la dll guarda solo el nombre del PDB y no la ruta de la PC
donde se compiló. No cambia el código.

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

## El log ya no repite el ini (16/11/2026)

Desde las dll `ed02623f` (DR2), `1065e51a` (DR1) y `d74a6908` (Wreckfest), todo lo que repetía contenido
del `d3dx.ini` en `d3d11_log.txt` (clave=valor, nombres de sección, teclas, variables, ciclos de
los `[Key]`, cuerpos de ShaderRegex, líneas de command list, "Optimised out", "Statically
evaluated", "Creating cached resource") pasó de `LogInfo` a `LogDebug`. Con `[Logging] calls=1`
y `debug=0` el log conserva la versión, los avisos y los errores; con `debug=1` sale todo como en
3Dmigoto original.

## La marca de sesión firmada (16/11/2026)

Desde las dll `4836ed0a` (DR2), `314b989e` (DR1) y `3dd73520` (Wreckfest), `TrueTripleMarca.h`
es la versión 2: la marca de sesión va firmada con ed25519 y atada a la PC. La cabecera trae
el verificador entero (ed25519, SHA-512 y SHA-256 de TweetNaCl, de dominio público) y la clave
**pública**; el dll compara además la huella de la PC y la frescura, y deja en el log el motivo
cuando queda en reposo (`TrueTriple: sin sesion (<motivo>), ...`). `TrueTripleReposo.h` (nuevo)
lleva el ini de reposo embebido, que antes estaba en la misma cabecera. Todo eso es parte de la
fuente GPL publicada acá. La clave privada con que la app firma no es parte del dll ni de esta
fuente: con lo publicado acá se compila un dll idéntico en comportamiento al distribuido.
