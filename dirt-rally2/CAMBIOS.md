# DiRT Rally 2.0: cambios respecto de 3Dmigoto

Base: [bo3b/3Dmigoto](https://github.com/bo3b/3Dmigoto) en `4ce5f2f72b2777223d2e809dcdafec514ac98295`.
Fuente del `d3d11.dll` de TrueTriple para DiRT Rally 2.0, versión 1.7.2 (la dll compilada el
8/10/2026 y distribuida con la app 1.7.2; fuente publicada el 8/11/2026),
SHA-256 del dll `d07fc90116a195e17dea47ca8104b82c79dfce1ef7da669e58c980fbaeac9421`.

Generado con un diff contra el commit base; `truetriple.patch` es ese diff y aplica limpio
con `git apply`. En total: 20 archivos, 11 modificados y 9 nuevos, 8330 líneas agregadas
y 20 quitadas (11 de esas son espacios al final de línea que el editor limpió en
`D3D11Wrapper.cpp`).

Nota: la versión anterior de esta carpeta (publicada como "1.7.0") traía el eco del `.ini` al log
con `LogDebug` en `IniHandler.cpp` (cambio del 10/10/2026). Esa dll no es la que se distribuye;
la 1.7.2 de arriba conserva `LogInfo` ahí.

Nota 2 (8/11/2026): en `CommandList.cpp`, la carpeta de las capturas de la corrida automática
de culling (`TripleAutoCapturaTick`) pasó de una ruta fija de la PC del autor a
`%LOCALAPPDATA%\TrueTriple\capturas-auto`. Es el único cambio respecto de la fuente con que se
compiló la dll D07FC901; la próxima dll que se compile de esta fuente ya lo trae.

## Archivos modificados

| Archivo | + / - | Qué cambió |
|---|---|---|
| `DirectX11/CommandList.cpp` | 3906 / 0 | Comandos nuevos del `.ini` (`triple_region`, `triple_scissor`, `triple_params`, `triple_restore`) y la matemática por monitor: proyección off-axis desde la proyección real del draw call y el scissor por región. Lectura y apertura de los planos del frustum (por CPU y por constant buffer) y un hook a la función del juego que arma el frustum, para que no recorte objetos de los monitores laterales. Detector de replay de solo lectura sobre el estado interno del juego (versión 1.18.130.9032; si no reconoce la firma, no hace nada). Una corrida automática de capturas para ensayar el culling, inerte si el `.ini` no la pide. Nuevo en 1.7.2: la variante del terreno lejano (media caja orientada con la cámara en vez del cubo alineado al mundo, con el plano cercano original: `$cull_variante`), el parche del HUD del juego para que el OSD no se achique con la ventana ancha, y las lecturas de variables del `.ini` por copia bajo lock. |
| `DirectX11/CommandList.h` | 96 / 0 | Declaraciones de lo anterior, más `TripleReadIniGlobal`/`TripleWriteIniGlobal` (copia bajo lock de una variable del `.ini`, porque la recarga del `.ini` vacía el mapa y un puntero guardado quedaba colgado) y `TripleInstallHudLayout`. |
| `DirectX11/HackerContext.cpp` | 1402 / 1 | Captura la matriz de cámara del juego interceptando `Map`/`Unmap` del constant buffer y copia los scissor rects que pone el juego para restaurarlos después de cada pasada por región. |
| `DirectX11/HackerContext.h` | 13 / 0 | Declaraciones de lo anterior. |
| `DirectX11/Overlay.cpp` | 1565 / 1 | El cartel en pantalla del mod: lee su texto de `TrueTriple.menu.txt` y los nombres de auto de `TrueTriple.autos.txt` (archivos junto al dll), para poder traducirlo y mostrar las teclas reales. |
| `DirectX11/Overlay.h` | 10 / 0 | Declaraciones de lo anterior. |
| `DirectX11/D3D11Wrapper.cpp` | 45 / 11 | Incluye `TrueTripleTaaLock.cpp`, `TrueTripleCmaaLock.cpp`, `TrueTripleHudLayout.h` y `TrueTripleShadowFit.h`. Instala el parche del HUD antes de crear el dispositivo y, si el `.ini` pide `$force_taa_off` o `$force_cmaa_on`, los bloqueos de antialias (cada uno con su interruptor; si CMAA falla y el TAA se había instalado acá, se deshace). El resto son espacios al final de línea. |
| `DirectX11/HackerDXGI.cpp` | 7 / 0 | Llama en cada `Present` al paso de la corrida automática de capturas (inerte si no está pedida). |
| `DirectX11/Hunting.cpp` | 2 / 0 | Al recargar solo shaders, vuelve a aplicar los reemplazos por regex. |
| `DirectX11/IniHandler.cpp` | 22 / 7 | Arreglo de compilación para v145 (se quitó `<experimental/filesystem>`, que no se usaba). Opción `repeat = N` en las secciones `[Key]`: repetir la acción N veces por segundo con la tecla apretada (0 = como antes). |
| `DirectX11/profiling.cpp` | 7 / 0 | Vuelca el monitor de rendimiento interno al `d3d11_log.txt`. |

## Archivos nuevos

| Archivo | Líneas | Qué es |
|---|---|---|
| `DirectX11/TrueTripleTaaLock.cpp` / `.h` | 222 / 30 | Bloquea el TAA del juego (solo con el ejecutable 1.18.130.9032, verificado por timestamp), porque con varias regiones por cuadro rompe la imagen. |
| `DirectX11/TrueTripleCmaaLock.cpp` / `.h` | 246 / 30 | Fuerza CMAA como antialias en su lugar, con la misma verificación. |
| `DirectX11/OrthoFrustumProbe.h` | 56 | Diagnóstico: escala una caja ortográfica de frustum sin cambiar sus normales. |
| `DirectX11/VisibilityProbe.h` | 166 | Diagnóstico pasivo de las consultas de visibilidad del juego; no escribe nada. |
| `DirectX11/TrueTripleHudLayout.h` | 118 | Parche del HUD (solo con el ejecutable 1.18.130.9032): cambia únicamente la consulta del escalador del OSD, no el estado Eyefinity global (que achica los menús y cambia la cámara). |
| `DirectX11/TrueTripleShadowFit.h` | 144 | El ajuste de las cascadas de sombra lee la relación de aspecto de la cámara; se le da una copia local de solo lectura para esa llamada, sin tocar la cámara compartida (pasto y objetos conservan la vista ancha). |
| `DirectX11/TrueTripleShadowTrace.h` | 230 | Diagnóstico opcional de las sombras: copia entradas, no edita estado ni datos de shaders. |

