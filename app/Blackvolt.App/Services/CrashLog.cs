using System.Runtime.InteropServices;
using System.Text;

namespace Blackvolt.App.Services;

/// <summary>
/// Writes unhandled exceptions to %LOCALAPPDATA%\Blackvolt\crash.log and tells the user, so a
/// failure at startup is never silent.
/// </summary>
internal static partial class CrashLog
{
    public static string FilePath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Blackvolt", "crash.log");

    private static int _shown;

    public static void Install()
    {
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception ex) Fatal("Error no controlado", ex);
        };
        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            Write("Tarea con error no observado", e.Exception);
            e.SetObserved();
        };
    }

    public static void Write(string context, Exception ex)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            var text = new StringBuilder()
                .AppendLine($"==== {DateTime.Now:yyyy-MM-dd HH:mm:ss} · Blackvolt {Updater.CurrentVersion} · {context}")
                .AppendLine($"Windows {Environment.OSVersion.Version} · {RuntimeInformation.FrameworkDescription} · {AppContext.BaseDirectory}")
                .AppendLine(ex.ToString())
                .AppendLine();
            File.AppendAllText(FilePath, text.ToString());
        }
        catch (Exception)
        {
            // Nothing else we can do: never throw from the crash handler.
        }
    }

    /// <summary>Logs and shows a native message box (works even when XAML failed to load).</summary>
    public static void Fatal(string context, Exception ex)
    {
        Write(context, ex);
        if (Interlocked.Exchange(ref _shown, 1) != 0) return;
        var message = $"Blackvolt no pudo continuar.\n\n{ex.GetType().Name}: {ex.Message}\n\n" +
                      $"Detalles guardados en:\n{FilePath}";
        MessageBoxW(0, message, "Blackvolt", 0x10 /* MB_ICONERROR */);
    }

    [LibraryImport("user32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int MessageBoxW(nint hwnd, string text, string caption, uint type);
}
