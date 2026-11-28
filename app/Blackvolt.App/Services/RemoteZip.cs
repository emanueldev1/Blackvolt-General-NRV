using System.Buffers.Binary;
using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;

namespace Blackvolt.App.Services;

/// <summary>
/// Reads single members of a ZIP on a web server without downloading the whole archive: the
/// central directory and each wanted member are fetched with HTTP Range requests. A release
/// package of a couple of GB costs only the bytes of the files that are extracted.
/// </summary>
internal sealed class RemoteZip
{
    internal sealed record Entry(string Name, ushort Method, uint Crc32, long CompressedSize, long Size, long HeaderOffset);

    private readonly HttpClient _http;
    private readonly string _url;

    public long Length { get; private set; }
    public IReadOnlyList<Entry> Entries { get; private set; } = [];

    private RemoteZip(HttpClient http, string url)
    {
        _http = http;
        _url = url;
    }

    /// <summary>Opens the archive. Throws <see cref="NotSupportedException"/> when the server ignores ranges.</summary>
    public static async Task<RemoteZip> OpenAsync(HttpClient http, string url, CancellationToken ct)
    {
        var zip = new RemoteZip(http, url);
        await zip.ReadDirectoryAsync(ct);
        return zip;
    }

    private async Task<(byte[] Data, long Total)> RangeAsync(long from, long to, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, _url);
        request.Headers.Range = new RangeHeaderValue(from, to);
        using var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);
        if (response.StatusCode != HttpStatusCode.PartialContent)
        {
            response.EnsureSuccessStatusCode();
            throw new NotSupportedException("El servidor no admite descargas parciales.");
        }
        var total = response.Content.Headers.ContentRange?.Length ?? 0;
        return (await response.Content.ReadAsByteArrayAsync(ct), total);
    }

    private async Task ReadDirectoryAsync(CancellationToken ct)
    {
        // Total size first: GitHub's release CDN refuses suffix ranges ("bytes=-N"), so every
        // request names explicit offsets. Then the end of central directory, in the last 64 KiB + 22 bytes.
        var (_, total) = await RangeAsync(0, 0, ct);
        if (total <= 0) throw new NotSupportedException("El servidor no informó el tamaño del archivo.");
        Length = total;
        var tailStart = Math.Max(0, total - 65_557);
        var (tail, _) = await RangeAsync(tailStart, total - 1, ct);
        int eocd = -1;
        for (int i = tail.Length - 22; i >= 0; i--)
        {
            if (BinaryPrimitives.ReadUInt32LittleEndian(tail.AsSpan(i)) == 0x06054b50) { eocd = i; break; }
        }
        if (eocd < 0) throw new InvalidDataException("El paquete no es un ZIP válido.");

        long count = BinaryPrimitives.ReadUInt16LittleEndian(tail.AsSpan(eocd + 10));
        long cdSize = BinaryPrimitives.ReadUInt32LittleEndian(tail.AsSpan(eocd + 12));
        long cdOffset = BinaryPrimitives.ReadUInt32LittleEndian(tail.AsSpan(eocd + 16));
        if (count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF)
        {
            // ZIP64: the locator sits right before the classic record.
            var loc = eocd - 20;
            if (loc < 0 || BinaryPrimitives.ReadUInt32LittleEndian(tail.AsSpan(loc)) != 0x07064b50)
                throw new InvalidDataException("ZIP64 sin localizador.");
            var z64 = (long)BinaryPrimitives.ReadUInt64LittleEndian(tail.AsSpan(loc + 8));
            byte[] rec;
            if (z64 >= tailStart) rec = tail.AsSpan((int)(z64 - tailStart), 56).ToArray();
            else rec = (await RangeAsync(z64, z64 + 55, ct)).Data;
            if (BinaryPrimitives.ReadUInt32LittleEndian(rec) != 0x06064b50) throw new InvalidDataException("ZIP64 dañado.");
            count = (long)BinaryPrimitives.ReadUInt64LittleEndian(rec.AsSpan(32));
            cdSize = (long)BinaryPrimitives.ReadUInt64LittleEndian(rec.AsSpan(40));
            cdOffset = (long)BinaryPrimitives.ReadUInt64LittleEndian(rec.AsSpan(48));
        }

        var cd = cdOffset >= tailStart && cdOffset + cdSize <= total
            ? tail.AsSpan((int)(cdOffset - tailStart), (int)cdSize).ToArray()
            : (await RangeAsync(cdOffset, cdOffset + cdSize - 1, ct)).Data;

        var entries = new List<Entry>((int)Math.Min(count, 100_000));
        int p = 0;
        while (p + 46 <= cd.Length && BinaryPrimitives.ReadUInt32LittleEndian(cd.AsSpan(p)) == 0x02014b50)
        {
            var s = cd.AsSpan(p);
            ushort method = BinaryPrimitives.ReadUInt16LittleEndian(s[10..]);
            uint crc = BinaryPrimitives.ReadUInt32LittleEndian(s[16..]);
            long comp = BinaryPrimitives.ReadUInt32LittleEndian(s[20..]);
            long size = BinaryPrimitives.ReadUInt32LittleEndian(s[24..]);
            int nameLen = BinaryPrimitives.ReadUInt16LittleEndian(s[28..]);
            int extraLen = BinaryPrimitives.ReadUInt16LittleEndian(s[30..]);
            int commentLen = BinaryPrimitives.ReadUInt16LittleEndian(s[32..]);
            long offset = BinaryPrimitives.ReadUInt32LittleEndian(s[42..]);
            ushort flags = BinaryPrimitives.ReadUInt16LittleEndian(s[8..]);
            var nameBytes = s.Slice(46, nameLen);
            var name = ((flags & 0x800) != 0 ? System.Text.Encoding.UTF8 : System.Text.Encoding.Latin1).GetString(nameBytes);

            // ZIP64 extra field: only the values that overflowed are present, in this order.
            var extra = s.Slice(46 + nameLen, extraLen);
            for (int e = 0; e + 4 <= extra.Length;)
            {
                ushort id = BinaryPrimitives.ReadUInt16LittleEndian(extra[e..]);
                ushort len = BinaryPrimitives.ReadUInt16LittleEndian(extra[(e + 2)..]);
                if (id == 0x0001)
                {
                    int q = e + 4;
                    if (size == 0xFFFFFFFF) { size = (long)BinaryPrimitives.ReadUInt64LittleEndian(extra[q..]); q += 8; }
                    if (comp == 0xFFFFFFFF) { comp = (long)BinaryPrimitives.ReadUInt64LittleEndian(extra[q..]); q += 8; }
                    if (offset == 0xFFFFFFFF) { offset = (long)BinaryPrimitives.ReadUInt64LittleEndian(extra[q..]); }
                }
                e += 4 + len;
            }
            entries.Add(new Entry(name.Replace('\\', '/'), method, crc, comp, size, offset));
            p += 46 + nameLen + extraLen + commentLen;
        }
        Entries = entries;
    }

    /// <summary>
    /// Extracts one member to <paramref name="target"/>, checking its CRC-32. Returns the SHA-256
    /// (lower-case hex). <paramref name="progress"/> receives 0..1 of the compressed bytes.
    /// </summary>
    public async Task<string> ExtractAsync(Entry entry, string target, IProgress<double>? progress, CancellationToken ct)
    {
        if (entry.Method is not (0 or 8)) throw new InvalidDataException($"Compresión {entry.Method} no soportada.");
        var (local, _) = await RangeAsync(entry.HeaderOffset, entry.HeaderOffset + 29, ct);
        if (BinaryPrimitives.ReadUInt32LittleEndian(local) != 0x04034b50) throw new InvalidDataException("Cabecera local dañada.");
        long dataStart = entry.HeaderOffset + 30 + BinaryPrimitives.ReadUInt16LittleEndian(local.AsSpan(26)) +
                         BinaryPrimitives.ReadUInt16LittleEndian(local.AsSpan(28));

        using var request = new HttpRequestMessage(HttpMethod.Get, _url);
        request.Headers.Range = new RangeHeaderValue(dataStart, dataStart + Math.Max(0, entry.CompressedSize - 1));
        using var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);
        if (response.StatusCode != HttpStatusCode.PartialContent) throw new NotSupportedException("El servidor no admite descargas parciales.");
        await using var raw = await response.Content.ReadAsStreamAsync(ct);
        await using var counted = new ProgressStream(raw, entry.CompressedSize, progress);
        await using Stream data = entry.Method == 8 ? new DeflateStream(counted, CompressionMode.Decompress) : counted;
        var sha = await WriteVerifiedAsync(data, target, entry.Size, entry.Crc32, ct);
        progress?.Report(1.0);
        return sha;
    }

    /// <summary>Copies to a file while hashing (SHA-256) and checking size and CRC-32.</summary>
    internal static async Task<string> WriteVerifiedAsync(Stream data, string target, long size, uint crc, CancellationToken ct)
    {
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        uint crc32 = 0xFFFFFFFF;
        long written = 0;
        await using (var output = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, useAsync: true))
        {
            var buffer = new byte[1 << 20];
            int read;
            while ((read = await data.ReadAsync(buffer, ct)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, read), ct);
                sha.AppendData(buffer, 0, read);
                crc32 = Crc32.Update(crc32, buffer.AsSpan(0, read));
                written += read;
            }
        }
        if (written != size || ~crc32 != crc)
        {
            File.Delete(target);
            throw new InvalidDataException("El archivo descargado está incompleto o dañado.");
        }
        return Convert.ToHexString(sha.GetHashAndReset()).ToLowerInvariant();
    }

    private sealed class ProgressStream(Stream inner, long total, IProgress<double>? progress) : Stream
    {
        private long _done;
        private long _lastReport;
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => total;
        public override long Position { get => _done; set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count) => Count(inner.Read(buffer, offset, count));
        public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct = default) =>
            Count(await inner.ReadAsync(buffer, ct));
        public override async Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken ct) =>
            Count(await inner.ReadAsync(buffer.AsMemory(offset, count), ct));
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

        private int Count(int n)
        {
            _done += n;
            if (progress is not null && total > 0 && (_done - _lastReport > (1 << 20) || n == 0))
            {
                _lastReport = _done;
                progress.Report(Math.Min(1.0, (double)_done / total));
            }
            return n;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) inner.Dispose();
            base.Dispose(disposing);
        }
    }
}

/// <summary>CRC-32 (IEEE 802.3), as ZIP stores it.</summary>
internal static class Crc32
{
    private static readonly uint[] Table = Build();

    private static uint[] Build()
    {
        var t = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }

    public static uint Update(uint crc, ReadOnlySpan<byte> data)
    {
        foreach (var b in data) crc = Table[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc;
    }
}
