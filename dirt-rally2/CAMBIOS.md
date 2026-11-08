# DiRT Rally 2.0: cambios respecto de 3Dmigoto

Base: [bo3b/3Dmigoto](https://github.com/bo3b/3Dmigoto) en `4ce5f2f72b2777223d2e809dcdafec514ac98295`.
Fuente del `d3d11.dll` de TrueTriple para DiRT Rally 2.0, versión 1.7.0 (fuente al 8/11/2026),
SHA-256 del dll `69d760b041f8171f93c22927205ab851eb89bf93e68bb988bbb2e22531e45017`.

Generado con un diff contra el commit base; `truetriple.patch` es ese diff y aplica limpio
con `git apply`. En total: 17 archivos, 11 modificados y 6 nuevos, 7656 líneas agregadas
y 39 quitadas (11 de esas son espacios al final de línea que el editor limpió en
`D3D11Wrapper.cpp`).

## Archivos modificados

| Archivo | + / - | Qué cambió |
|---|---|---|
| `DirectX11/CommandList.cpp` | 3721 / 0 | Comandos nuevos del `.ini` (`triple_region`, `triple_scissor`, `triple_params`, `triple_restore`) y la matemática por monitor: proyección off-axis desde la proyección real del draw call y el scissor por región. Lectura y apertura de los planos del frustum (por CPU y por constant buffer) y un hook a la función del juego que arma el frustum, para que no recorte objetos de los monitores laterales. Detector de replay de solo lectura sobre el estado interno del juego (versión 1.18.130.9032; si no reconoce la firma, no hace nada). Una corrida automática de capturas para ensayar el culling, inerte si el `.ini` no la pide. |
| `DirectX11/CommandList.h` | 85 / 0 | Declaraciones de lo anterior. |
| `DirectX11/HackerContext.cpp` | 1411 / 1 | Captura la matriz de cámara del juego interceptando `Map`/`Unmap` del constant buffer y copia los scissor rects que pone el juego para restaurarlos después de cada pasada por región. |
| `DirectX11/HackerContext.h` | 13 / 0 | Declaraciones de lo anterior. |
| `DirectX11/Overlay.cpp` | 1565 / 1 | El cartel en pantalla del mod: lee su texto de `TrueTriple.menu.txt` y los nombres de auto de `TrueTriple.autos.txt` (archivos junto al dll), para poder traducirlo y mostrar las teclas reales. |
| `DirectX11/Overlay.h` | 10 / 0 | Declaraciones de lo anterior. |
| `DirectX11/D3D11Wrapper.cpp` | 37 / 11 | Incluye `TrueTripleTaaLock.cpp` y `TrueTripleCmaaLock.cpp` y, si el `.ini` pide `$force_native_aa`, los instala antes de crear el dispositivo; si fallan, el juego se cierra con un mensaje. El resto son espacios al final de línea. |
| `DirectX11/HackerDXGI.cpp` | 7 / 0 | Llama en cada `Present` al paso de la corrida automática de capturas (inerte si no está pedida). |
| `DirectX11/Hunting.cpp` | 2 / 0 | Al recargar solo shaders, vuelve a aplicar los reemplazos por regex. |
| `DirectX11/IniHandler.cpp` | 48 / 26 | Arreglo de compilación para v145 (se quitó `<experimental/filesystem>`, que no se usaba). El eco del `.ini` al log pasa de `LogInfo` a `LogDebug`, para que el log que mandan los usuarios no lleve el `.ini` entero. Opción `repeat = N` en las secciones `[Key]`: repetir la acción N veces por segundo con la tecla apretada (0 = como antes). |
| `DirectX11/profiling.cpp` (en este repo `Profiling.cpp`) | 7 / 0 | Vuelca el monitor de rendimiento interno al `d3d11_log.txt`. |

## Archivos nuevos

| Archivo | Líneas | Qué es |
|---|---|---|
| `DirectX11/TrueTripleTaaLock.cpp` / `.h` | 222 / 30 | Bloquea el TAA del juego (solo con el ejecutable 1.18.130.9032, verificado por timestamp), porque con varias regiones por cuadro rompe la imagen. |
| `DirectX11/TrueTripleCmaaLock.cpp` / `.h` | 246 / 30 | Fuerza CMAA como antialias en su lugar, con la misma verificación. |
| `DirectX11/OrthoFrustumProbe.h` | 56 | Diagnóstico: escala una caja ortográfica de frustum sin cambiar sus normales. |
| `DirectX11/VisibilityProbe.h` | 166 | Diagnóstico pasivo de las consultas de visibilidad del juego; no escribe nada. |

