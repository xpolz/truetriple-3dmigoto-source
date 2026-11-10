# DiRT Rally (DR1)

Fuente modificada de 3Dmigoto del mod TrueTriple para **DiRT Rally (2015)**, ejecutable de 32 bits.

- Versión del mod: 1.5.3 (app suelta, 5/9/2026) y 1.6.0 (paquete del launcher, 10/11/2026).
- `d3d11.dll` (x86, 3.101.184 bytes), SHA-256
  `ee23d9dfc7173fa688feca962928f7694ddda97fd61af419e32522af07052c70` (la anterior, `f876672f4ddccca6a93c7f99f879899511e09b43b90a0adc591cfac08efddb9f`, es la etiqueta `dr1-dll-f876672f`).
- Fuente exacta: etiqueta `dr1-dll-ee23d9df`.
- Contenido: `DirectX11/` (los 8 archivos completos), `truetriple.patch` (el mismo cambio
  contra el commit base `4ce5f2f72b2777223d2e809dcdafec514ac98295`) y `CAMBIOS.md`.

(El cambio `cb2010e` de `IniHandler.cpp`, el eco del `.ini` al log a `LogDebug`, ya está acá y en la dll `ee23d9df`.)

## Compilar

Igual que DR2 pero **Win32**: toolset v145, SDK 10.0.22621.0, `Release|Win32`.

```
git clone -c core.autocrlf=false https://github.com/bo3b/3Dmigoto.git
cd 3Dmigoto
git checkout 4ce5f2f72b2777223d2e809dcdafec514ac98295
git apply <este-repo>/dirt-rally1/truetriple.patch
MSBuild.exe "StereovisionHacks.sln" /t:DirectX11 /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0.22621.0 /m
MSBuild.exe "StereovisionHacks.sln" /t:NVAPI /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0.22621.0 /m
```

La salida queda en `builds\x32\Release\`. Un dll x64 en `drt.exe` no carga y no da ningún error.

## dll `b1d6e9aa` (10/11/2026): la marca de sesión

Agrega `TrueTripleMarca.h` y el enganche en `IniHandler.cpp` (`LoadConfigFile`): sin una marca
fresca de la app en `%LOCALAPPDATA%\TrueTriple\sesion-dr1.json` el
dll no carga el `d3dx.ini` y queda en reposo. SHA-256 de la dll: `b1d6e9aaab71630bd2b4c0e4f69396ba943e4f6181e6f73117ef0248a111a0da`. Etiqueta
`dr1-dll-b1d6e9aa`. La anterior (`ee23d9df…`) sigue en su etiqueta.
