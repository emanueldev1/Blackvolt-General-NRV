# DLSS NR

Implementación equivalente a la de Veyra 2.2.1 (`src/ngx/DlssNrRuntimeAdapter.cpp`,
`src/pipeline/EnhanceGraph.cpp`, `shaders/NrResidualComposite.hlsl`), adaptada a la cadena RGBA8 del
presentador de Blackvolt.

## Cadena por fotograma

```
YUV ─► RGBA8 (sRGB) ─► reducción a resolución interna (área exacta, luz lineal)
                         │  ── se envía la lista: NR exige una lista de comandos nueva ──
                         ▼
                    DLSS NR (feature 18, movimiento y profundidad a cero)
                         ▼
   original + (NR − entrada NR) reescalado guiado por bordes
   × cantidad de cambio (total, oscurecer, aclarar, color, luminosidad)
   → corrección Oklab opcional (tono, saturación, neutros, color/luminosidad, sombras, luces, compresión)
   → zonas protegidas (subtítulos, HUD)                                  ─► RTX VSR ─► TrueHDR ─► pantalla
```

Tiempos de GPU por etapa con timestamps: conversión, entrada NR, evaluación NR, composición, VSR,
TrueHDR y presentación (`bv_enhance_status`, telemetría `gpu.nr`, `gpu.nr.composite`).

## Runtimes

Se descargan del mismo sitio del que los saca Veyra: las releases de
[RankFTW/rhi-repo](https://github.com/RankFTW/rhi-repo/releases) (el espejo de runtimes de RenoDX),
un zip por versión (`nvngx_dlssnr_<versión>.zip`). Comprobado byte a byte: `dlssnr-310.9.16` es el
`runtime/experimental/nr-ampere` de Veyra 2.2 y `dlssnr-310.8.0` su `nr-original`.

| Ranura | Carpeta | Versión por defecto | Otras | GPU |
|---|---|---|---|---|
| Compatible | `runtime/nr/compat/` | `dlssnr-310.9.16` (SF FP16) | `dlssnr-310.8.SF-v2` | RTX 20 · 30 · 40 · 50 |
| NVIDIA original | `runtime/nr/original/` | `dlssnr-310.8.0` | `dlssnr-310.9.16-nvidia` (firmado, más reciente), `dlssnr-310.8.Lecram` | RTX 50 |

- `Services/NrRuntimeInstaller.cs` lee del zip solo el DLL con peticiones HTTP Range explícitas (el CDN de
  GitHub rechaza los rangos de sufijo `bytes=-N`); si un espejo no admite rangos, baja el zip entero.
  Espejos: GitHub, gh-proxy, ghfast. Si rhi-repo no responde, los builds por defecto se sacan del
  paquete NVIDIA portable de Veyra.
- Cada DLL se comprueba con el CRC-32 del zip, la cabecera PE y el **SHA-256 exacto del build elegido**
  (de los locks de Veyra o de las notas de la release) antes de que el motor lo instale (reemplazo atómico).
- Se instala solo en el primer arranque con una GPU RTX, desde el instalador, o en *Runtimes NVIDIA*,
  donde también se elige la versión de cada ranura.
- El modelo B/A (`DLSSNR.Hint.Render.Preset` 2/1) solo existe en el build compatible.
- La ruta RTX 30 (reescritura de arquitectura) no se instala con los builds SF 310.8.2+ y 310.9.16, que
  eligen su ruta a partir de la arquitectura real (`ngx/NrArchitecturePolicy.h`).

## Opciones

Las mismas que el editor de capa NR de Veyra: runtime, modelo de rendimiento, resolución interna
(nativa, 1440p, 1080p, 720p, 480p o porcentaje), intensidad, estilo 0–2, tono local, estructura local,
textura de piel, máscara automática, corrección de interfaz, cantidad de cambio (5 controles),
corrección de imagen automática por estilo o manual (8 controles) y zonas protegidas con borde suave.
Presets: Equilibrado, Natural, Intenso, Rendimiento.

## Antiparpadeo

Pasada propia a la resolución interna de NR (más barata que la de Veyra, que trabaja a tamaño
completo): el cambio de NR se mezcla con el del fotograma anterior con memoria exp(−dt/τ)
(τ = 80 ms suave, 180 ms fuerte, × estabilidad), solo donde la imagen y sus vecinos 3×3 no cambian.
Equivale al nivel «Static» de Veyra y no necesita flujo óptico. Se reinicia en saltos, pausas y
pasos de más de 250 ms.

## Exportar con NR

`export/GpuEnhancer` sube cada fotograma (RGBA8, misma matriz y rango de entrada y salida), ejecuta
la misma etapa NR con antiparpadeo y lo devuelve al codificador. La página Exportar usa los ajustes
de NR del reproductor.

No incluido todavía: niveles de antiparpadeo con flujo óptico, varias capas NR encadenadas y la
reutilización de inferencia entre fotogramas.
