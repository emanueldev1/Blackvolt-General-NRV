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
