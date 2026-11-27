# Blackvolt

Mejora de video en tiempo real para Windows con GPUs NVIDIA RTX: DLSS NR, súper resolución,
HDR, generación de fotogramas y estadísticas detalladas, en una app rápida y limpia.

> Estado: **0.1.0 en desarrollo.** La base del motor y la app ya funcionan; el pipeline de
> video llega en las siguientes versiones (ver [ROADMAP](docs/ROADMAP.md)).

## Qué hay en el repo

| Carpeta | Contenido |
|---|---|
| `engine/` | Motor nativo C++20 (DLL). Direct3D 12, detección de GPU, telemetría. API en C: `engine/include/blackvolt/bv_api.h`. |
| `app/Blackvolt.App/` | App de escritorio WinUI 3 (C# / .NET 9). Solo UI; todo el trabajo pesado lo hace el motor. |
| `tests/engine/` | Pruebas unitarias del motor. |
| `docs/` | Arquitectura y roadmap. |

## Requisitos

- Windows 10 21H2+ o Windows 11, x64.
- Visual Studio 2022 o más nuevo, con las cargas **Desarrollo de escritorio con C++** y **Desarrollo de aplicaciones de WinUI / .NET**.
- CMake 3.26+ (viene con Visual Studio).
- .NET 9 SDK.
- GPU con DirectX 12. Para RTX Video, una NVIDIA GeForce RTX 20 o superior con driver reciente.

## Compilar y ejecutar

```powershell
# 1. Motor (C++)
cmake --preset windows-x64
cmake --build --preset debug
ctest --preset debug

# 2. App (C#), copia blackvolt_engine.dll automáticamente
dotnet build app/Blackvolt.App -c Debug
dotnet run --project app/Blackvolt.App -c Debug
```

También puedes abrir `Blackvolt.sln` en Visual Studio después de compilar el motor.

## Principios

1. **Todo en la GPU.** Los fotogramas no vuelven a la CPU entre decodificar, mejorar y mostrar.
2. **Runtimes DLSS/NGX.** DLSS NR, DLSS-G, RTX VSR y TrueHDR, incluidos en cada Release y verificados por hash. Sin parches en memoria ni suplantar la GPU.
3. **Motor y UI separados.** La app habla con el motor solo por la API en C.
4. **Medir todo.** Cada etapa del pipeline reporta su tiempo; las estadísticas son parte del producto.
