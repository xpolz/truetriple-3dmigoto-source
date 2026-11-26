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
| `DirectX11/CommandList.cpp` | 3919 / 0 | Comandos nuevos del `.ini` (`triple_region`, `triple_scissor`, `triple_params`, `triple_restore`) y la matemática por monitor: proyección off-axis desde la proyección real del draw call y el scissor por región. Lectura y apertura de los planos del frustum (por CPU y por constant buffer) y un hook a la función del juego que arma el frustum, para que no recorte objetos de los monitores laterales. Detector de replay de solo lectura sobre el estado interno del juego (versión 1.18.130.9032; si no reconoce la firma, no hace nada). Una corrida automática de capturas para ensayar el culling, inerte si el `.ini` no la pide. Nuevo en 1.7.2: la variante del terreno lejano (media caja orientada con la cámara en vez del cubo alineado al mundo, con el plano cercano original: `$cull_variante`), el parche del HUD del juego para que el OSD no se achique con la ventana ancha, y las lecturas de variables del `.ini` por copia bajo lock. |
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

## dll `486d34f7` (10/11/2026): la marca de sesión

Agrega `TrueTripleMarca.h` y el enganche en `IniHandler.cpp` (`LoadConfigFile`): sin una marca
fresca de la app en `%LOCALAPPDATA%\TrueTriple\sesion-dr2.json` el
dll no carga el `d3dx.ini` y queda en reposo. SHA-256 de la dll: `486d34f70bbe0adc3686953796a50a05ad12a5b2cabb93762eb69e4a6de0f9cb`. Etiqueta
`dr2-dll-486d34f7`. La anterior (`d07fc901…`) sigue en su etiqueta.

## dll `ed02623f` (16/11/2026): el log ya no repite el ini

`IniHandler.cpp`, `CommandList.cpp`, `Hunting.cpp` y, nuevos en esta carpeta, `Override.cpp`, `input.cpp` y
`ShaderRegex.cpp`: todo lo que repetía contenido del `d3dx.ini` en `d3d11_log.txt` pasó de
`LogInfo` a `LogDebug` (clave=valor, nombres de sección, teclas, variables, ciclos de los `[Key]`,
cuerpos de ShaderRegex, líneas de command list, "Optimised out", "Statically evaluated",
"Creating cached resource"). Con `debug=0` el log conserva versión, avisos y errores; con
`debug=1` sale todo. Compilada con `LINK=/PDBALTPATH:%_PDB%`. SHA-256 de la dll:
`ed02623fc5e7b499b504603f58432c7181e6e2ac09b353b782e5150990dff951`; `nvapi64.dll`: `d11b884829145a5b7a48ddbec7acbbc7af3020b250032932eb31275bdf4a5acf`. Etiqueta `dr2-dll-ed02623f`.

## dll `4836ed0a` (16/11/2026): la marca de sesión va firmada

`TrueTripleMarca.h` pasa a la versión 2: la marca `%LOCALAPPDATA%\TrueTriple\sesion-dr2.json`
lleva una firma ed25519 y la huella de la PC, y la cabecera las verifica con la clave pública
que trae adentro (ed25519, SHA-512 y SHA-256 de TweetNaCl, de dominio público), además de la
frescura de siempre. `TrueTripleReposo.h` (nuevo en esta carpeta) recibe el ini de reposo
embebido y `SesionUsaUserConfig`, que estaban en la cabecera anterior. `IniHandler.cpp`: el
punto de lectura llama a `truetriple::MarcaVerificarLocal("dr2", ...)` y el log dice el
motivo del reposo (`TrueTriple: sin sesion (<motivo>), el mod queda en reposo [<detalle>]`) o
`TrueTriple: sesion abierta (ok)`. Compilada con `LINK=/PDBALTPATH:%_PDB%`. SHA-256 de la dll:
`4836ed0a1ae88be91a8c7b35846adc95c598f8e46ed7b6edd83d63386b280daf`; `nvapi64.dll`: `cc7746c4787dde26ac3e5608c223adca5f1dfddf030ab97da63957400b7272a8`. Etiqueta `dr2-dll-4836ed0a`.

## dll `076f6282` (18/11/2026): un solo monitor captura la cámara

`CommandList.cpp`, `TripleCaptureCameraCB`: con `$mono = 1` (opción "Un solo monitor") la
cámara principal es la que tiene el aspecto de la pantalla (ancho/alto del swapchain, con 3%
de margen) en vez del aspecto de triple (3.6 a 12). Sin eso no se capturaba ninguna cámara
con un monitor: el estabilizador no recibía cabeceo, rolido ni altura y los pases de pantalla
completa no se corregían. Con `$mono = 0` el comportamiento es el de antes. Compilada con
`LINK=/PDBALTPATH:%_PDB%`. SHA-256 de la dll: `076f6282abebaa34aca12398b2823e84d7a932767ea0b58e18c037dcc9c6ea3e`; `nvapi64.dll`: `ae4218ef417a2ae3fd15069a0415d44c60d50827416852e870c6e841799e6b96`. Etiqueta `dr2-dll-076f6282`.

## dll `fc84c6e3` (25/11/2026): versión de prueba de rendimiento (AMD)

Para una versión de prueba de DR2 (canal prueba, sobre la 1.9.18), no para la estable. Tres
opciones que la app prende con casillas: llegan en una línea de comentario
`#tt-prueba: liviano raster redibujo` de `TrueTriple.menu.txt` y las lee `IniHandler.cpp`
(`ttprueba::Leer`, solo con la sesión abierta); sin la línea, todo como la `076f6282`.
`liviano` (`HackerContext.cpp`, `CommandList.cpp`): sin los diagnósticos viejos de
`Map`/`Unmap`/`UpdateSubresource`, las variables del ini que se leían con el candado global en
cada constant buffer copiadas una vez por cuadro, y el prólogo de `TripleCaptureCameraCB` una
vez por cuadro. `raster` (`CommandList.cpp`): el custom shader con `rasterizer_state_merge`
reusa el estado combinado (caché por hilo) en vez de llamar a `CreateRasterizerState` en cada
corrida. `redibujo` (`CommandList.cpp`): `[CustomShaderTripleRedraw]` no guarda ni repone
render targets, UAVs ni viewports, `triple_params` no bindea al domain shader sin domain
shader y `triple_scissor` mide el render target una vez por objeto. Además
(`TrueTripleDiag.h`, `HackerDXGI.cpp`, compilada con `CL=/DTT_DIAG_INSTRUMENTAR=1`): una línea
de medición cada 10 s en `TrueTriple_diag.txt` y en `d3d11_log.txt`, y una vez la placa y
`D3D11_FEATURE_THREADING`. Compilada con `LINK=/PDBALTPATH:%_PDB%`. SHA-256 de la dll:
`fc84c6e316c42b8b37e77f5776beb2bf17c4e802636daa58a478440fb4224c46`; `nvapi64.dll` sin cambios.
Etiqueta `dr2-dll-fc84c6e3`.

## dll `382889c3` (26/11/2026): las opciones de rendimiento pasan a la versión normal

Sobre la `fc84c6e3` de la versión de prueba (que con las tres opciones llevó una RX 9070 XT de
69 a 106 FPS con el triple). Las mismas tres opciones (`liviano`, `raster`, `redibujo`), ahora en
`TrueTripleRendimiento.h` (espacio `ttrend`, antes `TrueTriplePrueba.h`/`ttprueba`), que llegan
en la línea `#tt-rendimiento: liviano raster redibujo` de `TrueTriple.menu.txt` (la escribe el
plugin con lo que el usuario eligió en la app; `IniHandler.cpp`, solo con la sesión abierta); sin
la línea, todo como la `076f6282`. La medición (`TrueTripleDiag.h`) queda compilada por defecto
(`TT_DIAG_INSTRUMENTAR` vale 1 sin pasarlo por `CL`) y su línea cada 10 s va solo al
`d3d11_log.txt` (ya no escribe `TrueTriple_diag.txt`). Compilada con
`LINK=/PDBALTPATH:%_PDB%`. SHA-256 de la dll:
`382889c337591d97c9add11d434c48004febbaaa076b0e968928ce41f442a418`; `nvapi64.dll` sin cambios.
Etiqueta `dr2-dll-382889c3`.
