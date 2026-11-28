using System.Runtime.InteropServices;

namespace Blackvolt.App.Interop;

/// <summary>P/Invoke surface of blackvolt_engine.dll. Mirrors engine/include/blackvolt/bv_api.h.</summary>
internal static partial class NativeEngine
{
    private const string Dll = "blackvolt_engine";
    public const uint ApiVersion = 9;
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

        public readonly string NameText { get { fixed (byte* p = Name) return Utf8.Read(p, 128); } }
        public readonly string DriverText { get { fixed (byte* p = DriverVersion) return Utf8.Read(p, 32); } }
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

    public enum Feature : int
    {
        DlssSuperResolution = 0,
        DlssRayReconstruction = 1,
        DlssFrameGeneration = 2,
        DlssNeuralRendering = 3,
        RtxVideoSuperResolution = 4,
        RtxTrueHdr = 5,
    }

    public const int FeatureCount = 6;

    public enum RuntimeState : int
    {
        Missing = 0,
        Found = 1,
        Ready = 2,
        UnsupportedGpu = 3,
        DriverTooOld = 4,
        NgxUnavailable = 5,
    }

    public enum RuntimeOrigin : int { Unknown = 0, Official = 1, Community = 2 }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct RuntimeStatus
    {
        public uint Size;
        public Feature Feature;
        public RuntimeState State;
        public RuntimeOrigin Origin;
        public fixed byte Label[96];
        public fixed byte Path[520];

        public readonly string LabelText { get { fixed (byte* p = Label) return Utf8.Read(p, 96); } }
        public readonly string PathText { get { fixed (byte* p = Path) return Utf8.Read(p, 520); } }
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct RuntimeCandidate
    {
        public uint Size;
        public Feature Feature;
        public RuntimeOrigin Origin;
        public fixed byte Label[96];
        public fixed byte Path[520];
        public NrVariant NrVariant;

        public readonly string LabelText { get { fixed (byte* p = Label) return Utf8.Read(p, 96); } }
        public readonly string PathText { get { fixed (byte* p = Path) return Utf8.Read(p, 520); } }
    }

    /// <summary>DLSS NR builds: compatible (RTX 20-50) and NVIDIA original (RTX 50).</summary>
    public enum NrVariant : uint { Auto = 0, Compat = 1, Original = 2 }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct NrRuntimeInfo
    {
        public uint Size;
        public NrVariant Variant;
        public uint Installed;
        public RuntimeOrigin Origin;
        public ulong Bytes;
        public fixed byte Version[32];
        public fixed byte Label[96];
        public fixed byte Sha256[72];
        public fixed byte Path[520];

        public readonly string VersionText { get { fixed (byte* p = Version) return Utf8.Read(p, 32); } }
        public readonly string LabelText { get { fixed (byte* p = Label) return Utf8.Read(p, 96); } }
        public readonly string Sha256Text { get { fixed (byte* p = Sha256) return Utf8.Read(p, 72); } }
        public readonly string PathText { get { fixed (byte* p = Path) return Utf8.Read(p, 520); } }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GpuMemory
    {
        public uint Size;
        public ulong UsageBytes;
        public ulong BudgetBytes;
        public ulong DedicatedBytes;
    }

    [LibraryImport(Dll, EntryPoint = "bv_nr_runtime_get")]
    public static partial Result NrRuntimeGet(NrVariant variant, ref NrRuntimeInfo info);

    [LibraryImport(Dll, EntryPoint = "bv_nr_runtime_import", StringMarshalling = StringMarshalling.Utf8)]
    public static partial Result NrRuntimeImport(string sourcePath, NrVariant variant, ref NrRuntimeInfo info);

    [LibraryImport(Dll, EntryPoint = "bv_nr_runtime_remove")]
    public static partial Result NrRuntimeRemove(NrVariant variant);

    [LibraryImport(Dll, EntryPoint = "bv_engine_stage_history", StringMarshalling = StringMarshalling.Utf8)]
    public static unsafe partial Result GetStageHistory(nint engine, string stage, float* output, uint capacity, out uint count);

    [LibraryImport(Dll, EntryPoint = "bv_engine_gpu_memory")]
    public static partial Result GetGpuMemory(nint engine, ref GpuMemory memory);

    [LibraryImport(Dll, EntryPoint = "bv_set_runtime_paths", StringMarshalling = StringMarshalling.Utf8)]
    public static partial Result SetRuntimePaths(string bundledDir, string userDir, string manifest);

    [LibraryImport(Dll, EntryPoint = "bv_runtime_import", StringMarshalling = StringMarshalling.Utf8)]
    public static partial Result ImportRuntime(string sourcePath, Feature feature, out RuntimeStatus status);

    [LibraryImport(Dll, EntryPoint = "bv_runtime_scan", StringMarshalling = StringMarshalling.Utf8)]
    public static unsafe partial Result ScanRuntimes(string folder, RuntimeCandidate* output, uint capacity, out uint count);

    [LibraryImport(Dll, EntryPoint = "bv_engine_runtime_status")]
    public static partial Result GetRuntimeStatus(nint engine, Feature feature, out RuntimeStatus status);

    public enum PlayerState : int { Idle = 0, Opening = 1, Playing = 2, Paused = 3, Ended = 4, Error = 5 }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct PlayerInfo
    {
        public uint Size;
        public PlayerState State;
        public double PositionS;
        public double DurationS;
        public uint Width;
        public uint Height;
        public double Fps;
        public uint HardwareDecode;
        public uint HasAudio;
        public ulong FramesPresented;
        public ulong FramesDropped;
        public fixed byte Codec[32];
        public fixed byte Error[256];
        public uint Live;
        public ulong FramesGenerated;
        public int AudioTrack;
        public int SubtitleTrack;

        public readonly string CodecText { get { fixed (byte* p = Codec) return Utf8.Read(p, 32); } }
        public readonly string ErrorText { get { fixed (byte* p = Error) return Utf8.Read(p, 256); } }
    }

    [LibraryImport(Dll, EntryPoint = "bv_player_create")]
    public static partial Result PlayerCreate(nint engine, out nint player);

    [LibraryImport(Dll, EntryPoint = "bv_player_destroy")]
    public static partial void PlayerDestroy(nint player);

    [LibraryImport(Dll, EntryPoint = "bv_player_swapchain")]
    public static partial Result PlayerSwapChain(nint player, out nint swapChain);

    [LibraryImport(Dll, EntryPoint = "bv_player_resize")]
    public static partial Result PlayerResize(nint player, uint width, uint height, float scaleX, float scaleY);

    [LibraryImport(Dll, EntryPoint = "bv_player_open", StringMarshalling = StringMarshalling.Utf8)]
    public static partial Result PlayerOpen(nint player, string url);

    [LibraryImport(Dll, EntryPoint = "bv_player_play")]
    public static partial Result PlayerPlay(nint player);

    [LibraryImport(Dll, EntryPoint = "bv_player_pause")]
    public static partial Result PlayerPause(nint player);

    [LibraryImport(Dll, EntryPoint = "bv_player_seek")]
    public static partial Result PlayerSeek(nint player, double seconds);

    [LibraryImport(Dll, EntryPoint = "bv_player_set_volume")]
    public static partial Result PlayerSetVolume(nint player, float volume);

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct Track
    {
        public uint Size;
        public int Index;
        public uint Kind; // 0 audio, 1 subtitle
        public fixed byte Language[16];
        public fixed byte Title[96];
        public fixed byte Codec[32];

        public readonly string LanguageText { get { fixed (byte* p = Language) return Utf8.Read(p, 16); } }
        public readonly string TitleText { get { fixed (byte* p = Title) return Utf8.Read(p, 96); } }
        public readonly string CodecText { get { fixed (byte* p = Codec) return Utf8.Read(p, 32); } }
    }

    [LibraryImport(Dll, EntryPoint = "bv_player_tracks")]
    public static unsafe partial Result PlayerTracks(nint player, Track* output, uint capacity, out uint count);

    [LibraryImport(Dll, EntryPoint = "bv_player_select_track")]
    public static partial Result PlayerSelectTrack(nint player, uint kind, int index);

    [LibraryImport(Dll, EntryPoint = "bv_player_subtitle")]
    public static unsafe partial Result PlayerSubtitle(nint player, byte* output, uint size);

    [LibraryImport(Dll, EntryPoint = "bv_player_set_rate")]
    public static partial Result PlayerSetRate(nint player, double rate);

    [LibraryImport(Dll, EntryPoint = "bv_player_info_get")]
    public static partial Result PlayerInfoGet(nint player, out PlayerInfo info);

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

internal static unsafe class Utf8
{
    public static string Read(byte* p, int max)
    {
        int len = 0;
        while (len < max && p[len] != 0) len++;
        return System.Text.Encoding.UTF8.GetString(p, len);
    }
}
