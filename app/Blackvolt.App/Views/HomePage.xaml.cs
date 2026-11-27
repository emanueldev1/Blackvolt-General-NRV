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
        if (Services.Updater.Available is { } update) ShowUpdate(update);
        Services.Updater.Found += OnUpdateFound;
        Unloaded += (_, _) => Services.Updater.Found -= OnUpdateFound;
    }

    private void OnUpdateFound(Services.UpdateInfo info) => DispatcherQueue.TryEnqueue(() => ShowUpdate(info));

    private void ShowUpdate(Services.UpdateInfo info)
    {
        if (info.Tag == Services.AppSettings.Current.Update.SkippedTag) return;
        UpdateBar.Message = $"Versión {info.Version} disponible (tienes la {Services.Updater.CurrentVersion}).";
        UpdateBar.IsOpen = true;
    }

    private void OnOpenUpdate(object sender, RoutedEventArgs e) => App.MainWindow?.Go("settings");

    private void OnQuickAction(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string tag }) App.MainWindow?.Go(tag);
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
