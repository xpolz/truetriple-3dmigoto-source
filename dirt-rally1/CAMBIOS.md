# DiRT Rally (DR1): cambios respecto de 3Dmigoto

Base: [bo3b/3Dmigoto](https://github.com/bo3b/3Dmigoto) en `4ce5f2f72b2777223d2e809dcdafec514ac98295`.
Fuente del `d3d11.dll` de TrueTriple para DiRT Rally (2015), **x86 (Win32)**: la dll compilada el
5/9/2026 y distribuida con la app suelta 1.5.3 y con el paquete del launcher 1.6.0 (10/11/2026),
SHA-256 `f876672f4ddccca6a93c7f99f879899511e09b43b90a0adc591cfac08efddb9f`
(3.101.184 bytes). Etiqueta de este repo: `dr1-dll-f876672f`.

Los 8 archivos de `DirectX11/` son los del commit `d85d471` del repo del mod, el árbol exacto con el
que se compiló esa dll. `truetriple.patch` es el diff de esos 8 archivos contra el commit base y
aplica limpio con `git apply`. En total: 8 archivos modificados, ninguno nuevo, unas 4900 líneas
agregadas y 8 quitadas (buena parte son comentarios que explican cada decisión).

Nota sobre el estado de la fuente: después de compilar esa dll hubo un único cambio de fuente,
`cb2010e` (10/10/2026), en `IniHandler.cpp`: el eco del `.ini` al log pasa de `LogInfo` a
`LogDebug`. **No está en esta carpeta ni en la dll `f876672f`**; se agregará a este repo junto con
la dll nueva, que tendrá su propia etiqueta.

## Archivos modificados

| Archivo | + / - | Qué cambió |
|---|---|---|
| `DirectX11/CommandList.cpp` | 3342 / 0 | Comandos nuevos del `.ini`: `set_scissor`, `triple_region`, `triple_scissor`, `triple_params`, `triple_restore`. Matemática por monitor: se arma la proyección corregida de cada tercio y se reemplaza solo `inverseProj` del constant buffer de cámara, con scissor proporcional al render target. Estado por contexto diferido (`thread_local`). Aviso en pantalla del "ojo virtual". Identificación del auto, detector de repetición y ganchos a la construcción de la proyección y del FOV del motor, todos verificados por firma contra el ejecutable de 32 bits (si no reconoce la firma, no hace nada). Diagnóstico de la niebla en el log. |
| `DirectX11/CommandList.h` | 92 / 0 | Declaraciones de `ScissorCommand`, `TripleRegionCommand`, `TripleParamsCommand` y `TripleRestoreCommand`. |
| `DirectX11/HackerContext.cpp` | 102 / 0 | Captura la matriz de cámara del juego interceptando `Map`/`Unmap` del constant buffer (lado CPU, porque un copy más `Map(READ)` es ilegal en contextos diferidos) y guarda una copia sombra de los scissor rects que pone el juego, para restaurarlos tras cada pasada por región. |
| `DirectX11/HackerContext.h` | 11 / 0 | Declaraciones de lo anterior (`mTripleScissorShadow`). |
| `DirectX11/Overlay.cpp` | 1309 / 1 | El cartel en pantalla del mod: lee su texto de `TrueTriple.menu.txt` y los nombres de auto de `TrueTriple.autos.txt` (archivos junto al dll), y publica el estado de postura y presets que le da el `.ini`. |
| `DirectX11/Overlay.h` | 5 / 0 | Declaraciones de `DrawTripleMenu` y `DrawMenuAutos`. |
| `DirectX11/IniHandler.cpp` | 30 / 7 | Arreglo de compilación para el toolset v145 (se quita `<experimental/filesystem>`, que no se usaba). Opción `repeat = N` en las secciones `[Key]`: repetir la acción N veces por segundo con la tecla apretada (0 = como antes). |
| `DirectX11/profiling.cpp` | 8 / 0 | Vuelca el monitor de rendimiento interno al `d3d11_log.txt`. (En el repo del mod el archivo se llama `Profiling.cpp`; acá lleva el nombre de 3Dmigoto.) |

## Diferencias con la fuente de DiRT Rally 2.0

Es el mismo conjunto de ideas que en `dirt-rally2/` pero para un proceso de 32 bits: se dejó
afuera el código de diagnóstico y de cacería del culling que solo servía en DR2 (x64) y no compila
en x86. La carpeta no trae archivos nuevos: todo vive en archivos que ya existen en 3Dmigoto.
