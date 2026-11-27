# Blackvolt

Mejora de video en tiempo real para Windows con GPUs NVIDIA RTX: súper resolución,
HDR, generación de fotogramas y estadísticas detalladas, en una app rápida y limpia.

> Estado: **0.1.0 en desarrollo.**

## Qué hace

- **Reproductor** de archivos locales con decodificación por hardware (D3D12 Video), presentación
  sin copias y VRR / G-SYNC.
- **PlayStation Remote Play (PS5 y PS4)** basado en chiaki-ng: descubre consolas en la red, las
  despierta, empareja con PIN y transmite hasta 1080p60 en HEVC/H.264 con DualSense (gatillos,
  vibración, giroscopio). **Importa los emparejamientos de Veyra y de chiaki-ng** para no volver a
  emparejar; las credenciales se guardan cifradas con DPAPI.
- **RTX Video Super Resolution y TrueHDR** por la vía oficial de NGX, en el reproductor y en el
  stream, con vuelta automática a bilineal / SDR si la GPU o el driver no lo soportan.
- **Estadísticas**: tiempos por etapa (decodificación, latencia, GPU por cada mejora), fotogramas
  perdidos y recuperados, bitrate, RTT.

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
- CMake 3.26+ y vcpkg (ambos vienen con Visual Studio; abre la "Developer PowerShell" para tener `VCPKG_ROOT`).
- .NET 9 SDK.
- GPU con DirectX 12. Para RTX Video, una NVIDIA GeForce RTX 20 o superior con driver reciente.

## Compilar y ejecutar

```powershell
# 0. SDK de NVIDIA y runtimes (una vez)
./scripts/fetch-sdks.ps1

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
2. **Runtimes DLSS/NGX oficiales.** RTX VSR, TrueHDR y DLSS, cargados por la vía oficial de NGX y verificados por hash. Sin parches en memoria ni suplantar la GPU ni otras aplicaciones. DLSS NR queda preparado para cuando NVIDIA lo publique oficialmente.
3. **Motor y UI separados.** La app habla con el motor solo por la API en C.
4. **Medir todo.** Cada etapa del pipeline reporta su tiempo; las estadísticas son parte del producto.

## Licencia

Blackvolt es software libre bajo la licencia **AGPL-3.0** (ver [LICENSE](LICENSE)).
Los runtimes de NVIDIA incluidos en los Releases conservan sus propias licencias: ver [NOTICE.md](NOTICE.md).
