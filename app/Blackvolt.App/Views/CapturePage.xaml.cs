using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace Blackvolt.App.Views;

public sealed partial class CapturePage : Page
{
    private const string NoAudio = "Sin audio";

    public CapturePage() => InitializeComponent();

    protected override async void OnNavigatedTo(NavigationEventArgs e) => await LoadAsync();

    private async void OnRefresh(object sender, RoutedEventArgs e) => await LoadAsync();

    private async Task LoadAsync()
    {
        Notice.IsOpen = false;
        StartButton.IsEnabled = false;
        IReadOnlyList<NativeCapture.CaptureDevice> devices;
        try
        {
            // Media Foundation enumeration can take a moment with many USB devices.
            devices = await Task.Run(NativeCapture.List);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            ShowNotice("El motor no está disponible", ex.Message, InfoBarSeverity.Error);
            return;
        }

        var settings = AppSettings.Current.Capture;
        var video = devices.Where(d => !d.IsAudio).Select(d => d.Name).Distinct().ToList();
        var audio = devices.Where(d => d.IsAudio).Select(d => d.Name).Distinct().ToList();

        VideoDevices.ItemsSource = video;
        AudioDevices.ItemsSource = new[] { NoAudio }.Concat(audio).ToList();
        VideoDevices.SelectedIndex = Math.Max(0, video.IndexOf(settings.VideoDevice));
        // Capture cards usually expose a matching audio device: prefer the remembered one, then a name match.
        var audioIndex = audio.IndexOf(settings.AudioDevice);
        if (audioIndex < 0 && video.Count > 0)
            audioIndex = audio.FindIndex(a => a.Contains(FirstWord(video[VideoDevices.SelectedIndex]), StringComparison.OrdinalIgnoreCase));
        AudioDevices.SelectedIndex = audioIndex >= 0 ? audioIndex + 1 : 0;
        Select(Resolution, settings.Height);
        Select(FrameRate, settings.Fps);

        LoadScreens();

        var any = video.Count > 0;
        EmptyCard.Visibility = any ? Visibility.Collapsed : Visibility.Visible;
        SetupCard.Visibility = any ? Visibility.Visible : Visibility.Collapsed;
        StartButton.IsEnabled = any;
    }

    private void OnStart(object sender, RoutedEventArgs e)
    {
        if (VideoDevices.SelectedItem is not string video) return;
        var audio = AudioDevices.SelectedItem as string;
        if (audio == NoAudio) audio = null;

        var settings = AppSettings.Current.Capture;
        settings.VideoDevice = video;
        settings.AudioDevice = audio ?? "";
        settings.Height = TagOf(Resolution);
        settings.Fps = TagOf(FrameRate);
        AppSettings.Current.Save();

        try
        {
            var url = NativeCapture.MakeUrl(video, audio, settings.Width, settings.Height, settings.Fps);
            Frame.Navigate(typeof(PlayerPage), new OpenMedia(url, video));
        }
        catch (EngineException ex)
        {
            ShowNotice("No se pudo preparar la captura", ex.Message, InfoBarSeverity.Error);
        }
    }

    private IReadOnlyList<ScreenSource> _screens = [];

    private void LoadScreens()
    {
        _screens = ScreenEnumerator.List(WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!));
        ScreenSources.ItemsSource = _screens.Select(s => s.Label).ToList();
        ScreenSources.SelectedIndex = _screens.Count > 0 ? 0 : -1;
        if (ScreenFps.SelectedIndex < 0) ScreenFps.SelectedIndex = 1;
    }

    private void OnStartScreen(object sender, RoutedEventArgs e)
    {
        if (ScreenSources.SelectedIndex < 0 || ScreenSources.SelectedIndex >= _screens.Count) return;
        var source = _screens[ScreenSources.SelectedIndex];
        var url = source.Url(TagOf(ScreenFps) is var fps and > 0 ? fps : 60, ScreenCursor.IsOn);
        Frame.Navigate(typeof(PlayerPage), new OpenMedia(url, source.Label));
    }

    private void ShowNotice(string title, string message, InfoBarSeverity severity)
    {
        Notice.Title = title;
        Notice.Message = message;
        Notice.Severity = severity;
        Notice.IsOpen = true;
    }

    private static string FirstWord(string name) => name.Split(' ', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? name;

    private static void Select(ComboBox box, int tag)
    {
        var index = box.Items.OfType<ComboBoxItem>().ToList().FindIndex(i => i.Tag as string == tag.ToString());
        box.SelectedIndex = Math.Max(0, index);
    }

    private static int TagOf(ComboBox box) =>
        box.SelectedItem is ComboBoxItem { Tag: string tag } && int.TryParse(tag, out var v) ? v : 0;
}
