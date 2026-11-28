using System.Runtime.InteropServices;
using Microsoft.UI.Xaml.Controls;

namespace Blackvolt.App.Interop;

/// <summary>One native player bound to a SwapChainPanel.</summary>
internal sealed class PlayerSession : IDisposable
{
    private nint _handle;

    internal nint Handle => _handle;

    private PlayerSession(nint handle) => _handle = handle;

    public static PlayerSession Create(EngineSession engine, SwapChainPanel panel)
    {
        NativeEngine.PlayerCreate(engine.Handle, out var handle).ThrowIfFailed();
        var session = new PlayerSession(handle);
        NativeEngine.PlayerSwapChain(handle, out var swapChain).ThrowIfFailed();
        SwapChainPanelInterop.SetSwapChain(panel, swapChain);
        return session;
    }

    public void Open(string path) => NativeEngine.PlayerOpen(_handle, path).ThrowIfFailed();
    public void Play() => NativeEngine.PlayerPlay(_handle);
    public void Pause() => NativeEngine.PlayerPause(_handle);
    public void Seek(double seconds) => NativeEngine.PlayerSeek(_handle, seconds);
    public void SetVolume(float volume) => NativeEngine.PlayerSetVolume(_handle, volume);
    public void SetRate(double rate) => NativeEngine.PlayerSetRate(_handle, rate);
    public void SelectTrack(uint kind, int index) => NativeEngine.PlayerSelectTrack(_handle, kind, index);

    public unsafe IReadOnlyList<NativeEngine.Track> Tracks()
    {
        NativeEngine.PlayerTracks(_handle, null, 0, out var count);
        if (count == 0) return [];
        var buffer = new NativeEngine.Track[count];
        fixed (NativeEngine.Track* p = buffer)
            NativeEngine.PlayerTracks(_handle, p, count, out count);
        return buffer.Take((int)count).ToList();
    }

    private readonly byte[] _subtitle = new byte[2048];

    /// <summary>Embedded subtitle text showing now ("" when none).</summary>
    public unsafe string Subtitle()
    {
        fixed (byte* p = _subtitle)
        {
            if (NativeEngine.PlayerSubtitle(_handle, p, (uint)_subtitle.Length) != NativeEngine.Result.Ok) return "";
            return Utf8.Read(p, _subtitle.Length);
        }
    }
    public void Resize(uint width, uint height, float scaleX, float scaleY) =>
        NativeEngine.PlayerResize(_handle, width, height, scaleX, scaleY);

    public void SetEnhance(Services.VideoSettings settings) =>
        NativeEnhance.PlayerSetEnhance(_handle, NativeEnhance.From(settings));

    public NativeEnhance.Status EnhanceStatus
    {
        get
        {
            var status = NativeEnhance.NewStatus();
            NativeEnhance.PlayerEnhanceStatus(_handle, ref status);
            return status;
        }
    }

    public NativeEngine.PlayerInfo Info
    {
        get
        {
            NativeEngine.PlayerInfoGet(_handle, out var info);
            return info;
        }
    }

    public void Dispose()
    {
        if (_handle != 0)
        {
            NativeEngine.PlayerDestroy(_handle);
            _handle = 0;
        }
    }
}

/// <summary>ISwapChainPanelNative::SetSwapChain without a COM interop assembly.</summary>
internal static unsafe class SwapChainPanelInterop
{
    private static readonly Guid IidSwapChainPanelNative = new("63aad0b8-7c24-40ff-85a8-640d944cc325");

    public static void SetSwapChain(SwapChainPanel panel, nint swapChain)
    {
        var unknown = ((WinRT.IWinRTObject)panel).NativeObject.ThisPtr;
        var iid = IidSwapChainPanelNative;
        Marshal.ThrowExceptionForHR(Marshal.QueryInterface(unknown, in iid, out var native));
        try
        {
            // vtable: IUnknown (3 slots), then SetSwapChain.
            var vtbl = *(void***)native;
            var setSwapChain = (delegate* unmanaged[Stdcall]<nint, nint, int>)vtbl[3];
            Marshal.ThrowExceptionForHR(setSwapChain(native, swapChain));
        }
        finally
        {
            Marshal.Release(native);
        }
    }
}
