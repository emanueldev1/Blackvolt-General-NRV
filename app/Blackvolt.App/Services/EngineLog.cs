using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Blackvolt.App.Services;

/// <summary>
/// Writes the native engine's log to %LOCALAPPDATA%\Blackvolt\engine.log (the previous run is kept
/// as engine.old.log), so problems seen while testing can be diagnosed afterwards.
/// </summary>
internal static unsafe partial class EngineLog
{
    private static readonly object Gate = new();
    private static StreamWriter? _writer;
    private const long MaxBytes = 8L << 20;

    public static string Folder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Blackvolt");
    public static string FilePath => Path.Combine(Folder, "engine.log");

    [LibraryImport("blackvolt_engine", EntryPoint = "bv_set_log_callback")]
    private static partial void SetCallback(delegate* unmanaged[Cdecl]<int, byte*, nint, void> fn, nint user);

    public static void Start()
    {
        try
        {
            Directory.CreateDirectory(Folder);
            var old = Path.Combine(Folder, "engine.old.log");
            if (File.Exists(FilePath)) File.Copy(FilePath, old, overwrite: true);
            _writer = new StreamWriter(new FileStream(FilePath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false))
            {
                AutoFlush = true,
            };
            _writer.WriteLine($"Blackvolt {Updater.CurrentVersion} · {DateTime.Now:yyyy-MM-dd HH:mm:ss} · {Environment.OSVersion}");
            SetCallback(&OnLog, 0);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or DllNotFoundException or EntryPointNotFoundException)
        {
            _writer = null;
        }
    }

    /// <summary>App-side events worth having next to the engine's (installs, session starts...).</summary>
    public static void Write(string message) => Append("app", message);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnLog(int level, byte* message, nint user)
    {
        var text = message == null ? "" : Marshal.PtrToStringUTF8((nint)message) ?? "";
        Append(level switch { 0 => "debug", 1 => "info", 2 => "warn", _ => "error" }, text);
    }

    private static void Append(string level, string text)
    {
        lock (Gate)
        {
            if (_writer is null) return;
            try
            {
                if (_writer.BaseStream.Length > MaxBytes)
                {
                    _writer.BaseStream.SetLength(0);
                    _writer.WriteLine("(log truncated)");
                }
                _writer.WriteLine($"{DateTime.Now:HH:mm:ss.fff} [{level}] {text}");
            }
            catch (IOException) { }
        }
    }
}
