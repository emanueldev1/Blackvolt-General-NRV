#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace bv::ngx { class NgxRuntime; }

namespace bv::enhance {

// User controls for the neural rendering stage.
struct NeuralControls {
    float intensity = 1.0f; // 0..1
    bool temporal = true;   // reuse the previous output (cleared on seeks and cuts)
    bool operator==(const NeuralControls&) const = default;
};

// Slot in the presenter's chain for NVIDIA's neural rendering (DLSS NR). It runs on the
// decoded picture at source size, before RTX VSR and TrueHDR:
//
//     YUV -> RGBA8 -> [NeuralRendering] -> [VSR] -> [TrueHDR] -> screen
//
// Contract for an implementation:
//   * input:  RGBA8 (sRGB), shader-resource state, `width` x `height`.
//   * output: RGBA8, unordered-access state, same size (owned by the presenter).
//   * evaluate() records into the presenter's command list and must not wait on the CPU.
//   * reset = true after a seek, a resolution change or a stream reconnect.
//
// createNeuralRendering() returns the implementation in use, or nullptr with a reason.
class NeuralRendering {
public:
    virtual ~NeuralRendering() = default;
    virtual bool evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output,
                          uint32_t width, uint32_t height, const NeuralControls& controls, bool reset) = 0;
    [[nodiscard]] virtual const char* name() const = 0;
};

// Returns the available implementation, or nullptr with `reason` filled in for the UI.
std::unique_ptr<NeuralRendering> createNeuralRendering(ngx::NgxRuntime* runtime, ID3D12GraphicsCommandList* list,
                                                       uint32_t width, uint32_t height, std::string* reason);

} // namespace bv::enhance
