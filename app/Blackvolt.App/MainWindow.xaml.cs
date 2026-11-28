using Blackvolt.App.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Animation;

namespace Blackvolt.App;

public sealed partial class MainWindow : Window
{
    private static readonly Dictionary<string, Type> Pages = new()
    {
        ["home"] = typeof(HomePage),
        ["player"] = typeof(PlayerPage),
        ["remote"] = typeof(RemotePlayPage),
        ["capture"] = typeof(CapturePage),
        ["export"] = typeof(ExportPage),
        ["runtimes"] = typeof(RuntimesPage),
        ["stats"] = typeof(StatsPage),
        ["settings"] = typeof(SettingsPage),
    };

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(TitleBar);
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1280, 800));
        ContentFrame.Navigate(typeof(HomePage));
        Services.NrRuntimeInstaller.Changed += () => DispatcherQueue.TryEnqueue(ShowNrInstall);
    }

    private bool _nrBarDismissed;

    private void ShowNrInstall()
    {
        var busy = Services.NrRuntimeInstaller.Busy;
        if (busy && !NrInstallBar.IsOpen && !_nrBarDismissed) NrInstallBar.IsOpen = true;
        NrInstallBar.Message = Services.NrRuntimeInstaller.Message;
        NrInstallProgress.Value = Services.NrRuntimeInstaller.Fraction;
        NrInstallProgress.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        if (!busy)
        {
            NrInstallBar.Severity = Services.NrRuntimeInstaller.LastFailed ? InfoBarSeverity.Warning : InfoBarSeverity.Success;
            if (!_nrBarDismissed) NrInstallBar.IsOpen = true;
        }
        else
        {
            NrInstallBar.Severity = InfoBarSeverity.Informational;
        }
        NrInstallBar.CloseButtonClick -= OnNrBarClosed;
        NrInstallBar.CloseButtonClick += OnNrBarClosed;
    }

    private void OnNrBarClosed(InfoBar sender, object args) => _nrBarDismissed = Services.NrRuntimeInstaller.Busy;

    /// <summary>Hides the navigation pane while streaming: the game gets the whole window.</summary>
    internal void SetImmersive(bool on)
    {
        Nav.IsPaneVisible = !on;
        Nav.IsPaneToggleButtonVisible = !on;
    }

    /// <summary>Selects a section of the navigation pane by its tag, as if the user clicked it.</summary>
    internal void Go(string tag)
    {
        if (tag == "settings")
        {
            Nav.SelectedItem = Nav.SettingsItem;
            return;
        }
        var item = Nav.MenuItems.OfType<NavigationViewItem>().FirstOrDefault(i => i.Tag as string == tag);
        if (item is not null) Nav.SelectedItem = item;
        else if (Pages.TryGetValue(tag, out var page)) ContentFrame.Navigate(page);
    }

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        var tag = args.IsSettingsSelected ? "settings" : (args.SelectedItemContainer?.Tag as string ?? "home");
        if (!Pages.TryGetValue(tag, out var page)) return;
        var label = (args.SelectedItemContainer?.Content as string) ?? "";
        ContentFrame.Navigate(page, label, new EntranceNavigationTransitionInfo());
    }
}
