# Avisos de terceros

Blackvolt se distribuye bajo **AGPL-3.0** (ver `LICENSE`). Los componentes siguientes
conservan sus propias licencias y **no** quedan cubiertos por la AGPL.

## NVIDIA DLSS SDK y runtimes NGX

Los paquetes de Release incluyen DLLs de NVIDIA (`nvngx_dlss.dll`, `nvngx_dlssd.dll`,
`nvngx_dlssg.dll`) del [DLSS SDK](https://github.com/NVIDIA/DLSS) bajo la NVIDIA RTX SDKs License.
Siguen siendo software propietario de NVIDIA. Blackvolt no está patrocinado ni respaldado por NVIDIA.

## DLSS NR

Blackvolt no incluye ni descarga ningún runtime de DLSS NR. El motor tiene preparada la etapa
(`engine/src/enhance/NeuralRendering.h`) para cuando NVIDIA publique NR oficialmente para
aplicaciones de terceros.

## Otros

- nlohmann/json — MIT.
- Windows App SDK — MIT.
- chiaki-ng (https://github.com/streetpea/chiaki-ng) — AGPL-3.0 con excepción de OpenSSL. Por usarlo,
  Blackvolt completo se distribuye bajo AGPL-3.0.
- SDL3 — zlib.
- FFmpeg — LGPL-2.1+ (enlazado dinámicamente desde vcpkg).
- OpenSSL, Opus, json-c, libevent, miniupnpc, curl, nanopb, Jerasure, GF-Complete: sus licencias
  respectivas (Apache-2.0, BSD, MIT y similares), incluidas en las carpetas de cada paquete.
