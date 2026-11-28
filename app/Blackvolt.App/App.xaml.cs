using Blackvolt.App.Interop;
using Microsoft.UI.Xaml;

namespace Blackvolt.App;

public partial class App : Application
{
    /// <summary>The main window, needed by file pickers (unpackaged apps must pass its HWND).</summary>
    internal static MainWindow? MainWindow { get; private set; }

    public App()
    {
        AppDomain.CurrentDomain.UnhandledException += (_, e) => WriteCrash("AppDomain", e.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, e) => WriteCrash("Task", e.Exception);
        UnhandledException += (_, e) => WriteCrash("XAML", e.Exception);
        InitializeComponent();
    }

    /// <summary>Appends an unhandled exception to %LOCALAPPDATA%\Blackvolt\crash.log.</summary>
    internal static void WriteCrash(string source, Exception? ex)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Blackvolt");
            Directory.CreateDirectory(dir);
            File.AppendAllText(Path.Combine(dir, "crash.log"),
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {source}: {ex}{Environment.NewLine}{Environment.NewLine}");
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
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
        // "Open with Blackvolt" / a file dropped on the exe: play it right away.
        var file = Environment.GetCommandLineArgs().Skip(1).FirstOrDefault(a => !a.StartsWith("--", StringComparison.Ordinal) && File.Exists(a));
        if (file is not null) MainWindow.Go("player", new Interop.OpenMedia(Path.GetFullPath(file), Path.GetFileName(file)));
        var args = Environment.GetCommandLineArgs();
        var pageAt = Array.FindIndex(args, a => a.Equals("--page", StringComparison.OrdinalIgnoreCase));
        if (file is null && pageAt > 0 && pageAt + 1 < args.Length) MainWindow.Go(args[pageAt + 1]);
        _ = Services.Updater.CheckInBackgroundAsync();
        // Like Veyra's package, DLSS NR should just be there: fetch it once on RTX GPUs. The
        // installer passes --install-nr when the user ticked "install DLSS NR".
        if (Environment.GetCommandLineArgs().Any(a => a.Equals("--install-nr", StringComparison.OrdinalIgnoreCase)))
            Services.NrRuntimeInstaller.InstallForThisGpu();
        else
            Services.NrRuntimeInstaller.AutoInstallIfNeeded();
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
            Services.EngineLog.Start();
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
