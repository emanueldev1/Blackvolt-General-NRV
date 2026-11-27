using Blackvolt.App.Interop;
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

        var version = typeof(App).Assembly.GetName().Version?.ToString(3) ?? "0.1.0";
        VersionText.Text = $"Blackvolt {version}";
    }
}
