using Blackvolt.App.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Caps = Blackvolt.App.Interop.NativeEngine.Capabilities;

namespace Blackvolt.App.Views;

public sealed partial class HomePage : Page
{
    private static readonly (Caps Cap, string Label)[] CapabilityLabels =
    [
        (Caps.RtxVideoSuperResolution, "RTX Video Super Resolution"),
        (Caps.RtxTrueHdr, "RTX TrueHDR"),
        (Caps.OpticalFlow, "Optical Flow"),
        (Caps.Nvenc, "NVENC"),
        (Caps.Nvdec, "NVDEC"),
        (Caps.Tearing, "VRR / G-SYNC"),
        (Caps.D3D12, "DirectX 12"),
    ];

    public HomePage()
    {
        InitializeComponent();
        Show(App.Engine?.Adapter, App.EngineError);
    }

    private void Show(GpuAdapter? gpu, string? error)
    {
        if (gpu is null)
        {
            GpuCard.Visibility = Visibility.Collapsed;
            EngineErrorBar.Message = error ?? "Error desconocido.";
            EngineErrorBar.IsOpen = true;
            return;
        }

        GpuName.Text = gpu.Name;
        var driver = string.IsNullOrEmpty(gpu.DriverVersion) ? "" : $"  ·  Driver {gpu.DriverVersion}";
        GpuDetails.Text = $"{gpu.VramGiB:0.#} GB VRAM{driver}";
        CapabilityChips.ItemsSource = CapabilityLabels.Where(c => gpu.Has(c.Cap)).Select(c => c.Label).ToList();
    }
}
