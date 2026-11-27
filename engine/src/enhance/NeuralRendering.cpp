#include "NeuralRendering.h"

namespace bv::enhance {

std::unique_ptr<NeuralRendering> createNeuralRendering(ngx::NgxRuntime*, ID3D12GraphicsCommandList*, uint32_t,
                                                       uint32_t, std::string* reason) {
    if (reason) *reason = "Pendiente del soporte oficial de NVIDIA para aplicaciones de terceros.";
    return nullptr;
}

} // namespace bv::enhance
