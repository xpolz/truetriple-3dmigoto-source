# Wreckfest

Fuente modificada de 3Dmigoto del mod TrueTriple para **Wreckfest** (ejecutable de 64 bits, `Wreckfest_x64.exe`).

- `d3d11.dll` (x64), SHA-256 `b50cded63b73c99fce06fbd9e2feb2fada83c9a8d47c69953540cd93167a7845`.
- Fuente exacta: etiqueta `wreckfest-dll-b50cded6`.
- Se entregó con la app suelta 0.1.4 y con el paquete del launcher 0.2.0 (10/11/2026).
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
