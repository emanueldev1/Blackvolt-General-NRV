using System.Runtime.InteropServices;
using Microsoft.UI.Xaml.Controls;

namespace Blackvolt.App.Interop;

/// <summary>P/Invoke surface for PlayStation Remote Play. Mirrors the bv_consoles / bv_discovery /
/// bv_pairing / bv_stream section of bv_api.h.</summary>
internal static partial class NativeRemote
{
    private const string Dll = "blackvolt_engine";

    public enum ConsoleKind : uint { PS4 = 0, PS5 = 1 }
    public enum DiscoveredState : uint { Unknown = 0, Ready = 1, Standby = 2 }
    public enum PairingState : uint { Running = 0, Succeeded = 1, Failed = 2, Canceled = 3 }
    public enum StreamState : uint { Idle = 0, Waking = 1, Connecting = 2, Streaming = 3, Reconnecting = 4, Ended = 5, Error = 6 }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ConsoleInfo
    {
        public uint Size;
        public fixed byte Nickname[64];
        public fixed byte Host[64];
        public fixed byte ConsoleId[16];
        public ConsoleKind Kind;
        public fixed byte Source[16];
        public uint HasAccountId;
        public uint PrefWidth, PrefHeight, PrefFps, PrefBitrateKbps, PrefCodec;

        public readonly string NicknameText { get { fixed (byte* p = Nickname) return Utf8.Read(p, 64); } }
        public readonly string HostText { get { fixed (byte* p = Host) return Utf8.Read(p, 64); } }
        public readonly string ConsoleIdText { get { fixed (byte* p = ConsoleId) return Utf8.Read(p, 16); } }
        public readonly string SourceText { get { fixed (byte* p = Source) return Utf8.Read(p, 16); } }
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct DiscoveredConsole
    {
        public uint Size;
        public fixed byte Name[64];
        public fixed byte Host[64];
        public fixed byte ConsoleId[16];
        public ConsoleKind Kind;
        public DiscoveredState State;
        public fixed byte RunningApp[96];
        public fixed byte SystemVersion[32];
        public int PairedIndex;

        public readonly string NameText { get { fixed (byte* p = Name) return Utf8.Read(p, 64); } }
        public readonly string HostText { get { fixed (byte* p = Host) return Utf8.Read(p, 64); } }
        public readonly string ConsoleIdText { get { fixed (byte* p = ConsoleId) return Utf8.Read(p, 16); } }
        public readonly string RunningAppText { get { fixed (byte* p = RunningApp) return Utf8.Read(p, 96); } }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct StreamSettings
    {
        public uint Size;
        public uint Width, Height, Fps, BitrateKbps, Codec;
        public uint AutoReconnect, WakeIfAsleep, DualSenseFeatures;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct StreamInfo
    {
        public uint Size;
        public StreamState State;
        public uint Width, Height;
        public double Fps;
        public double BitrateMbps;
        public double RttMs;
        public double DecodeMs;
        public ulong FramesPresented;
        public ulong FramesLost;
        public ulong FramesRecovered;
        public uint Reconnects;
        public uint HardwareDecode;
        public uint GamepadConnected;
        public fixed byte Gamepad[64];
        public fixed byte Codec[32];
        public fixed byte Error[256];

        public readonly string GamepadText { get { fixed (byte* p = Gamepad) return Utf8.Read(p, 64); } }
        public readonly string CodecText { get { fixed (byte* p = Codec) return Utf8.Read(p, 32); } }
        public readonly string ErrorText { get { fixed (byte* p = Error) return Utf8.Read(p, 256); } }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GamepadState
    {
        public uint Size;
        public uint Buttons;
        public byte L2, R2;
        public short LeftX, LeftY, RightX, RightY;
    }

    [LibraryImport(Dll, EntryPoint = "bv_consoles_open", StringMarshalling = StringMarshalling.Utf8)]
    public static partial NativeEngine.Result ConsolesOpen(string path, out nint consoles);
    [LibraryImport(Dll, EntryPoint = "bv_consoles_close")]
    public static partial void ConsolesClose(nint consoles);
    [LibraryImport(Dll, EntryPoint = "bv_consoles_count")]
    public static partial uint ConsolesCount(nint consoles);
    [LibraryImport(Dll, EntryPoint = "bv_consoles_get")]
    public static partial NativeEngine.Result ConsolesGet(nint consoles, uint index, out ConsoleInfo info);
    [LibraryImport(Dll, EntryPoint = "bv_consoles_remove")]
    public static partial NativeEngine.Result ConsolesRemove(nint consoles, uint index);
    [LibraryImport(Dll, EntryPoint = "bv_consoles_update", StringMarshalling = StringMarshalling.Utf8)]
    public static partial NativeEngine.Result ConsolesUpdate(nint consoles, uint index, string? nickname, string? host);
    [LibraryImport(Dll, EntryPoint = "bv_consoles_import_installed")]
    public static partial NativeEngine.Result ConsolesImportInstalled(nint consoles, out uint added);
    [LibraryImport(Dll, EntryPoint = "bv_consoles_import_path", StringMarshalling = StringMarshalling.Utf8)]
    public static partial NativeEngine.Result ConsolesImportPath(nint consoles, string path, out uint added);
    [LibraryImport(Dll, EntryPoint = "bv_consoles_wake")]
    public static unsafe partial NativeEngine.Result ConsolesWake(nint consoles, uint index, byte* error, uint errorSize);

    [LibraryImport(Dll, EntryPoint = "bv_discovery_start")]
    public static partial NativeEngine.Result DiscoveryStart(nint known, out nint discovery);
    [LibraryImport(Dll, EntryPoint = "bv_discovery_stop")]
    public static partial void DiscoveryStop(nint discovery);
    [LibraryImport(Dll, EntryPoint = "bv_discovery_list")]
    public static unsafe partial NativeEngine.Result DiscoveryList(nint discovery, DiscoveredConsole* items, uint capacity, out uint count);

    [LibraryImport(Dll, EntryPoint = "bv_pairing_start", StringMarshalling = StringMarshalling.Utf8)]
    public static partial NativeEngine.Result PairingStart(nint consoles, string host, ConsoleKind kind, string? accountId, uint pin, out nint pairing);
    [LibraryImport(Dll, EntryPoint = "bv_pairing_poll")]
    public static unsafe partial NativeEngine.Result PairingPoll(nint pairing, out PairingState state, byte* error, uint errorSize);
    [LibraryImport(Dll, EntryPoint = "bv_pairing_destroy")]
    public static partial void PairingDestroy(nint pairing);

    [LibraryImport(Dll, EntryPoint = "bv_stream_create")]
    public static partial NativeEngine.Result StreamCreate(nint engine, out nint stream);
    [LibraryImport(Dll, EntryPoint = "bv_stream_destroy")]
    public static partial void StreamDestroy(nint stream);
    [LibraryImport(Dll, EntryPoint = "bv_stream_swapchain")]
    public static partial NativeEngine.Result StreamSwapChain(nint stream, out nint swapChain);
    [LibraryImport(Dll, EntryPoint = "bv_stream_resize")]
    public static partial NativeEngine.Result StreamResize(nint stream, uint width, uint height, float scaleX, float scaleY);
    [LibraryImport(Dll, EntryPoint = "bv_stream_start")]
    public static partial NativeEngine.Result StreamStart(nint stream, nint consoles, uint index, in StreamSettings settings);
    [LibraryImport(Dll, EntryPoint = "bv_stream_stop")]
    public static partial NativeEngine.Result StreamStop(nint stream);
    [LibraryImport(Dll, EntryPoint = "bv_stream_info_get")]
    public static partial NativeEngine.Result StreamInfoGet(nint stream, out StreamInfo info);
    [LibraryImport(Dll, EntryPoint = "bv_stream_set_volume")]
    public static partial NativeEngine.Result StreamSetVolume(nint stream, float volume);
    [LibraryImport(Dll, EntryPoint = "bv_stream_set_input")]
    public static partial NativeEngine.Result StreamSetInput(nint stream, in GamepadState state);
    [LibraryImport(Dll, EntryPoint = "bv_stream_ps_button")]
    public static partial NativeEngine.Result StreamPsButton(nint stream);
    [LibraryImport(Dll, EntryPoint = "bv_stream_console_sleep")]
    public static partial NativeEngine.Result StreamConsoleSleep(nint stream);
    [LibraryImport(Dll, EntryPoint = "bv_stream_request_keyframe")]
    public static partial NativeEngine.Result StreamRequestKeyframe(nint stream);
}

/// <summary>A paired console as the UI shows it.</summary>
internal sealed record PairedConsole(
    uint Index, string Nickname, string Host, string ConsoleId, NativeRemote.ConsoleKind Kind, string Source,
    bool HasAccountId, NativeRemote.ConsoleInfo Raw);

/// <summary>The encrypted list of paired consoles (one per app, opened at startup).</summary>
internal sealed class ConsoleLibrary : IDisposable
{
    private nint _handle;

    private ConsoleLibrary(nint handle) => _handle = handle;

    public nint Handle => _handle;

    public static string StorePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Blackvolt", "remoteplay", "consoles.dat");

    public static ConsoleLibrary Open()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(StorePath)!);
        NativeRemote.ConsolesOpen(StorePath, out var handle).ThrowIfFailed();
        return new ConsoleLibrary(handle);
    }

    public IReadOnlyList<PairedConsole> List()
    {
        var count = NativeRemote.ConsolesCount(_handle);
        var list = new List<PairedConsole>((int)count);
        for (uint i = 0; i < count; i++)
        {
            if (NativeRemote.ConsolesGet(_handle, i, out var c) != NativeEngine.Result.Ok) continue;
            list.Add(new PairedConsole(i, c.NicknameText, c.HostText, c.ConsoleIdText, c.Kind, c.SourceText,
                                       c.HasAccountId != 0, c));
        }
        return list;
    }

    public uint ImportInstalled()
    {
        NativeRemote.ConsolesImportInstalled(_handle, out var added).ThrowIfFailed();
        return added;
    }

    /// <summary>Returns null when the file or folder had no pairings.</summary>
    public uint? ImportPath(string path)
    {
        var r = NativeRemote.ConsolesImportPath(_handle, path, out var added);
        if (r == NativeEngine.Result.Unsupported) return null;
        r.ThrowIfFailed();
        return added;
    }

    public void Update(uint index, string? nickname, string? host) =>
        NativeRemote.ConsolesUpdate(_handle, index, nickname, host).ThrowIfFailed();

    public void Remove(uint index) => NativeRemote.ConsolesRemove(_handle, index).ThrowIfFailed();

    /// <summary>Null on success, otherwise the reason.</summary>
    public unsafe string? Wake(uint index)
    {
        var buffer = stackalloc byte[256];
        buffer[0] = 0;
        return NativeRemote.ConsolesWake(_handle, index, buffer, 256) == NativeEngine.Result.Ok ? null : Utf8.Read(buffer, 256);
    }

    public void Dispose()
    {
        if (_handle != 0)
        {
            NativeRemote.ConsolesClose(_handle);
            _handle = 0;
        }
    }
}

internal sealed class ConsoleDiscovery : IDisposable
{
    private nint _handle;

    private ConsoleDiscovery(nint handle) => _handle = handle;

    public static ConsoleDiscovery Start(ConsoleLibrary? known)
    {
        NativeRemote.DiscoveryStart(known?.Handle ?? 0, out var handle).ThrowIfFailed();
        return new ConsoleDiscovery(handle);
    }

    public unsafe IReadOnlyList<NativeRemote.DiscoveredConsole> List()
    {
        var items = new NativeRemote.DiscoveredConsole[32];
        uint count;
        fixed (NativeRemote.DiscoveredConsole* p = items)
        {
            if (NativeRemote.DiscoveryList(_handle, p, (uint)items.Length, out count) != NativeEngine.Result.Ok) return [];
        }
        return items.Take((int)Math.Min(count, (uint)items.Length)).ToArray();
    }

    public void Dispose()
    {
        if (_handle != 0)
        {
            NativeRemote.DiscoveryStop(_handle);
            _handle = 0;
        }
    }
}

internal sealed class ConsolePairing : IDisposable
{
    private nint _handle;

    private ConsolePairing(nint handle) => _handle = handle;

    public static ConsolePairing Start(ConsoleLibrary library, string host, NativeRemote.ConsoleKind kind, string? accountId, uint pin)
    {
        NativeRemote.PairingStart(library.Handle, host, kind, string.IsNullOrWhiteSpace(accountId) ? null : accountId.Trim(),
                                  pin, out var handle).ThrowIfFailed();
        return new ConsolePairing(handle);
    }

    public unsafe (NativeRemote.PairingState State, string Error) Poll()
    {
        var buffer = stackalloc byte[256];
        buffer[0] = 0;
        NativeRemote.PairingPoll(_handle, out var state, buffer, 256);
        return (state, Utf8.Read(buffer, 256));
    }

    public void Dispose()
    {
        if (_handle != 0)
        {
            NativeRemote.PairingDestroy(_handle);
            _handle = 0;
        }
    }
}

/// <summary>A Remote Play stream rendered into a SwapChainPanel.</summary>
internal sealed class RemoteStream : IDisposable
{
    private nint _handle;

    private RemoteStream(nint handle) => _handle = handle;

    public static RemoteStream Create(EngineSession engine, SwapChainPanel panel)
    {
        NativeRemote.StreamCreate(engine.Handle, out var handle).ThrowIfFailed();
        var stream = new RemoteStream(handle);
        NativeRemote.StreamSwapChain(handle, out var swapChain).ThrowIfFailed();
        SwapChainPanelInterop.SetSwapChain(panel, swapChain);
        return stream;
    }

    public void Start(ConsoleLibrary library, uint index, NativeRemote.StreamSettings settings)
    {
        settings.Size = (uint)Marshal.SizeOf<NativeRemote.StreamSettings>();
        NativeRemote.StreamStart(_handle, library.Handle, index, settings).ThrowIfFailed();
    }

    public void Stop() => NativeRemote.StreamStop(_handle);
    public void Resize(uint w, uint h, float sx, float sy) => NativeRemote.StreamResize(_handle, w, h, sx, sy);
    public void SetVolume(float v) => NativeRemote.StreamSetVolume(_handle, v);
    public void PsButton() => NativeRemote.StreamPsButton(_handle);
    public void SleepConsole() => NativeRemote.StreamConsoleSleep(_handle);
    public void RequestKeyframe() => NativeRemote.StreamRequestKeyframe(_handle);
    public NativeEngine.Result Screenshot(string path) => NativeEnhance.StreamScreenshot(_handle, path);

    public void SetEnhance(Services.VideoSettings settings) =>
        NativeEnhance.StreamSetEnhance(_handle, NativeEnhance.From(settings));

    public NativeEnhance.Status EnhanceStatus
    {
        get
        {
            var status = NativeEnhance.NewStatus();
            NativeEnhance.StreamEnhanceStatus(_handle, ref status);
            return status;
        }
    }

    public void SetInput(NativeRemote.GamepadState state)
    {
        state.Size = (uint)Marshal.SizeOf<NativeRemote.GamepadState>();
        NativeRemote.StreamSetInput(_handle, state);
    }

    public NativeRemote.StreamInfo Info
    {
        get
        {
            NativeRemote.StreamInfoGet(_handle, out var info);
            return info;
        }
    }

    public void Dispose()
    {
        if (_handle != 0)
        {
            NativeRemote.StreamDestroy(_handle);
            _handle = 0;
        }
    }
}
