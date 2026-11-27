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
│  (archivo,   (NVDEC /   (RTX VSR, TrueHDR,  (swapchain flip+VRR,                       │
│   captura,    D3D12      FRUC, color)        NVENC)                                    │
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

## Dependencias externas (no se suben al repo)

| SDK | Uso | Licencia |
|---|---|---|
| NVIDIA RTX Video SDK | Súper resolución y TrueHDR | Licencia de NVIDIA |
| NVIDIA Optical Flow SDK (FRUC) | Interpolación de fotogramas | Licencia de NVIDIA |
| NVIDIA Video Codec SDK | NVDEC / NVENC | Licencia de NVIDIA |
| FFmpeg (build LGPL) | Demux de contenedores | LGPL 2.1, enlace dinámico |
| Windows App SDK | UI WinUI 3 | MIT |

Los SDKs se descargan localmente en `third_party/sdk/` (ignorado por git).
