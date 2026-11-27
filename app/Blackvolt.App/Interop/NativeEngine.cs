using System.Runtime.InteropServices;

namespace Blackvolt.App.Interop;

/// <summary>P/Invoke surface of blackvolt_engine.dll. Mirrors engine/include/blackvolt/bv_api.h.</summary>
internal static partial class NativeEngine
{
    private const string Dll = "blackvolt_engine";
    public const uint ApiVersion = 1;
    public const uint BestAdapter = uint.MaxValue;

    public enum Result : int
    {
        Ok = 0,
        InvalidArgument = 1,
        NotInitialized = 2,
        NoCompatibleGpu = 3,
        DeviceLost = 4,
        Unsupported = 5,
        Internal = 99,
    }

    public enum Vendor : int { Unknown = 0, Nvidia = 1, Amd = 2, Intel = 3 }

    [Flags]
    public enum Capabilities : uint
    {
        None = 0,
        D3D12 = 1 << 0,
        RtxVideoSuperResolution = 1 << 1,
        RtxTrueHdr = 1 << 2,
        Nvenc = 1 << 3,
        Nvdec = 1 << 4,
        OpticalFlow = 1 << 5,
        Tearing = 1 << 6,
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct AdapterInfo
    {
        public uint Size;
        public uint Index;
        public Vendor Vendor;
        public uint VendorId;
        public uint DeviceId;
        public ulong DedicatedVramBytes;
        public Capabilities Capabilities;
        public fixed byte Name[128];
        public fixed byte DriverVersion[32];

        public readonly string NameText { get { fixed (byte* p = Name) return Utf8(p, 128); } }
        public readonly string DriverText { get { fixed (byte* p = DriverVersion) return Utf8(p, 32); } }

        private static string Utf8(byte* p, int max)
        {
            int len = 0;
            while (len < max && p[len] != 0) len++;
            return System.Text.Encoding.UTF8.GetString(p, len);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct StageStats
    {
        public uint Size;
        public uint Samples;
        public double AvgUs;
        public double P50Us;
        public double P95Us;
        public double P99Us;
        public double MaxUs;
    }

    [LibraryImport(Dll, EntryPoint = "bv_api_version")]
    public static partial uint GetApiVersion();

    [LibraryImport(Dll, EntryPoint = "bv_enum_adapters")]
    public static unsafe partial Result EnumAdapters(AdapterInfo* output, uint capacity, out uint count);

    [LibraryImport(Dll, EntryPoint = "bv_engine_create")]
    public static partial Result CreateEngine(uint adapterIndex, out nint engine);

    [LibraryImport(Dll, EntryPoint = "bv_engine_destroy")]
    public static partial void DestroyEngine(nint engine);

    [LibraryImport(Dll, EntryPoint = "bv_engine_adapter")]
    public static partial Result GetEngineAdapter(nint engine, out AdapterInfo info);

    [LibraryImport(Dll, EntryPoint = "bv_engine_stage_stats", StringMarshalling = StringMarshalling.Utf8)]
    public static partial Result GetStageStats(nint engine, string stage, out StageStats stats);
}
