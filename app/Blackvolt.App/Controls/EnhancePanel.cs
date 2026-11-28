using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using NrVariant = Blackvolt.App.Interop.NativeEngine.NrVariant;

namespace Blackvolt.App.Controls;

/// <summary>
/// Flyout content with every video enhancement control: DLSS NR (runtime, model, internal size,
/// model parameters, amount of change, correction, protected regions, presets), RTX Video Super
/// Resolution and TrueHDR. Edits <see cref="AppSettings.Video"/> in place, saves it and raises
/// <see cref="Changed"/> (debounced) so the page pushes it to its player or stream.
/// </summary>
internal sealed class EnhancePanel : StackPanel
{
    private static readonly (string Label, int Value)[] Qualities =
        [("Rápida", 1), ("Equilibrada", 2), ("Alta", 3), ("Máxima", 4)];
    private static readonly (string Label, int Value)[] Sizes =
        [("Nativa (máxima calidad)", 0), ("1440p", 1), ("1080p · recomendado", 2), ("720p", 3), ("480p", 4), ("Manual (porcentaje)", 150)];
    private static readonly string[] Runtimes =
        ["Automático", "Compatible SF · RTX 20–50", "NVIDIA original · RTX 50"];
    private static readonly string[] Protections =
        ["Ninguna", "Franja inferior (subtítulos)", "Franja superior", "Superior e inferior", "Centro (mira / HUD)"];
    private static readonly (string Label, string Key)[] Presets =
        [("Equilibrado", "equilibrado"), ("Natural", "natural"), ("Intenso", "intenso"), ("Rendimiento", "rendimiento")];

    private readonly VideoSettings _settings = AppSettings.Current.Video;
    private NrSettings Nr => _settings.Nr;

    private readonly List<Action> _loaders = [];
    private readonly DispatcherTimer _debounce = new() { Interval = TimeSpan.FromMilliseconds(120) };
    private bool _loading;

    private readonly ToggleSwitch _nr, _vsr, _hdr;
    private readonly StackPanel _nrBody, _manualSize, _correctionAuto, _correctionManual, _skinValue;
    private readonly TextBlock _nrState, _note, _peakValue, _download;
    private readonly Button _downloadButton;
    private readonly ProgressBar _downloadBar;
    private readonly StackPanel _hdrOptions;
    private readonly ComboBox _quality;
    private readonly ComboBox _presetBox;

    public event Action? Changed;

    public EnhancePanel()
    {
        Width = 380;
        Spacing = 8;
        _loading = true;
        _debounce.Tick += (_, _) =>
        {
            _debounce.Stop();
            AppSettings.Current.Save();
            Changed?.Invoke();
        };

        var title = new TextBlock { Text = "Mejora de video", FontWeight = FontWeights.SemiBold, FontSize = 16 };
        Children.Add(title);

        // ---------------------------------------------------------------- User presets
        var presetRow = new Grid { ColumnSpacing = 6 };
        presetRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        presetRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        presetRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _presetBox = new ComboBox { Header = "Mis presets", PlaceholderText = "Elegir…", HorizontalAlignment = HorizontalAlignment.Stretch, IsEditable = true };
        var save = new Button { Content = "Guardar", VerticalAlignment = VerticalAlignment.Bottom };
        var delete = new Button { Content = new FontIcon { Glyph = "", FontSize = 14 }, VerticalAlignment = VerticalAlignment.Bottom };
        ToolTipService.SetToolTip(save, "Guarda todos los ajustes de mejora con el nombre escrito");
        ToolTipService.SetToolTip(delete, "Borra el preset elegido");
        Grid.SetColumn(save, 1);
        Grid.SetColumn(delete, 2);
        presetRow.Children.Add(_presetBox);
        presetRow.Children.Add(save);
        presetRow.Children.Add(delete);
        Children.Add(presetRow);
        RefreshPresets();
        _presetBox.SelectionChanged += (_, _) =>
        {
            if (_loading || _presetBox.SelectedItem is not string name) return;
            if (!AppSettings.Current.Presets.TryGetValue(name, out var preset)) return;
            _settings.CopyFrom(preset);
            Load();
            Commit();
        };
        save.Click += (_, _) =>
        {
            var name = (_presetBox.Text ?? "").Trim();
            if (name.Length == 0) return;
            AppSettings.Current.Presets[name] = _settings.Clone();
            AppSettings.Current.Save();
            RefreshPresets(name);
        };
        delete.Click += (_, _) =>
        {
            var name = (_presetBox.SelectedItem as string ?? _presetBox.Text ?? "").Trim();
            if (AppSettings.Current.Presets.Remove(name)) AppSettings.Current.Save();
            RefreshPresets();
        };

        // ---------------------------------------------------------------- Smoothness / A/B
        AddCombo(this, "Fluidez de movimiento", ["Desactivada", "×2", "×3", "×4"],
                 () => Math.Clamp(_settings.Interpolation, 1, 4) - 1, v => _settings.Interpolation = v + 1,
                 "Genera fotogramas intermedios con flujo óptico en la GPU (cualquier marca). Sin latencia añadida; se limita sola a lo que tu pantalla puede mostrar.");
        AddToggle(this, "Comparar A/B (original a la izquierda)", () => _settings.Compare, v => _settings.Compare = v);
        AddCombo(this, "Reducción de ruido", ["Desactivada", "Suave", "Media", "Fuerte"],
                 () => Math.Clamp(_settings.Denoise, 0, 3), v => _settings.Denoise = v,
                 "Filtro bilateral que respeta los bordes, antes de DLSS NR. Útil con vídeo de bitrate bajo o grano.");
        Children.Add(new MenuFlyoutSeparator { Margin = new Thickness(0, 4, 0, 4) });

        // ---------------------------------------------------------------- DLSS NR
        _nr = new ToggleSwitch { Header = "DLSS NR · reconstrucción neuronal", OnContent = "Activado", OffContent = "Desactivado" };
        Children.Add(_nr);
        _nrState = Hint("");
        Children.Add(_nrState);

        // Runtime download, shown when NR has no DLL to run.
        _download = Hint("");
        _downloadBar = new ProgressBar { Maximum = 1, Visibility = Visibility.Collapsed };
        _downloadButton = new Button { Content = "Descargar runtime DLSS NR", Visibility = Visibility.Collapsed };
        _downloadButton.Click += async (_, _) =>
        {
            var variant = Nr.Runtime == 2 ? NrVariant.Original : NrVariant.Compat;
            if (await NrRuntimeInstaller.InstallAsync([variant])) Commit();
        };
        Children.Add(_downloadButton);
        Children.Add(_downloadBar);
        Children.Add(_download);
        Unloaded += (_, _) => NrRuntimeInstaller.Changed -= OnInstallerChanged;
        Loaded += (_, _) => { NrRuntimeInstaller.Changed += OnInstallerChanged; OnInstallerChanged(); };

        _nrBody = new StackPanel { Spacing = 6 };
        Children.Add(_nrBody);

        // Presets
        var presets = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        foreach (var (label, key) in Presets)
        {
            var b = new Button { Content = label, Padding = new Thickness(10, 4, 10, 5), FontSize = 12 };
            b.Click += (_, _) => { Nr.ApplyPreset(key); Load(); Commit(); };
            presets.Children.Add(b);
        }
        _nrBody.Children.Add(Caption("Ajustes rápidos"));
        _nrBody.Children.Add(presets);

        // Runtime and model
        var runtime = Section("Runtime y modelo", expanded: true);
        AddCombo(runtime, "Runtime NR", Runtimes, () => Nr.Runtime, v => Nr.Runtime = v,
                 "Compatible: build FP16 de la comunidad para RTX 20 a 50. Original: el de NVIDIA, solo RTX 50.");
        AddToggle(runtime, "Modelo de rendimiento (B)", () => Nr.PerformanceModel, v => Nr.PerformanceModel = v,
                  "Activado usa el modelo B, más nuevo y rápido; desactivado el A. Solo en el runtime compatible.");
        var sizeCombo = AddCombo(runtime, "Resolución interna", Sizes.Select(s => s.Label).ToArray(),
            () => Nr.Size >= 125 ? Sizes.Length - 1 : Math.Max(0, Array.FindIndex(Sizes, s => s.Value == Nr.Size)),
            i => Nr.Size = Sizes[i].Value == 150 ? (Nr.Size >= 125 ? Nr.Size : 175) : Sizes[i].Value,
            "NR trabaja a esta resolución y solo su cambio vuelve al tamaño real; la salida no cambia de tamaño.");
        _manualSize = new StackPanel { Spacing = 2 };
        AddSlider(_manualSize, "Escala", 25, 100, 1, () => Math.Clamp(Nr.Size - 100, 25, 100), v => Nr.Size = 100 + (int)v, v => $"{v:0} %");
        runtime.Children.Add(_manualSize);
        AddSlider(runtime, "Intensidad del modelo", 0, 100, 1, () => Nr.Intensity, v => Nr.Intensity = (int)v, v => $"{v:0} %");
        _nrBody.Children.Add(Wrap("Runtime y modelo", runtime, true));

        // Model parameters
        var model = Section("Parámetros del modelo");
        var styles = new RadioButtons { Header = "Estilo · experimental", MaxColumns = 3 };
        foreach (var s in new[] { "0", "1", "2" }) styles.Items.Add(s);
        styles.SelectionChanged += (_, _) => { if (!_loading && styles.SelectedIndex >= 0) { Nr.Style = styles.SelectedIndex; Commit(); } };
        _loaders.Add(() => styles.SelectedIndex = Math.Clamp(Nr.Style, 0, 2));
        model.Children.Add(styles);
        AddSlider(model, "Tono local", 0, 1, 0.01, () => Nr.Tone, v => Nr.Tone = v, v => v.ToString("0.00"));
        AddSlider(model, "Estructura local", 0, 1, 0.01, () => Nr.Structure, v => Nr.Structure = v, v => v.ToString("0.00"));
        AddToggle(model, "Textura de piel personalizada", () => Nr.SkinEnabled, v => Nr.SkinEnabled = v, "Sin verificar. Apagado deja que el modelo decida.");
        _skinValue = new StackPanel { Spacing = 2 };
        AddSlider(_skinValue, "Textura de piel", 0, 2, 0.01, () => Nr.Skin, v => Nr.Skin = v, v => v.ToString("0.00"));
        model.Children.Add(_skinValue);
        AddToggle(model, "Máscara automática · experimental", () => Nr.AutoMask, v => Nr.AutoMask = v);
        AddToggle(model, "Corrección de interfaz · sin verificar", () => Nr.UiCorrection, v => Nr.UiCorrection = v);
        _nrBody.Children.Add(Wrap("Parámetros del modelo", model));

        // Amount of change
        var amount = Section("Cantidad de cambio");
        amount.Children.Add(Hint("Escala lo que NR cambia, no la imagen. 1 = lo que entrega el modelo."));
        AddSlider(amount, "Cambio total", 0, 5, 0.01, () => Nr.Total, v => Nr.Total = v, v => $"{v:0.00}×");
        AddSlider(amount, "Oscurecimiento", 0, 2, 0.01, () => Nr.Darken, v => Nr.Darken = v, v => $"{v:0.00}×");
        AddSlider(amount, "Aclarado", 0, 2, 0.01, () => Nr.Brighten, v => Nr.Brighten = v, v => $"{v:0.00}×");
        AddSlider(amount, "Cambio de color", 0, 2, 0.01, () => Nr.Color, v => Nr.Color = v, v => $"{v:0.00}×");
        AddSlider(amount, "Cambio de luminosidad", 0, 2, 0.01, () => Nr.Luminance, v => Nr.Luminance = v, v => $"{v:0.00}×");
        _nrBody.Children.Add(Wrap("Cantidad de cambio", amount));

        // Picture correction
        var correction = Section("Corrección de imagen");
        AddToggle(correction, "Activar corrección", () => Nr.Correction, v => Nr.Correction = v,
                  "Protege tono, saturación y luces cuando NR se pasa. Apagado deja el cambio tal cual.");
        AddToggle(correction, "Automática (según el estilo)", () => Nr.CorrectionAuto, v => Nr.CorrectionAuto = v);
        _correctionAuto = new StackPanel { Spacing = 2 };
        AddSlider(_correctionAuto, "Fuerza de la corrección", 0, 1, 0.01, () => Nr.CorrectionAmount, v => Nr.CorrectionAmount = v, v => v.ToString("0.00"));
        correction.Children.Add(_correctionAuto);
        _correctionManual = new StackPanel { Spacing = 2 };
        AddSlider(_correctionManual, "Protección de tono", 0, 1, 0.01, () => Nr.Hue, v => Nr.Hue = v, v => v.ToString("0.00"));
        AddSlider(_correctionManual, "Protección de saturación", 0, 1, 0.01, () => Nr.Chroma, v => Nr.Chroma = v, v => v.ToString("0.00"));
        AddSlider(_correctionManual, "Protección de neutros", 0, 1, 0.01, () => Nr.Neutral, v => Nr.Neutral = v, v => v.ToString("0.00"));
        AddSlider(_correctionManual, "Conservar color original", 0, 1, 0.01, () => Nr.ColorKeep, v => Nr.ColorKeep = v, v => v.ToString("0.00"));
        AddSlider(_correctionManual, "Conservar luminosidad", 0, 1, 0.01, () => Nr.LumaKeep, v => Nr.LumaKeep = v, v => v.ToString("0.00"));
        AddSlider(_correctionManual, "Protección de sombras", 0, 1, 0.01, () => Nr.Shadow, v => Nr.Shadow = v, v => v.ToString("0.00"));
        AddSlider(_correctionManual, "Protección de luces", 0, 1, 0.01, () => Nr.Highlight, v => Nr.Highlight = v, v => v.ToString("0.00"));
        AddSlider(_correctionManual, "Compresión local", 0, 1, 0.01, () => Nr.Compression, v => Nr.Compression = v, v => v.ToString("0.00"));
        correction.Children.Add(_correctionManual);
        _nrBody.Children.Add(Wrap("Corrección de imagen", correction));

        // Protected regions
        var protect = Section("Zonas protegidas");
        AddCombo(protect, "Mantener original en", Protections, () => Nr.Protect, v => Nr.Protect = v,
                 "Útil para que los subtítulos o el HUD no cambien.");
        AddSlider(protect, "Borde suave", 0, 64, 1, () => Nr.Feather, v => Nr.Feather = v, v => $"{v:0} px");
        _nrBody.Children.Add(Wrap("Zonas protegidas", protect));

        // Anti-flicker
        var flicker = Section("Antiparpadeo");
        AddCombo(flicker, "Memoria temporal",
                 ["Desactivado", "Zonas quietas · suave", "Zonas quietas · fuerte", "Sigue el movimiento · suave", "Sigue el movimiento · fuerte"],
                 () => Nr.AntiFlicker, v => Nr.AntiFlicker = v,
                 "Acumula el cambio de NR en el tiempo para quitar el parpadeo de texturas finas. «Sigue el movimiento» lo lleva con el flujo óptico, también en paneos y escenas en movimiento.");
        AddSlider(flicker, "Estabilidad", 0, 1, 0.01, () => Nr.Stability, v => Nr.Stability = v, v => v.ToString("0.00"));
        _nrBody.Children.Add(Wrap("Antiparpadeo", flicker));

        var reset = new HyperlinkButton { Content = "Restablecer DLSS NR", Padding = new Thickness(0) };
        reset.Click += (_, _) => { Nr.ResetLook(); Load(); Commit(); };
        _nrBody.Children.Add(reset);

        Children.Add(new MenuFlyoutSeparator { Margin = new Thickness(0, 4, 0, 4) });

        // ---------------------------------------------------------------- Colour
        var color = Section("Color");
        AddSlider(color, "Brillo", -1, 1, 0.01, () => _settings.Brightness, v => _settings.Brightness = v, v => $"{v:+0.00;-0.00;0} EV");
        AddSlider(color, "Contraste", 0, 2, 0.01, () => _settings.Contrast, v => _settings.Contrast = v, v => $"{v * 100:0} %");
        AddSlider(color, "Saturación", 0, 2, 0.01, () => _settings.Saturation, v => _settings.Saturation = v, v => $"{v * 100:0} %");
        AddSlider(color, "Intensidad (vibrance)", -1, 1, 0.01, () => _settings.Vibrance, v => _settings.Vibrance = v, v => $"{v:+0.00;-0.00;0}");
        AddSlider(color, "Temperatura", -1, 1, 0.01, () => _settings.Temperature, v => _settings.Temperature = v, v => v < 0 ? "fría" : v > 0 ? "cálida" : "neutra");
        AddSlider(color, "Tinte", -1, 1, 0.01, () => _settings.Tint, v => _settings.Tint = v, v => v < 0 ? "verde" : v > 0 ? "magenta" : "neutro");
        AddSlider(color, "Gamma", 0.5, 2, 0.01, () => _settings.Gamma, v => _settings.Gamma = v, v => v.ToString("0.00"));
        AddToggle(color, "Antialiasing FXAA", () => _settings.Fxaa, v => _settings.Fxaa = v, "Suaviza los bordes dentados de juegos capturados.");
        var colorReset = new HyperlinkButton { Content = "Restablecer color", Padding = new Thickness(0) };
        colorReset.Click += (_, _) => { _settings.ResetColor(); Load(); Commit(); };
        color.Children.Add(colorReset);
        Children.Add(Wrap("Color y antialiasing", color));

        Children.Add(new MenuFlyoutSeparator { Margin = new Thickness(0, 4, 0, 4) });

        // ---------------------------------------------------------------- RTX VSR
        _vsr = new ToggleSwitch { Header = "Superresolución", OnContent = "Activada", OffContent = "Desactivada" };
        Children.Add(_vsr);
        Children.Add(Hint("Reescala al tamaño real de la ventana. RTX VSR usa IA (GeForce RTX); NVIDIA Image Scaling funciona en cualquier GPU."));
        AddCombo(this, "Escalador", ["RTX Video Super Resolution", "NVIDIA Image Scaling (cualquier GPU)"],
                 () => Math.Clamp(_settings.Scaler, 0, 1), v => _settings.Scaler = v);
        AddSlider(this, "Nitidez (NIS)", 0, 1, 0.01, () => _settings.Sharpness, v => _settings.Sharpness = v, v => $"{v * 100:0} %");
        _quality = new ComboBox { Header = "Calidad", HorizontalAlignment = HorizontalAlignment.Stretch };
        foreach (var (label, _) in Qualities) _quality.Items.Add(label);
        Children.Add(_quality);

        Children.Add(new MenuFlyoutSeparator { Margin = new Thickness(0, 4, 0, 4) });

        // ---------------------------------------------------------------- TrueHDR
        AddToggle(this, "Ver vídeo HDR como SDR (tone mapping)", () => _settings.HdrTonemap, v => _settings.HdrTonemap = v,
                  "Los vídeos HDR10/HLG se muestran en HDR si tu pantalla lo tiene activado; con esto se convierten siempre a SDR.");
        _hdr = new ToggleSwitch { Header = "RTX Video TrueHDR", OnContent = "Activado", OffContent = "Desactivado" };
        Children.Add(_hdr);
        Children.Add(Hint("Convierte el video SDR a HDR. Requiere HDR activado en Windows."));
        _hdrOptions = new StackPanel { Spacing = 4 };
        _peakValue = new TextBlock { Opacity = 0.7, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Right };
        AddSlider(_hdrOptions, "Brillo máximo", 0, 2000, 50, () => _settings.HdrPeakNits, v => _settings.HdrPeakNits = v < 400 ? 0 : (int)v,
                  v => v < 400 ? "Auto" : $"{v:0} nits", _peakValue);
        AddSlider(_hdrOptions, "Contraste", 0, 200, 1, () => _settings.HdrContrast, v => _settings.HdrContrast = (int)v, v => $"{v:0}");
        AddSlider(_hdrOptions, "Saturación", 0, 200, 1, () => _settings.HdrSaturation, v => _settings.HdrSaturation = (int)v, v => $"{v:0}");
        AddSlider(_hdrOptions, "Gris medio", 10, 100, 1, () => _settings.HdrMiddleGray, v => _settings.HdrMiddleGray = (int)v, v => $"{v:0}");
        var hdrReset = new HyperlinkButton { Content = "Restablecer valores HDR", Padding = new Thickness(0) };
        hdrReset.Click += (_, _) =>
        {
            (_settings.HdrPeakNits, _settings.HdrContrast, _settings.HdrSaturation, _settings.HdrMiddleGray) = (0, 100, 100, 50);
            Load();
            Commit();
        };
        _hdrOptions.Children.Add(hdrReset);
        Children.Add(_hdrOptions);

        _note = new TextBlock { TextWrapping = TextWrapping.Wrap, FontSize = 12, Visibility = Visibility.Collapsed };
        if (Resource<Brush>("BvVoltBrush") is { } volt) _note.Foreground = volt;
        Children.Add(_note);

        Load();
        _nr.Toggled += (_, _) => { if (_loading) return; Nr.Enabled = _nr.IsOn; Commit(); };
        _vsr.Toggled += (_, _) => { if (_loading) return; _settings.SuperResolution = _vsr.IsOn; Commit(); };
        _hdr.Toggled += (_, _) => { if (_loading) return; _settings.TrueHdr = _hdr.IsOn; Commit(); };
        _quality.SelectionChanged += (_, _) =>
        {
            if (_loading) return;
            _settings.SuperResolutionQuality = Qualities[Math.Max(0, _quality.SelectedIndex)].Value;
            Commit();
        };
        _loading = false;
        _ = sizeCombo;
    }

    /// <summary>Called by the page with the presenter's latest status.</summary>
    public void ShowStatus(in NativeEnhance.Status status)
    {
        var text = NativeEnhance.Explain(status, _settings);
        _note.Text = text ?? string.Empty;
        _note.Visibility = text is null ? Visibility.Collapsed : Visibility.Visible;
        if (_settings.HdrPeakNits <= 0)
            _peakValue.Text = status.DisplayPeakNits > 0 ? $"Auto ({status.DisplayPeakNits:0} nits)" : "Auto";

        if (Nr.Enabled && status.Flags.HasFlag(NativeEnhance.Flags.NeuralActive))
        {
            _nrState.Text = $"{status.NrLabelText} · interno {status.NrWidth}×{status.NrHeight} · " +
                            $"{status.GpuNrTotalMs:0.00} ms por fotograma";
            _nrState.Visibility = Visibility.Visible;
        }
        else
        {
            _nrState.Visibility = Visibility.Collapsed;
        }
        _downloadButton.Visibility = Nr.Enabled && NativeEnhance.NrMissing(status) && !NrRuntimeInstaller.Busy
            ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnInstallerChanged() => DispatcherQueue?.TryEnqueue(() =>
    {
        var busy = NrRuntimeInstaller.Busy;
        _downloadBar.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        _downloadBar.Value = NrRuntimeInstaller.Fraction;
        _download.Text = NrRuntimeInstaller.Message;
        _download.Visibility = busy || NrRuntimeInstaller.LastFailed ? Visibility.Visible : Visibility.Collapsed;
        if (busy) _downloadButton.Visibility = Visibility.Collapsed;
    });

    private void Load()
    {
        _loading = true;
        _nr.IsOn = Nr.Enabled;
        _vsr.IsOn = _settings.SuperResolution;
        _quality.SelectedIndex = Array.FindIndex(Qualities, q => q.Value == _settings.SuperResolutionQuality) is var i and >= 0 ? i : 3;
        _hdr.IsOn = _settings.TrueHdr;
        foreach (var load in _loaders) load();
        UpdateVisibility();
        _loading = false;
    }

    private void RefreshPresets(string? select = null)
    {
        var was = _loading;
        _loading = true;
        _presetBox.Items.Clear();
        foreach (var name in AppSettings.Current.Presets.Keys.OrderBy(n => n, StringComparer.CurrentCultureIgnoreCase))
            _presetBox.Items.Add(name);
        if (select is not null) _presetBox.SelectedItem = select;
        _loading = was;
    }

    private void UpdateVisibility()
    {
        _nrBody.Opacity = Nr.Enabled ? 1.0 : 0.45;
        _manualSize.Visibility = Nr.Size >= 125 ? Visibility.Visible : Visibility.Collapsed;
        _skinValue.Visibility = Nr.SkinEnabled ? Visibility.Visible : Visibility.Collapsed;
        _correctionAuto.Visibility = Nr.Correction && Nr.CorrectionAuto ? Visibility.Visible : Visibility.Collapsed;
        _correctionManual.Visibility = Nr.Correction && !Nr.CorrectionAuto ? Visibility.Visible : Visibility.Collapsed;
        _quality.IsEnabled = _settings.SuperResolution;
        _hdrOptions.Opacity = _settings.TrueHdr ? 1.0 : 0.45;
        _hdrOptions.IsHitTestVisible = _settings.TrueHdr;
    }

    private void Commit()
    {
        if (_loading) return;
        UpdateVisibility();
        _debounce.Stop();
        _debounce.Start();
    }

    // ---- builders ------------------------------------------------------------------------

    private static StackPanel Section(string _, bool expanded = false) => new() { Spacing = 6 };

    private static Expander Wrap(string header, UIElement content, bool expanded = false) => new()
    {
        Header = header,
        Content = content,
        IsExpanded = expanded,
        HorizontalAlignment = HorizontalAlignment.Stretch,
        HorizontalContentAlignment = HorizontalAlignment.Stretch,
    };

    private void AddSlider(Panel host, string label, double min, double max, double step, Func<double> get, Action<double> set,
                           Func<double, string> format, TextBlock? valueText = null)
    {
        var header = new Grid();
        header.Children.Add(new TextBlock { Text = label, FontSize = 12 });
        var value = valueText ?? new TextBlock { Opacity = 0.7, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Right };
        value.FontFamily = new FontFamily("Cascadia Mono, Consolas");
        header.Children.Add(value);
        var slider = new Slider { Minimum = min, Maximum = max, StepFrequency = step, SmallChange = step, LargeChange = step * 10, IsThumbToolTipEnabled = false };
        slider.ValueChanged += (_, e) =>
        {
            value.Text = format(e.NewValue);
            if (_loading) return;
            set(e.NewValue);
            Commit();
        };
        _loaders.Add(() =>
        {
            slider.Value = get();
            value.Text = format(slider.Value);
        });
        host.Children.Add(header);
        host.Children.Add(slider);
    }

    private void AddToggle(Panel host, string label, Func<bool> get, Action<bool> set, string? hint = null)
    {
        var toggle = new ToggleSwitch { Header = label, OnContent = "Sí", OffContent = "No" };
        toggle.Toggled += (_, _) =>
        {
            if (_loading) return;
            set(toggle.IsOn);
            Commit();
        };
        _loaders.Add(() => toggle.IsOn = get());
        host.Children.Add(toggle);
        if (hint is not null) host.Children.Add(Hint(hint));
    }

    private ComboBox AddCombo(Panel host, string label, string[] items, Func<int> get, Action<int> set, string? hint = null)
    {
        var combo = new ComboBox { Header = label, HorizontalAlignment = HorizontalAlignment.Stretch };
        foreach (var item in items) combo.Items.Add(item);
        combo.SelectionChanged += (_, _) =>
        {
            if (_loading || combo.SelectedIndex < 0) return;
            set(combo.SelectedIndex);
            Load(); // dependent rows (manual size)
            Commit();
        };
        _loaders.Add(() => combo.SelectedIndex = Math.Clamp(get(), 0, items.Length - 1));
        host.Children.Add(combo);
        if (hint is not null) host.Children.Add(Hint(hint));
        return combo;
    }

    private static TextBlock Caption(string text) =>
        new() { Text = text, FontSize = 12, Opacity = 0.8, Margin = new Thickness(0, 4, 0, 0) };

    private static TextBlock Hint(string text) =>
        new() { Text = text, TextWrapping = TextWrapping.Wrap, FontSize = 12, Opacity = 0.65, Margin = new Thickness(0, -4, 0, 2) };

    private static T? Resource<T>(string key) where T : class =>
        Application.Current.Resources.TryGetValue(key, out var value) ? value as T : null;
}
