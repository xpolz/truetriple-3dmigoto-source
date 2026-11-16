# Wreckfest

Fuente modificada de 3Dmigoto del mod TrueTriple para **Wreckfest** (ejecutable de 64 bits, `Wreckfest_x64.exe`).

- `d3d11.dll` (x64), SHA-256 `d74a6908d6a2abb06eae6f45db8bcc76115273971816baf848b10d2c5cb19bb4`.
- Fuente exacta: etiqueta `wreckfest-dll-d74a6908`.
- Paquete del launcher 0.2.10 (16/11/2026) en adelante: el eco del `d3dx.ini` al log va con `LogDebug`, y la dll ya no lleva la ruta del PDB. `nvapi64.dll` distribuido: `e56391badc1c3c283f40509d70bdfefb918caeac9f1a48c2b9aa9ea13a34e33d` (sin cambios desde la app suelta).

| SHA-256 del `d3d11.dll` | Con qué se entregó | Fuente exacta |
|---|---|---|
| `d74a6908…9bb4` | paquete del launcher 0.2.10 en adelante | etiqueta `wreckfest-dll-d74a6908` |
| `881ad15d…c8b5` | paquetes del launcher 0.2.3 a 0.2.9 | etiqueta `wreckfest-dll-881ad15d` |
| `63b42a12…fa6d` | paquete del launcher 0.2.2 | etiqueta `wreckfest-dll-63b42a12` |
| `0a987102…1fd4` | paquete del launcher 0.2.1 | etiqueta `wreckfest-dll-0a987102` |
| `b50cded6…7845` | app suelta 0.1.4 y paquete 0.2.0 | etiqueta `wreckfest-dll-b50cded6` |
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
