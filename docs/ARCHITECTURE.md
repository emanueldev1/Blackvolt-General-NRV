# Arquitectura

## Vista general

```
┌──────────────────────────── Blackvolt.App (WinUI 3, C#) ────────────────────────────┐
│  Inicio · Reproductor · Captura · Exportar · Estadísticas · Ajustes                  │
│  Interop/NativeEngine.cs  ── P/Invoke ──┐                                            │
└─────────────────────────────────────────┼────────────────────────────────────────────┘
                                          │  API en C estable (bv_api.h)
┌─────────────────────────────── blackvolt_engine.dll (C++20) ─────────────────────────┐
│  Source ──► Decode ──► Enhance stages ──► Present / Encode                            │
│  (archivo,   (NVDEC /   (DLSS NR, RTX VSR,  (swapchain flip+VRR,                       │
│   captura,    D3D12      TrueHDR, DLSS-G)    NVENC)                                    │
│   pantalla)   Video)                                                                   │
│                                                                                        │
│  gpu/       D3D12Context: device + colas direct/compute/copy                           │
│  telemetry/ RollingStats por etapa: promedio, p50, p95, p99, máximo                    │
│  core/      Engine, Log                                                                │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

## Decisiones

- **DLL nativa + API en C.** La UI no ve tipos C++; se puede reemplazar o probar el motor sin la UI.
  Las excepciones nunca cruzan la frontera (`guarded()` en `api.cpp`).
- **Tres colas D3D12.** Decodificación/copia, mejora (compute) y presentación pueden solaparse en la GPU.
- **Pipeline por etapas con interfaz fija.** Cada etapa recibe y entrega texturas D3D12 del pool del motor.
  Añadir un efecto nuevo = añadir una etapa, sin tocar las demás.
- **Telemetría siempre activa.** Ventana de 1024 muestras por etapa, sin asignar memoria al grabar.
  La UI la consulta a 2 Hz; el historial de sesiones se guardará en SQLite (v0.5).
- **Selección de GPU.** RTX primero, luego NVIDIA, luego discretas, desempate por VRAM (`gpu/AdapterRanking`).
  Las capacidades se infieren por identidad y se confirman en tiempo de ejecución al cargar cada SDK.

## Runtime de NVIDIA (DLSS / NGX)

Blackvolt usa los runtimes NGX de NVIDIA, cargados en tiempo de ejecución desde `runtime/`:

| DLL | Función | Origen |
|---|---|---|
| `nvngx_dlssnr.dll` | DLSS NR (Neural Rendering), `enhance/NeuralRendering` + `ngx/DlssNrSession` | Paquete NVIDIA de Veyra (descarga automática) |
| `nvngx_dlssg.dll` | DLSS Frame Generation | DLSS SDK (oficial) |
| `nvngx_vsr.dll` | RTX Video Super Resolution | RTX Video SDK (oficial) |
| `nvngx_truehdr.dll` | RTX Video TrueHDR | RTX Video SDK (oficial) |
| `nvofapi64.dll` | Optical flow por hardware | Driver de NVIDIA |

- El cargador NGX (`engine/src/ngx/`) es propio, escrito contra los headers públicos del DLSS SDK.
- **Las DLLs no se suben a Git** (pesan 7 a 276 MB; GitHub rechaza archivos de más de 100 MB).
  Van dentro del ZIP / instalador de cada Release. `runtime/manifest.json` fija nombre, versión
  y SHA-256 de cada una; el script de empaquetado y la app verifican esos hashes.
- **RTX 20/30:** las builds de la comunidad (DLL de NVIDIA modificadas) no se redistribuyen.
  La app ofrece descargarlas desde su origen, verificando el hash, a petición del usuario.
- **DLSS NR** se carga como Veyra: `nvngx_dlssnr.dll` directamente (exporta su propio Init_Ext/
  CreateFeature/Evaluate), con el App ID del snippet firmado y un shim en *su* entrada de importación
  de `GetModuleFileNameW` (responde `nvngx.dll` solo para el módulo de Blackvolt). En RTX 30, con builds
  que lo necesitan, la consulta de arquitectura de NVAPI se responde como Blackwell solo para la GPU
  elegida y solo desde el módulo NR. Ver [DLSS_NR.md](DLSS_NR.md).

## Otras dependencias (no se suben al repo)

| SDK | Uso | Licencia |
|---|---|---|
| NVIDIA DLSS SDK (headers) | Interfaz NGX | Licencia NVIDIA RTX SDK |
| NVIDIA Video Codec SDK | NVDEC / NVENC | Licencia de NVIDIA |
| FFmpeg (build LGPL) | Demux de contenedores | LGPL 2.1, enlace dinámico |
| Windows App SDK | UI WinUI 3 | MIT |

Los SDKs se descargan localmente en `third_party/sdk/` (ignorado por git).
