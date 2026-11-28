using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Blackvolt.App.Views;

public sealed partial class SettingsPage : Page
{
    public SettingsPage()
    {
        InitializeComponent();

        try
        {
            var active = App.Engine?.Adapter.Index;
            AdapterList.ItemsSource = EngineSession.ListAdapters()
                .Select(a => $"{a.Name}  ·  {a.VramGiB:0.#} GB{(a.Index == active ? "  ·  en uso" : "")}")
                .ToList();
        }
        catch (Exception ex) when (ex is EngineException or DllNotFoundException)
        {
            AdapterList.ItemsSource = new[] { "No se pudo leer la lista de GPUs." };
        }

        VersionText.Text = $"Blackvolt {Updater.CurrentVersion}";
        AutoCheckSwitch.IsOn = AppSettings.Current.Update.AutoCheck;
        foreach (var name in Mirrors.Names) MirrorBox.Items.Add(name);
        MirrorBox.SelectedIndex = Math.Clamp(AppSettings.Current.Update.Mirror, 0, Mirrors.Names.Length - 1);
        NrAutoSwitch.IsOn = AppSettings.Current.Video.Nr.AutoInstall;
        ResumeSwitch.IsOn = AppSettings.Current.Capture.ResumeLastSource;
        if (Updater.Available is { } update) ShowUpdate(update);

        var ui = AppSettings.Current.Ui;
        foreach (var (label, _) in StartPages) StartPageBox.Items.Add(label);
        StartPageBox.SelectedIndex = Math.Max(0, Array.FindIndex(StartPages, p => p.Tag == ui.StartPage));
        foreach (var (label, _) in HideOptions) AutoHideBox.Items.Add(label);
        AutoHideBox.SelectedIndex = Math.Max(0, Array.FindIndex(HideOptions, o => Math.Abs(o.Seconds - ui.AutoHideSeconds) < 0.2));
        RememberWindowSwitch.IsOn = ui.RememberWindow;
        ReduceMotionSwitch.IsOn = ui.ReduceMotion;
        _loaded = true;
    }

    private static readonly (string Label, string Tag)[] StartPages =
        [("Inicio", "home"), ("Reproductor", "player"), ("PlayStation", "remote"), ("Captura", "capture"), ("La última que usé", "last")];
    private static readonly (string Label, double Seconds)[] HideOptions =
        [("Tras 1,5 s (como Veyra)", 1.6), ("Tras 3 s", 3), ("Tras 6 s", 6), ("Nunca", 0)];
    private bool _loaded;

    private void OnStartPageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded || StartPageBox.SelectedIndex < 0) return;
        AppSettings.Current.Ui.StartPage = StartPages[StartPageBox.SelectedIndex].Tag;
        AppSettings.Current.Save();
    }

    private void OnAutoHideChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded || AutoHideBox.SelectedIndex < 0) return;
        AppSettings.Current.Ui.AutoHideSeconds = HideOptions[AutoHideBox.SelectedIndex].Seconds;
        AppSettings.Current.Save();
    }

    private void OnRememberWindowToggled(object sender, RoutedEventArgs e)
    {
        if (!_loaded) return;
        AppSettings.Current.Ui.RememberWindow = RememberWindowSwitch.IsOn;
        AppSettings.Current.Save();
    }

    private void OnReduceMotionToggled(object sender, RoutedEventArgs e)
    {
        if (!_loaded) return;
        AppSettings.Current.Ui.ReduceMotion = ReduceMotionSwitch.IsOn;
        AppSettings.Current.Save();
    }

    private void OnOpenLogs(object sender, RoutedEventArgs e)
    {
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", $"\"{EngineLog.Folder}\"") { UseShellExecute = true });
    }

    private void OnCopyDiagnostics(object sender, RoutedEventArgs e)
    {
        var sb = new System.Text.StringBuilder();
        sb.AppendLine($"Blackvolt {Updater.CurrentVersion} · {Environment.OSVersion}");
        if (App.Engine?.Adapter is { } a) sb.AppendLine($"GPU: {a.Name} · {a.VramGiB:0.#} GB · driver {a.DriverVersion}");
        else sb.AppendLine($"Motor: {App.EngineError}");
        foreach (var v in new[] { NativeEngine.NrVariant.Compat, NativeEngine.NrVariant.Original })
        {
            var info = NrRuntimeInstaller.Info(v);
            sb.AppendLine($"NR {v}: {(info.Installed != 0 ? $"{info.VersionText} {info.Sha256Text[..Math.Min(12, info.Sha256Text.Length)]}" : "no instalado")}");
        }
        foreach (var r in NrRuntimeInstaller.VideoRuntimes) sb.AppendLine($"{r.Label}: {(r.Installed ? "instalado" : "no instalado")}");
        sb.AppendLine();
        sb.AppendLine("--- engine.log (últimas líneas)");
        try
        {
            using var f = new FileStream(EngineLog.FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var reader = new StreamReader(f);
            var lines = reader.ReadToEnd().Split('\n');
            foreach (var line in lines.Skip(Math.Max(0, lines.Length - 80))) sb.AppendLine(line.TrimEnd('\r'));
        }
        catch (IOException) { sb.AppendLine("(sin registro)"); }
        var crash = Path.Combine(EngineLog.Folder, "crash.log");
        if (File.Exists(crash))
        {
            sb.AppendLine("--- crash.log (final)");
            var text = File.ReadAllText(crash);
            sb.AppendLine(text[Math.Max(0, text.Length - 4000)..]);
        }
        var package = new Windows.ApplicationModel.DataTransfer.DataPackage();
        package.SetText(sb.ToString());
        Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(package);
        App.MainWindow?.Toast("Diagnóstico copiado", "Pégalo en tu mensaje o reporte.", InfoBarSeverity.Success, 3);
    }

    private UpdateInfo? _update;
    private CancellationTokenSource? _download;

    private void OnMirrorChanged(object sender, SelectionChangedEventArgs e)
    {
        if (MirrorBox.SelectedIndex < 0 || AppSettings.Current.Update.Mirror == MirrorBox.SelectedIndex) return;
        AppSettings.Current.Update.Mirror = MirrorBox.SelectedIndex;
        AppSettings.Current.Save();
    }

    private void OnResumeToggled(object sender, RoutedEventArgs e)
    {
        if (AppSettings.Current.Capture.ResumeLastSource == ResumeSwitch.IsOn) return;
        AppSettings.Current.Capture.ResumeLastSource = ResumeSwitch.IsOn;
        AppSettings.Current.Save();
    }

    private void OnNrAutoToggled(object sender, RoutedEventArgs e)
    {
        if (AppSettings.Current.Video.Nr.AutoInstall == NrAutoSwitch.IsOn) return;
        AppSettings.Current.Video.Nr.AutoInstall = NrAutoSwitch.IsOn;
        AppSettings.Current.Save();
    }

    private void OnAutoCheckToggled(object sender, RoutedEventArgs e)
    {
        if (AppSettings.Current.Update.AutoCheck == AutoCheckSwitch.IsOn) return;
        AppSettings.Current.Update.AutoCheck = AutoCheckSwitch.IsOn;
        AppSettings.Current.Save();
    }

    private async void OnCheckUpdates(object sender, RoutedEventArgs e)
    {
        CheckButton.IsEnabled = false;
        UpdateNotice.IsOpen = false;
        try
        {
            var update = await Updater.CheckAsync();
            if (update is null)
            {
                UpdatePanel.Visibility = Visibility.Collapsed;
                Notice("Estás al día", $"Blackvolt {Updater.CurrentVersion} es la última versión.", InfoBarSeverity.Success);
            }
            else
            {
                ShowUpdate(update);
            }
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or System.Text.Json.JsonException)
        {
            Notice("No se pudo comprobar", "Revisa tu conexión a internet e inténtalo de nuevo.", InfoBarSeverity.Warning);
        }
        finally
        {
            CheckButton.IsEnabled = true;
        }
    }

    private void ShowUpdate(UpdateInfo update)
    {
        _update = update;
        UpdateTitle.Text = $"Blackvolt {update.Version} disponible";
        UpdateNotes.Text = string.IsNullOrWhiteSpace(update.Notes) ? "Sin notas de la versión." : update.Notes.Trim();
        if (Uri.TryCreate(update.PageUrl, UriKind.Absolute, out var page)) ReleaseLink.NavigateUri = page;
        var mb = update.ZipSize / 1024.0 / 1024.0;
        if (mb > 0) InstallButton.SetValue(ToolTipService.ToolTipProperty, $"{mb:0} MB");
        UpdatePanel.Visibility = Visibility.Visible;
    }

    private async void OnInstallUpdate(object sender, RoutedEventArgs e)
    {
        if (_update is null) return;
        InstallButton.IsEnabled = false;
        SkipButton.IsEnabled = false;
        UpdateNotice.IsOpen = false;
        UpdateProgress.Visibility = Visibility.Visible;
        UpdateProgress.IsIndeterminate = true;
        _download = new CancellationTokenSource();
        var progress = new Progress<double>(p =>
        {
            UpdateProgress.IsIndeterminate = false;
            UpdateProgress.Value = p;
        });
        try
        {
            if (await Updater.DownloadAndInstallAsync(_update, progress, _download.Token))
            {
                AppSettings.Current.Save();
                Application.Current.Exit();
            }
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or IOException
                                       or InvalidDataException or UnauthorizedAccessException
                                       or System.ComponentModel.Win32Exception)
        {
            // Win32Exception: the user declined the administrator prompt.
            Notice("No se pudo actualizar", ex.Message, InfoBarSeverity.Error);
            InstallButton.IsEnabled = true;
            SkipButton.IsEnabled = true;
            UpdateProgress.Visibility = Visibility.Collapsed;
        }
    }

    private void OnSkipUpdate(object sender, RoutedEventArgs e)
    {
        if (_update is null) return;
        Updater.Skip(_update);
        UpdatePanel.Visibility = Visibility.Collapsed;
        Notice("Versión omitida", $"No volveremos a avisarte de la {_update.Version}.", InfoBarSeverity.Informational);
    }

    private void Notice(string title, string message, InfoBarSeverity severity)
    {
        UpdateNotice.Title = title;
        UpdateNotice.Message = message;
        UpdateNotice.Severity = severity;
        UpdateNotice.IsOpen = true;
    }
}
