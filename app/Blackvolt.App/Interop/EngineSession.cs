namespace Blackvolt.App.Interop;

/// <summary>Owns one native engine instance. Dispose to release the GPU device.</summary>
internal sealed class EngineSession : IDisposable
{
    private nint _handle;

    private EngineSession(nint handle, GpuAdapter adapter)
    {
        _handle = handle;
        Adapter = adapter;
    }

    public GpuAdapter Adapter { get; }

    public static unsafe IReadOnlyList<GpuAdapter> ListAdapters()
    {
        NativeEngine.EnumAdapters(null, 0, out uint count).ThrowIfFailed();
        if (count == 0) return [];
        var buffer = new NativeEngine.AdapterInfo[count];
        fixed (NativeEngine.AdapterInfo* p = buffer)
        {
            NativeEngine.EnumAdapters(p, count, out count).ThrowIfFailed();
        }
        return buffer.Take((int)count).Select(GpuAdapter.From).ToList();
    }

    public static EngineSession Start(uint adapterIndex = NativeEngine.BestAdapter)
    {
        if (NativeEngine.GetApiVersion() != NativeEngine.ApiVersion)
            throw new EngineException("La versión del motor (blackvolt_engine.dll) no coincide con la app.");

        NativeEngine.CreateEngine(adapterIndex, out nint handle).ThrowIfFailed();
        NativeEngine.GetEngineAdapter(handle, out var info).ThrowIfFailed();
        return new EngineSession(handle, GpuAdapter.From(info));
    }

    public NativeEngine.StageStats GetStats(string stage)
    {
        ObjectDisposedException.ThrowIf(_handle == 0, this);
        NativeEngine.GetStageStats(_handle, stage, out var stats).ThrowIfFailed();
        return stats;
    }

    public void Dispose()
    {
        if (_handle != 0)
        {
            NativeEngine.DestroyEngine(_handle);
            _handle = 0;
        }
    }
}

internal sealed record GpuAdapter(
    uint Index,
    string Name,
    NativeEngine.Vendor Vendor,
    ulong VramBytes,
    string DriverVersion,
    NativeEngine.Capabilities Capabilities)
{
    public double VramGiB => VramBytes / (1024.0 * 1024 * 1024);
    public bool Has(NativeEngine.Capabilities cap) => (Capabilities & cap) == cap;

    internal static GpuAdapter From(NativeEngine.AdapterInfo i) =>
        new(i.Index, i.NameText, i.Vendor, i.DedicatedVramBytes, i.DriverText, i.Capabilities);
}

internal sealed class EngineException(string message) : Exception(message);

internal static class ResultExtensions
{
    public static void ThrowIfFailed(this NativeEngine.Result r)
    {
        if (r != NativeEngine.Result.Ok) throw new EngineException(r switch
        {
            NativeEngine.Result.NoCompatibleGpu => "No se encontró una GPU compatible con DirectX 12.",
            NativeEngine.Result.DeviceLost => "Se perdió el dispositivo de la GPU (reinicio o cambio de driver).",
            NativeEngine.Result.Unsupported => "Esta función no está disponible en este equipo.",
            _ => $"Error del motor: {r}",
        });
    }
}
