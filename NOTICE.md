# Avisos de terceros

Blackvolt se distribuye bajo **AGPL-3.0** (ver `LICENSE`). Los componentes siguientes
conservan sus propias licencias y **no** quedan cubiertos por la AGPL.

## NVIDIA DLSS SDK y runtimes NGX

Los paquetes de Release incluyen DLLs de NVIDIA (`nvngx_dlss.dll`, `nvngx_dlssd.dll`,
`nvngx_dlssg.dll`) del [DLSS SDK](https://github.com/NVIDIA/DLSS) bajo la NVIDIA RTX SDKs License.
Siguen siendo software propietario de NVIDIA. Blackvolt no está patrocinado ni respaldado por NVIDIA.

## DLSS NR (builds de la comunidad)

`runtime/nvngx_dlssnr.dll` y `runtime/legacy/nvngx_dlssnr.dll` son builds de la comunidad
**modificadas** a partir del runtime DLSS NR de NVIDIA (Lecram 310.8.3 y ShortFuse SF-v2 310.8.2),
obtenidas de https://github.com/RankFTW/rhi-repo y verificadas por SHA-256 (`runtime/manifest.json`).
No están firmadas por NVIDIA ni respaldadas por NVIDIA. Los derechos de NVIDIA sobre el runtime
y el modelo no se ven afectados por la licencia de Blackvolt.

## Otros

- nlohmann/json — MIT.
- Windows App SDK — MIT.
