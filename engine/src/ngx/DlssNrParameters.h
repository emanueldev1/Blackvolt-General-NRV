#pragma once

// NGX parameter names for DLSS NR (feature 18), in one place. The value type is fixed by the
// setter used at the call site (unsigned int, int, float, ID3D12Resource*, void*): a name with
// the wrong type is silently ignored by the runtime, so keep the setters as documented here.

namespace bv::ngx::dlssnr {

// --- Creation (unsigned int unless noted)
constexpr const char* kWidth = "DLSSNR.Width";
constexpr const char* kHeight = "DLSSNR.Height";
constexpr const char* kInputWidth = "DLSSNR.InputWidth";
constexpr const char* kInputHeight = "DLSSNR.InputHeight";
constexpr const char* kOutputWidth = "DLSSNR.OutputWidth";
constexpr const char* kOutputHeight = "DLSSNR.OutputHeight";
constexpr const char* kOutputDotWidth = "DLSSNR.Output.Width";
constexpr const char* kOutputDotHeight = "DLSSNR.Output.Height";
constexpr const char* kUpscaling = "DLSSNR.Upscaling";
constexpr const char* kScale = "DLSSNR.Scale";                 // float
constexpr const char* kScalingRatio = "DLSSNR.ScalingRatio";   // float
constexpr const char* kComputeScalingRatioCallback = "DLSSNRComputeScalingRatioCallback"; // void*
constexpr const char* kHintRenderPreset = "DLSSNR.Hint.Render.Preset"; // int
constexpr const char* kStdWidth = "Width";
constexpr const char* kStdHeight = "Height";
constexpr const char* kPerfQualityValue = "PerfQualityValue"; // int
constexpr const char* kCreationNodeMask = "CreationNodeMask";
constexpr const char* kVisibilityNodeMask = "VisibilityNodeMask";

// --- Evaluation resources (ID3D12Resource*)
constexpr const char* kColor = "DLSSNR.Color";
constexpr const char* kOutput = "DLSSNR.Output";
constexpr const char* kMVec = "DLSSNR.MVec";
constexpr const char* kDepth = "DLSSNR.Depth";

// --- Evaluation sub-rectangles (unsigned int)
constexpr const char* kColorSubrectWidth = "DLSSNR.ColorSubrectWidth";
constexpr const char* kColorSubrectHeight = "DLSSNR.ColorSubrectHeight";
constexpr const char* kOutputSubrectWidth = "DLSSNR.OutputSubrectWidth";
constexpr const char* kOutputSubrectHeight = "DLSSNR.OutputSubrectHeight";
constexpr const char* kMVecSubrectWidth = "DLSSNR.MVecSubrectWidth";
constexpr const char* kMVecSubrectHeight = "DLSSNR.MVecSubrectHeight";
constexpr const char* kDepthSubrectWidth = "DLSSNR.DepthSubrectWidth";
constexpr const char* kDepthSubrectHeight = "DLSSNR.DepthSubrectHeight";

// --- Evaluation controls
constexpr const char* kMVecScaleX = "DLSSNR.MVecScaleX";          // float
constexpr const char* kMVecScaleY = "DLSSNR.MVecScaleY";          // float
constexpr const char* kDepthInverted = "DLSSNR.DepthInverted";    // int
constexpr const char* kIndicatorInvertX = "DLSS.Indicator.Invert.X.Axis"; // int
constexpr const char* kIndicatorInvertY = "DLSS.Indicator.Invert.Y.Axis"; // int
constexpr const char* kEnabled = "DLSSNR.Enabled";                // int
constexpr const char* kReset = "DLSSNR.Reset";                    // int
constexpr const char* kStyle = "DLSSNR.Style";                    // int 0..2
constexpr const char* kIntensity = "DLSSNR.Intensity";            // float 0..1
constexpr const char* kLocalToneStrength = "DLSSNR.LocalToneStrength";           // float 0..1
constexpr const char* kLocalStructureStrength = "DLSSNR.LocalStructureStrength"; // float 0..1
constexpr const char* kSkinStructureStrength = "DLSSNR.SkinStructureStrength";   // float, -1 = unset
constexpr const char* kUseAutoMask = "DLSSNR.UseAutoMask";        // int
constexpr const char* kUICorrection = "DLSSNR.UICorrection";      // int

} // namespace bv::ngx::dlssnr
