# DiRT Rally (DR1)

Fuente modificada de 3Dmigoto del mod TrueTriple para **DiRT Rally (2015)**, ejecutable de 32 bits.

- Versión del mod: 1.5.3 (app suelta, 5/9/2026) y 1.6.0 (paquete del launcher, 10/11/2026).
- `d3d11.dll` (x86, 3.101.184 bytes), SHA-256
  `f876672f4ddccca6a93c7f99f879899511e09b43b90a0adc591cfac08efddb9f`.
- Fuente exacta: etiqueta `dr1-dll-f876672f`.
- Contenido: `DirectX11/` (los 8 archivos completos), `truetriple.patch` (el mismo cambio
  contra el commit base `4ce5f2f72b2777223d2e809dcdafec514ac98295`) y `CAMBIOS.md`.

Aviso: hay un cambio de fuente posterior a esa dll (`cb2010e`, `IniHandler.cpp`, el eco del
`.ini` al log pasa a `LogDebug`) que todavía no está acá porque la dll no se recompiló. Cuando
salga la dll nueva se agrega con su propia etiqueta.

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
