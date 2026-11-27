using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace Blackvolt.App.Controls;

/// <summary>Flyout content with the RTX Video controls. Edits <see cref="AppSettings.Video"/> in place,
/// saves it and calls <see cref="Changed"/> so the page can push it to its player or stream.</summary>
internal sealed class EnhancePanel : StackPanel
{
    private static readonly (string Label, int Value)[] Qualities =
        [("Rápida", 1), ("Equilibrada", 2), ("Alta", 3), ("Máxima", 4)];

    private readonly VideoSettings _settings = AppSettings.Current.Video;
    private readonly ToggleSwitch _vsr, _hdr;
    private readonly ComboBox _quality;
    private readonly Slider _peak, _contrast, _saturation, _middleGray;
    private readonly TextBlock _peakValue, _note;
    private readonly StackPanel _hdrOptions;
    private bool _loading;

    public event Action? Changed;

    public EnhancePanel()
    {
        Width = 320;
        Spacing = 10;
        _loading = true;

        var title = new TextBlock { Text = "Mejora de video", FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        if (Resource<Style>("BodyStrongTextBlockStyle") is { } strong) title.Style = strong;
        Children.Add(title);

        _vsr = new ToggleSwitch { Header = "RTX Video Super Resolution", OnContent = "Activado", OffContent = "Desactivado" };
        Children.Add(_vsr);
        Children.Add(Hint("Reescala con IA al tamaño real de la ventana y limpia los artefactos de compresión."));

        _quality = new ComboBox { Header = "Calidad", HorizontalAlignment = HorizontalAlignment.Stretch };
        foreach (var (label, _) in Qualities) _quality.Items.Add(label);
        Children.Add(_quality);

        Children.Add(new MenuFlyoutSeparator { Margin = new Thickness(0, 4, 0, 4) });

        _hdr = new ToggleSwitch { Header = "RTX Video TrueHDR", OnContent = "Activado", OffContent = "Desactivado" };
        Children.Add(_hdr);
        Children.Add(Hint("Convierte el video SDR a HDR. Requiere HDR activado en Windows."));

        _hdrOptions = new StackPanel { Spacing = 4 };
        _peakValue = new TextBlock { Opacity = 0.7, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Right };
        _peak = AddSlider(_hdrOptions, "Brillo máximo", 0, 2000, 50, _peakValue);
        _contrast = AddSlider(_hdrOptions, "Contraste", 0, 200, 1);
        _saturation = AddSlider(_hdrOptions, "Saturación", 0, 200, 1);
        _middleGray = AddSlider(_hdrOptions, "Gris medio", 10, 100, 1);
        var reset = new HyperlinkButton { Content = "Restablecer valores HDR", Padding = new Thickness(0) };
        reset.Click += (_, _) =>
        {
            _settings.HdrPeakNits = 0;
            _settings.HdrContrast = 100;
            _settings.HdrSaturation = 100;
            _settings.HdrMiddleGray = 50;
            Load();
            Commit();
        };
        _hdrOptions.Children.Add(reset);
        Children.Add(_hdrOptions);

        _note = new TextBlock { TextWrapping = TextWrapping.Wrap, FontSize = 12, Visibility = Visibility.Collapsed };
        if (Resource<Brush>("BvVoltBrush") is { } volt) _note.Foreground = volt;
        Children.Add(_note);

        Load();
        _vsr.Toggled += (_, _) => Commit();
        _hdr.Toggled += (_, _) => Commit();
        _quality.SelectionChanged += (_, _) => Commit();
        foreach (var s in new[] { _peak, _contrast, _saturation, _middleGray }) s.ValueChanged += OnSlider;
        _loading = false;
    }

    /// <summary>Called by the page with the presenter's latest status.</summary>
    public void ShowStatus(in NativeEnhance.Status status)
    {
        var text = NativeEnhance.Explain(status, _settings);
        _note.Text = text ?? string.Empty;
        _note.Visibility = text is null ? Visibility.Collapsed : Visibility.Visible;
        if (_settings.HdrPeakNits <= 0)
            _peakValue.Text = status.DisplayPeakNits > 0 ? $"Auto ({status.DisplayPeakNits:0} nits)" : "Auto";
    }

    private void Load()
    {
        _loading = true;
        _vsr.IsOn = _settings.SuperResolution;
        _quality.SelectedIndex = Array.FindIndex(Qualities, q => q.Value == _settings.SuperResolutionQuality) is var i and >= 0 ? i : 3;
        _hdr.IsOn = _settings.TrueHdr;
        _peak.Value = _settings.HdrPeakNits;
        _contrast.Value = _settings.HdrContrast;
        _saturation.Value = _settings.HdrSaturation;
        _middleGray.Value = _settings.HdrMiddleGray;
        UpdateLabels();
        _loading = false;
    }

    private void OnSlider(object sender, RangeBaseValueChangedEventArgs e)
    {
        UpdateLabels();
        Commit();
    }

    private void UpdateLabels()
    {
        // Below 400 nits the slider means "use what the display reports".
        var peak = (int)_peak.Value;
        _peakValue.Text = peak < 400 ? "Auto" : $"{peak} nits";
        _quality.IsEnabled = _vsr.IsOn;
        _hdrOptions.Opacity = _hdr.IsOn ? 1.0 : 0.45;
        _hdrOptions.IsHitTestVisible = _hdr.IsOn;
    }

    private void Commit()
    {
        if (_loading) return;
        _settings.NeuralRendering = false;
        _settings.SuperResolution = _vsr.IsOn;
        _settings.SuperResolutionQuality = Qualities[Math.Max(0, _quality.SelectedIndex)].Value;
        _settings.TrueHdr = _hdr.IsOn;
        var peak = (int)_peak.Value;
        _settings.HdrPeakNits = peak < 400 ? 0 : peak;
        _settings.HdrContrast = (int)_contrast.Value;
        _settings.HdrSaturation = (int)_saturation.Value;
        _settings.HdrMiddleGray = (int)_middleGray.Value;
        UpdateLabels();
        AppSettings.Current.Save();
        Changed?.Invoke();
    }

    private static Slider AddSlider(Panel host, string label, double min, double max, double step, TextBlock? value = null)
    {
        var header = new Grid();
        header.Children.Add(new TextBlock { Text = label, FontSize = 12 });
        if (value is not null) header.Children.Add(value);
        var slider = new Slider { Minimum = min, Maximum = max, StepFrequency = step, SmallChange = step };
        host.Children.Add(header);
        host.Children.Add(slider);
        return slider;
    }

    private static TextBlock Hint(string text) =>
        new() { Text = text, TextWrapping = TextWrapping.Wrap, FontSize = 12, Opacity = 0.65, Margin = new Thickness(0, -6, 0, 0) };

    private static T? Resource<T>(string key) where T : class =>
        Application.Current.Resources.TryGetValue(key, out var value) ? value as T : null;
}
