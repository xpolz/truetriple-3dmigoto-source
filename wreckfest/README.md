# Wreckfest

Fuente modificada de 3Dmigoto del mod TrueTriple para **Wreckfest** (ejecutable de 64 bits, `Wreckfest_x64.exe`).

- `d3d11.dll` (x64), SHA-256 `63b42a12ea61c0290e92fdf8cef02ae42a7a227a96e218c07bf58f66a470fa6d`.
- Fuente exacta: etiqueta `wreckfest-dll-63b42a12`.
- Paquete del launcher 0.2.2 (11/11/2026) en adelante: en reposo, además de no cargar el `d3dx.ini`, no lee ni guarda `d3dx_user.ini` (la dll anterior lo vaciaba al salir y se perdía el asiento). Las anteriores: `0a987102` (paquete 0.2.1, etiqueta `wreckfest-dll-0a987102`) y `b50cded6` (app suelta 0.1.4 y paquete 0.2.0, etiqueta `wreckfest-dll-b50cded6`).
- Contenido: `DirectX11/` (los archivos modificados, completos) y `truetriple.patch` (el mismo
  cambio contra el commit base `4ce5f2f72b2777223d2e809dcdafec514ac98295` de `bo3b/3Dmigoto`).

## Compilar

Toolset v145, SDK 10.0.22621.0, `Release|x64`.

```
git clone -c core.autocrlf=false https://github.com/bo3b/3Dmigoto.git
cd 3Dmigoto
git checkout 4ce5f2f72b2777223d2e809dcdafec514ac98295
git apply <este-repo>/wreckfest/truetriple.patch
MSBuild.exe "StereovisionHacks.sln" /t:DirectX11 /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0.22621.0 /p:PostBuildEventUseInBuild=false /m
```

La salida queda en `builds\x64\Release\`.
