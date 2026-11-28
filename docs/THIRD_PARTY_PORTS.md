# Código de terceros incorporado

Blackvolt es AGPL-3.0. Estas partes vienen de otros proyectos (licencias compatibles). Al restaurar
`NOTICE.md`, añade este contenido y corrige su sección «DLSS NR», que ya no es cierta: Blackvolt ahora
descarga el runtime NR de RankFTW/rhi-repo, como Veyra.

| Parte | Origen | Licencia |
|---|---|---|
| Carga de DLSS NR (`ngx/DlssNrSession.cpp`, `ngx/NrArchitecturePolicy.h`, `ngx/DlssNrParameters.h`) | Veyra 2.2.1, `src/ngx/DlssNrRuntimeAdapter.cpp` y cabeceras (Likely7/Veyra-NRVideo) | GPL-3.0 |
| Composición residual, corrección Oklab y zonas protegidas de NR (shader en `enhance/NeuralRendering.cpp`) | Veyra `shaders/NrResidualComposite.hlsl`, `NrCorrection.hlsli`, `NrProtection.hlsli`; partes adaptadas de SAOG0721/Magpie | GPL-3.0; matrices Oklab de Björn Ottosson (dominio público) |
| Reducción de ruido (`enhance/Denoise.cpp`) | Veyra `shaders/VideoDenoise.hlsl` ← Blinue/Magpie ← bloc97/Anime4K | GPL-3.0 / MIT |
| NVIDIA Image Scaling (`third_party/nis`) | NVIDIA Image Scaling SDK 1.0.3 | MIT (`third_party/nis/licence.txt`) |
| Runtimes `nvngx_dlssnr.dll` | Se descargan en la máquina del usuario desde RankFTW/rhi-repo (mismo origen que Veyra); no se redistribuyen en el repo ni en las Releases | Propietario de NVIDIA / builds de la comunidad |

Interpolación de fotogramas, antiparpadeo de NR, overlay de estadísticas, instalador y lector de ZIP
remoto son código propio de Blackvolt.
