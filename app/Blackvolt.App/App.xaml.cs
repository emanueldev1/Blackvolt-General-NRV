using Blackvolt.App.Interop;
using Microsoft.UI.Xaml;

namespace Blackvolt.App;

public partial class App : Application
{
    /// <summary>The main window, needed by file pickers (unpackaged apps must pass its HWND).</summary>
    internal static MainWindow? MainWindow { get; private set; }

    public App()
    {
        InitializeComponent();
    }

    /// <summary>Engine session shared by all pages. Null when no compatible GPU was found.</summary>
    internal static EngineSession? Engine { get; private set; }

    /// <summary>Reason the engine could not start, shown on the home page.</summary>
    public static string? EngineError { get; private set; }

    /// <summary>Paired PlayStation consoles (encrypted store). Null if Remote Play is unavailable.</summary>
    internal static ConsoleLibrary? Consoles { get; private set; }

    public static string? ConsolesError { get; private set; }

    /// <summary>Consoles imported automatically from chiaki-ng / Veyra on first launch.</summary>
    internal static uint ConsolesImportedOnStart { get; set; }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        StartEngine();
        OpenConsoles();
        MainWindow = new MainWindow();
        MainWindow.Closed += (_, _) =>
        {
            Consoles?.Dispose();
            Engine?.Dispose();
        };
        MainWindow.Activate();
    }

    /// <summary>
    /// Recreates the engine so NGX picks up newly imported runtimes (NGX reads its
    /// search paths once, at initialisation).
    /// </summary>
    internal static void RestartEngine()
    {
        Engine?.Dispose();
        Engine = null;
        StartEngine();
    }

    private static void OpenConsoles()
    {
        try
        {
            Consoles = ConsoleLibrary.Open();
            // First run: bring over pairings from chiaki-ng / Veyra so nobody has to pair again.
            if (Consoles.List().Count == 0) ConsolesImportedOnStart = Consoles.ImportInstalled();
        }
        catch (Exception ex) when (ex is EngineException or DllNotFoundException or EntryPointNotFoundException)
        {
            ConsolesError = ex.Message;
        }
    }

    private static void StartEngine()
    {
        EngineError = null;
        try
        {
            Runtimes.Configure();
            Engine = EngineSession.Start();
        }
        catch (Exception ex) when (ex is EngineException or DllNotFoundException or EntryPointNotFoundException)
        {
            EngineError = ex is DllNotFoundException
                ? "No se encontró blackvolt_engine.dll. Compila el motor antes de ejecutar la app (ver README)."
                : ex.Message;
        }
    }
}
