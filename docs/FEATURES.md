# Mejoras de video

Cadena del presentador (reproductor, captura y Remote Play comparten la misma):

```
fuente (SDR, HDR10, HLG) ─► RGBA8 ─► [reducción de ruido] ─► [DLSS NR + antiparpadeo] ─► [color / FXAA]
   ─► [interpolación] ─► [RTX VSR | NIS] ─► [TrueHDR] ─► pantalla (+ comparación A/B, capturas, subtítulos)
```

| Etapa | Qué hace | GPU | Código | Prueba |
|---|---|---|---|---|
| Reducción de ruido | Bilateral 5×5 que respeta bordes, luma y croma; Suave / Media / Fuerte | Cualquiera D3D12 | `enhance/Denoise` | `test_denoise` (GPU/WARP): fuerte quita ~60 % del ruido sin mover el borde |
| DLSS NR | Feature 18 de NVIDIA, como Veyra (ver [DLSS_NR.md](DLSS_NR.md)) | RTX 20–50 | `enhance/NeuralRendering`, `ngx/DlssNrSession` | Compila con/sin NGX; requiere la DLL para ejecutarse |
| Antiparpadeo NR | Memoria temporal del cambio de NR: en zonas quietas, o siguiendo el movimiento (historial reproyectado con el flujo óptico de la entrada NR) | RTX | `CSTemporal` en `NeuralRendering` | `motion_estimate_points_to_the_other_frame` (dirección y magnitud del vector) |
| Color y FXAA | Brillo, contraste, saturación, intensidad, temperatura, tinte, gamma; antialiasing FXAA para capturas de juegos | Cualquiera D3D12 | `enhance/PostFx` | `test_postfx`: identidad exacta, gris perfecto con saturación 0, FXAA suaviza el borde sin tocar zonas planas |
| HDR10 / HLG | Decodificación PQ/HLG y BT.2020→BT.709; en pantalla HDR se muestra en HDR (scRGB), si no (o con mejoras SDR activas) tone mapping con el pico real del contenido (MaxCLL / metadatos de masterización) | Cualquiera | shader del presentador | Fórmulas verificadas contra ST 2084 / BT.2100 / BT.2408 |
| Interpolación («Fluidez de movimiento») | Flujo óptico jerárquico propio (búsqueda 1/16 → 1/4, subpíxel, mediana) + warp bidireccional con detección de oclusiones. ×2/×3/×4, limitado a la frecuencia de la pantalla, sin latencia añadida (usa el fotograma ya decodificado) | Cualquiera D3D12 | `enhance/FrameInterpolation`, `Player::renderLoop` | `test_interpolation`: error 0,5–2,6/255 frente a 1,3–35 de un fundido; ~0,6 ms a 720p en una RTX 4060 (0,1 ms los fotogramas extra) |
| RTX Video Super Resolution | IA de NVIDIA al tamaño de la ventana | RTX | `ngx/VideoSuperResolution` | — |
| NVIDIA Image Scaling | Escalado adaptativo a bordes + nitidez (1×–2×); se usa solo cuando VSR no está disponible o si se elige | Cualquiera D3D12 | `enhance/NisScaler`, `third_party/nis` (MIT) | `test_nis`: 2× más fiel que bilineal (7,5 frente a 8,8/255) |
| TrueHDR | SDR → HDR | RTX + HDR en Windows | `ngx/TrueHdr` | — |
| Comparación A/B | Original a la izquierda, mejorado a la derecha (tecla C) | — | final del `draw()` | — |
| Capturas | PNG de lo que se ve (S en el reproductor, F12 en Remote Play), HDR convertido a SDR | — | `D3D12Presenter::screenshot` | — |

Otras funciones:

- **Subtítulos**: pistas de texto incrustadas (SRT, ASS, mov_text, WebVTT), archivos externos `.srt/.vtt/.ass` (carga automática del archivo con el mismo nombre), estilo (tamaño, fondo, posición), tecla V. Pruebas: `ass_events_become_plain_text` y el lector de archivos.
- **Pistas de audio**: cambio de idioma (reabre en la misma posición).
- **Velocidad 0,25×–4×** sin cambiar el tono (WSOLA propio, `audio/TimeStretch`), teclas ↑/↓. Prueba `test_timestretch`: duración exacta y 440 Hz a todas las velocidades.
- **Pantalla completa** (F11, F, doble clic), **retomar la última fuente** al abrir el reproductor (archivo en su posición o la captura).

- **Exportar** aplica la reducción de ruido, DLSS NR y el color del reproductor (`export/GpuEnhancer`).
- **Captura de pantalla y ventanas** (`screen:desktop?region=…`, `screen:title=…`, FFmpeg gdigrab), además de capturadoras y cámaras.
- **Presets propios**: guardan todos los ajustes de mejora con nombre.
- **Estadísticas**: overlay con FPS, 1 % bajo, gráfica de tiempos de fotograma y de GPU, coste por etapa, fotogramas generados y VRAM; página de Estadísticas con gráfica en vivo.
- **Runtimes DLSS NR**: descarga e instalación automáticas desde RankFTW/rhi-repo, el mismo origen que Veyra (solo el DLL, con espejos y el paquete de Veyra como respaldo), versión elegible por ranura, verificación por CRC/SHA-256.
- **Instalador** (Inno Setup) y actualizador con espejos.

Las pruebas de GPU (`test_interpolation`, `test_denoise`, `test_nis`, `test_postfx`) usan la GPU si hay
una y, si no (CI), el rasterizador WARP de Windows.
