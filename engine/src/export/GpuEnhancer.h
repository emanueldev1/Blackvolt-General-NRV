#pragma once

#include "enhance/Denoise.h"
#include "enhance/NeuralRendering.h"
#include "enhance/PostFx.h"
#include "export/Exporter.h"

#include <memory>
#include <string>

struct ID3D12Device;
namespace bv::ngx { class NgxRuntime; }

namespace bv::exporter {

// GPU filters for offline export: each decoded RGBA frame is uploaded, run through the same
// stages the player uses (denoise, then DLSS NR with anti-flicker) and read back. Synchronous: one
// frame in flight, so the encoder always gets frames in order. denoise: 0 off .. 3 strong.
std::unique_ptr<FrameFilter> createNrFilter(ID3D12Device* device, ngx::NgxRuntime* ngx, enhance::NrLocator locate,
                                            uint32_t width, uint32_t height, bool neural,
                                            const enhance::NeuralControls& controls, uint32_t denoise,
                                            const enhance::ColorControls& color, double frameMs, std::string* error);

} // namespace bv::exporter
