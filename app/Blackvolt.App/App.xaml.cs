using Blackvolt.App.Interop;
using Microsoft.UI.Xaml;

namespace Blackvolt.App;

public partial class App : Application
{
    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
    }

    /// <summary>Engine session shared by all pages. Null when no compatible GPU was found.</summary>
    public static EngineSession? Engine { get; private set; }

    /// <summary>Reason the engine could not start, shown on the home page.</summary>
    public static string? EngineError { get; private set; }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            Engine = EngineSession.Start();
        }
        catch (Exception ex) when (ex is EngineException or DllNotFoundException or EntryPointNotFoundException)
        {
            EngineError = ex is DllNotFoundException
                ? "No se encontró blackvolt_engine.dll. Compila el motor antes de ejecutar la app (ver README)."
                : ex.Message;
        }

        _window = new MainWindow();
        _window.Closed += (_, _) => Engine?.Dispose();
        _window.Activate();
    }
}
