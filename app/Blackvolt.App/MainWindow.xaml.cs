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
        ["capture"] = typeof(ComingSoonPage),
        ["export"] = typeof(ComingSoonPage),
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
    }

    /// <summary>Hides the navigation pane while streaming: the game gets the whole window.</summary>
    internal void SetImmersive(bool on)
    {
        Nav.IsPaneVisible = !on;
        Nav.IsPaneToggleButtonVisible = !on;
    }

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        var tag = args.IsSettingsSelected ? "settings" : (args.SelectedItemContainer?.Tag as string ?? "home");
        if (!Pages.TryGetValue(tag, out var page)) return;
        var label = (args.SelectedItemContainer?.Content as string) ?? "";
        ContentFrame.Navigate(page, label, new EntranceNavigationTransitionInfo());
    }
}
