using System.Runtime.InteropServices;

namespace Blackvolt.App.Interop;

/// <summary>Capture cards and webcams (bv_capture_* in bv_api.h). They play through the regular player.</summary>
internal static partial class NativeCapture
{
    private const string Dll = "blackvolt_engine";

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct Device
    {
        public uint Size;
        public uint IsAudio;
        public fixed byte Name[128];
        public fixed byte Id[256];

        public readonly string NameText { get { fixed (byte* p = Name) return Utf8.Read(p, 128); } }
        public readonly string IdText { get { fixed (byte* p = Id) return Utf8.Read(p, 256); } }
    }

    [LibraryImport(Dll, EntryPoint = "bv_capture_devices")]
    private static partial NativeEngine.Result Devices([Out] Device[]? devices, uint capacity, out uint count);

    [LibraryImport(Dll, EntryPoint = "bv_capture_url", StringMarshalling = StringMarshalling.Utf8)]
    private static unsafe partial NativeEngine.Result Url(string video, string? audio, uint width, uint height, uint fps,
                                                          byte* output, uint outputSize);

    public sealed record CaptureDevice(string Name, string Id, bool IsAudio);

    public static IReadOnlyList<CaptureDevice> List()
    {
        if (Devices(null, 0, out var count) != NativeEngine.Result.Ok || count == 0) return [];
        var buffer = new Device[count];
        if (Devices(buffer, count, out count) != NativeEngine.Result.Ok) return [];
        return buffer.Take((int)Math.Min(count, (uint)buffer.Length))
                     .Select(d => new CaptureDevice(d.NameText, d.IdText, d.IsAudio != 0))
                     .ToList();
    }

    public static unsafe string MakeUrl(string video, string? audio, int width, int height, int fps)
    {
        var buffer = stackalloc byte[1024];
        Url(video, string.IsNullOrEmpty(audio) ? null : audio, (uint)Math.Max(0, width), (uint)Math.Max(0, height),
            (uint)Math.Max(0, fps), buffer, 1024).ThrowIfFailed();
        return Utf8.Read(buffer, 1024);
    }
}

/// <summary>What the player page should open when navigated to.</summary>
internal sealed record OpenMedia(string Url, string Title);
