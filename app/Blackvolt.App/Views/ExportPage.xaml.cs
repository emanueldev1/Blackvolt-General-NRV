using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using Windows.Storage.Pickers;
using ExportState = Blackvolt.App.Interop.NativeExport.State;

namespace Blackvolt.App.Views;

public sealed partial class ExportPage : Page
{
    private static readonly string[] VideoExtensions =
        [".mp4", ".mkv", ".mov", ".avi", ".webm", ".m4v", ".ts", ".m2ts", ".wmv", ".flv"];

    // The export outlives the page: leaving it does not stop the encode.
    private static NativeExport.Job? _job;
    private static string _input = "";
    private static string _output = "";
    private static bool _recorded;

    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(250) };

    public ExportPage()
    {
        InitializeComponent();
        _timer.Tick += (_, _) => Refresh();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        var s = AppSettings.Current.Export;
        Select(CodecBox, s.Codec.ToString());
        Select(ResolutionBox, s.Height.ToString());
        Select(ContainerBox, s.Container);
        QualitySlider.Value = s.Quality;
        AudioSwitch.IsOn = s.Audio;
        NrSwitch.IsOn = s.Nr;
        NrSwitch.IsEnabled = App.Engine is not null;
        CodecBox.SelectionChanged += (_, _) => UpdateEncoderHint();
        UpdateEncoderHint();
        if (e.Parameter is string path && File.Exists(path)) SetInput(path);
        else if (_input.Length > 0) SetInput(_input);
        if (_job is not null)
        {
            ProgressCard.Visibility = Visibility.Visible;
            Refresh();
            _timer.Start();
        }
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e) => _timer.Stop();

    private void SetInput(string path)
    {
        _input = path;
        InputText.Text = path;
        StartButton.IsEnabled = _job is null || IsFinished(_job.Current.State);
    }

    private void UpdateEncoderHint()
    {
        string[] hardware = TagOf(CodecBox) switch
        {
            "0" => ["h264_nvenc", "h264_amf", "h264_mf"],
            "2" => ["av1_nvenc", "av1_amf", "av1_mf"],
            _ => ["hevc_nvenc", "hevc_amf", "hevc_mf"],
        };
        string? found = null;
        try { found = hardware.FirstOrDefault(n => NativeExport.HasEncoder(n) != 0); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        EncoderHint.Text = found is null
            ? "Se elegirá el mejor codificador disponible al empezar."
            : $"Se probará primero {Pretty(found)} y, si la GPU no lo admite, el siguiente disponible.";
    }

    private static string Pretty(string encoder) => encoder switch
    {
        _ when encoder.Contains("nvenc") => "NVENC (NVIDIA)",
        _ when encoder.Contains("_amf") => "AMF (AMD)",
        _ when encoder.Contains("_qsv") => "Quick Sync (Intel)",
        _ when encoder.EndsWith("_mf") => "Media Foundation",
        _ => encoder,
    };

    private void OnQualityChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e) =>
        QualitySlider.Header = $"Calidad · {e.NewValue:0} {(e.NewValue >= 85 ? "(casi sin pérdidas)" : e.NewValue <= 40 ? "(archivo pequeño)" : "")}";

    private async void OnPickInput(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.VideosLibrary };
        foreach (var ext in VideoExtensions) picker.FileTypeFilter.Add(ext);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!));
        var file = await picker.PickSingleFileAsync();
        if (file is not null) SetInput(file.Path);
    }

    private async void OnStart(object sender, RoutedEventArgs e)
    {
        if (_input.Length == 0) return;
        var container = TagOf(ContainerBox) ?? ".mp4";
        var codec = int.TryParse(TagOf(CodecBox), out var c) ? c : 1;
        var height = int.TryParse(TagOf(ResolutionBox), out var h) ? h : 0;

        var picker = new FileSavePicker
        {
            SuggestedStartLocation = PickerLocationId.VideosLibrary,
            SuggestedFileName = Path.GetFileNameWithoutExtension(_input) + (height > 0 ? $" {height}p" : "") + " (Blackvolt)",
        };
        picker.FileTypeChoices.Add(container.TrimStart('.').ToUpperInvariant(), [container]);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!));
        var file = await picker.PickSaveFileAsync();
        if (file is null) return;

        var settings = AppSettings.Current.Export;
        settings.Codec = codec;
        settings.Height = height;
        settings.Quality = (int)QualitySlider.Value;
        settings.Audio = AudioSwitch.IsOn;
        settings.Nr = NrSwitch.IsOn && NrSwitch.IsEnabled;
        settings.Container = container;
        AppSettings.Current.Save();

        _job?.Dispose();
        _job = null;
        _recorded = false;
        Notice.IsOpen = false;
        try
        {
            _output = file.Path;
            NativeEnhance.Settings? nr = null;
            if (settings.Nr && App.Engine is not null)
            {
                var video = AppSettings.Current.Video;
                var s = NativeEnhance.From(video);
                // NR only on NVIDIA and when it is on in the player; denoise everywhere.
                s.Neural = video.Nr.Enabled && App.Engine.Adapter.Vendor == NativeEngine.Vendor.Nvidia ? 1u : 0u;
                var colorChanged = video.Brightness != 0 || video.Contrast != 1 || video.Saturation != 1 || video.Vibrance != 0 ||
                                   video.Temperature != 0 || video.Tint != 0 || video.Gamma != 1 || video.Fxaa;
                if (s.Neural != 0 || s.Denoise != 0 || colorChanged) nr = s;
            }
            _job = NativeExport.Job.Start(_input, _output, (NativeExport.Codec)codec, 0, height,
                                          settings.Quality, settings.Audio, App.Engine, nr);
        }
        catch (Exception ex) when (ex is EngineException or DllNotFoundException or EntryPointNotFoundException)
        {
            ShowNotice("No se pudo empezar", ex.Message, InfoBarSeverity.Error);
            return;
        }
        StartButton.IsEnabled = false;
        ProgressCard.Visibility = Visibility.Visible;
        Refresh();
        _timer.Start();
    }

    private void OnCancel(object sender, RoutedEventArgs e) => _job?.Cancel();

    private void OnShowFile(object sender, RoutedEventArgs e)
    {
        if (!File.Exists(_output)) return;
        System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{_output}\"");
    }

    private void OnPlayFile(object sender, RoutedEventArgs e)
    {
        if (File.Exists(_output)) Frame.Navigate(typeof(PlayerPage), new OpenMedia(_output, Path.GetFileName(_output)));
    }

    private void Refresh()
    {
        if (_job is null) return;
        var s = _job.Current;
        var finished = IsFinished(s.State);
        ProgressTitle.Text = s.State switch
        {
            ExportState.Done => $"Listo: {Path.GetFileName(_output)}",
            ExportState.Failed => "La exportación falló",
            ExportState.Cancelled => "Exportación cancelada",
            _ => $"Exportando {Path.GetFileName(_input)}",
        };
        Bar.IsIndeterminate = s.State == ExportState.Running && s.Progress <= 0;
        Bar.Value = s.Progress;
        Bar.ShowError = s.State == ExportState.Failed;
        PercentText.Text = $"{s.Progress * 100:0}%";

        var encoder = s.EncoderText;
        var lines = new List<string>();
        if (encoder.Length > 0)
            lines.Add($"{Pretty(encoder)} · {(s.Hardware != 0 ? "GPU" : "CPU")} · {s.Width}×{s.Height}");
        if (s.Frames > 0)
            lines.Add($"{s.Frames} fotogramas · {s.Fps:0.0} fps · {Clock(s.ElapsedS)}" +
                      (s.State == ExportState.Running && s.EtaS > 0 ? $" · quedan {Clock(s.EtaS)}" : ""));
        if (s.EnhanceText is { Length: > 0 } enhanceText) lines.Add(enhanceText);
        if (s.MessageText is { Length: > 0 } message) lines.Add(message);
        DetailText.Text = string.Join("\n", lines);

        CancelButton.Visibility = finished ? Visibility.Collapsed : Visibility.Visible;
        ShowButton.Visibility = s.State == ExportState.Done ? Visibility.Visible : Visibility.Collapsed;
        PlayButton.Visibility = ShowButton.Visibility;
        if (finished)
        {
            _timer.Stop();
            if (s.State == ExportState.Done && !_recorded)
            {
                _recorded = true;
                Services.SessionHistory.Add(new Services.SessionRecord
                {
                    Kind = "Exportar",
                    Title = Path.GetFileName(_output),
                    StartedUtc = DateTime.UtcNow.AddSeconds(-s.ElapsedS),
                    DurationS = s.ElapsedS,
                    Width = s.Width,
                    Height = s.Height,
                    AvgFps = s.Fps,
                    Presented = s.Frames,
                    Detail = $"{Pretty(encoder)} · {(s.Hardware != 0 ? "GPU" : "CPU")}",
                });
            }
            StartButton.IsEnabled = _input.Length > 0;
        }
    }

    private static bool IsFinished(ExportState state) => state is ExportState.Done or ExportState.Failed or ExportState.Cancelled;

    private static string Clock(double seconds)
    {
        var t = TimeSpan.FromSeconds(Math.Max(0, seconds));
        return t.TotalHours >= 1 ? t.ToString(@"h\:mm\:ss") : t.ToString(@"m\:ss");
    }

    private void ShowNotice(string title, string message, InfoBarSeverity severity)
    {
        Notice.Title = title;
        Notice.Message = message;
        Notice.Severity = severity;
        Notice.IsOpen = true;
    }

    private static void Select(ComboBox box, string tag)
    {
        var index = box.Items.OfType<ComboBoxItem>().ToList().FindIndex(i => i.Tag as string == tag);
        box.SelectedIndex = Math.Max(0, index);
    }

    private static string? TagOf(ComboBox box) => box.SelectedItem is ComboBoxItem { Tag: string tag } ? tag : null;
}
