using System.Runtime.InteropServices;

namespace Blackvolt.App.Interop;

/// <summary>Offline transcode (bv_export_* in bv_api.h).</summary>
internal static partial class NativeExport
{
    private const string Dll = "blackvolt_engine";

    public enum Codec : int { H264 = 0, Hevc = 1, Av1 = 2 }
    public enum State : int { Idle = 0, Running = 1, Done = 2, Failed = 3, Cancelled = 4 }

    [StructLayout(LayoutKind.Sequential)]
    public struct Options
    {
        public uint Size;
        public nint Input;
        public nint Output;
        public Codec Codec;
        public uint Width;
        public uint Height;
        public uint Quality;
        public uint Audio;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct Status
    {
        public uint Size;
        public State State;
        public double Progress;
        public double Fps;
        public double ElapsedS;
        public double EtaS;
        public ulong Frames;
        public uint Hardware;
        public uint Width;
        public uint Height;
        public fixed byte Encoder[32];
        public fixed byte Message[256];

        public readonly string EncoderText { get { fixed (byte* p = Encoder) return Utf8.Read(p, 32); } }
        public readonly string MessageText { get { fixed (byte* p = Message) return Utf8.Read(p, 256); } }
    }

    [LibraryImport(Dll, EntryPoint = "bv_export_start")]
    private static partial NativeEngine.Result NativeStart(in Options options, out nint job);

    [LibraryImport(Dll, EntryPoint = "bv_export_status_get")]
    private static partial NativeEngine.Result NativeStatus(nint job, ref Status status);

    [LibraryImport(Dll, EntryPoint = "bv_export_cancel")]
    private static partial NativeEngine.Result NativeCancel(nint job);

    [LibraryImport(Dll, EntryPoint = "bv_export_destroy")]
    private static partial void NativeDestroy(nint job);

    [LibraryImport(Dll, EntryPoint = "bv_export_has_encoder", StringMarshalling = StringMarshalling.Utf8)]
    public static partial uint HasEncoder(string name);

    /// <summary>A running export. Dispose cancels it and waits for the worker.</summary>
    public sealed class Job : IDisposable
    {
        private nint _handle;

        private Job(nint handle) => _handle = handle;

        public static Job Start(string input, string output, Codec codec, int width, int height, int quality, bool audio)
        {
            var inPtr = Marshal.StringToCoTaskMemUTF8(input);
            var outPtr = Marshal.StringToCoTaskMemUTF8(output);
            try
            {
                var o = new Options
                {
                    Size = (uint)Marshal.SizeOf<Options>(),
                    Input = inPtr,
                    Output = outPtr,
                    Codec = codec,
                    Width = (uint)Math.Max(0, width),
                    Height = (uint)Math.Max(0, height),
                    Quality = (uint)Math.Clamp(quality, 0, 100),
                    Audio = audio ? 1u : 0u,
                };
                NativeStart(o, out var handle).ThrowIfFailed();
                return new Job(handle);
            }
            finally
            {
                Marshal.FreeCoTaskMem(inPtr);
                Marshal.FreeCoTaskMem(outPtr);
            }
        }

        public Status Current
        {
            get
            {
                var s = new Status { Size = (uint)Marshal.SizeOf<Status>() };
                if (_handle != 0) NativeStatus(_handle, ref s);
                return s;
            }
        }

        public void Cancel()
        {
            if (_handle != 0) NativeCancel(_handle);
        }

        public void Dispose()
        {
            if (_handle == 0) return;
            NativeDestroy(_handle);
            _handle = 0;
        }
    }
}
