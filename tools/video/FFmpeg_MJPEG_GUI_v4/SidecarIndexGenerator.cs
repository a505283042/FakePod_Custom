using System;
using System.Collections.Generic;
using System.Threading;
using System.IO;

namespace FakePod.VideoTools
{
    public sealed class SidecarIndexResult
    {
        public bool Ok { get; set; }
        public string Path { get; set; }
        public string SourcePath { get; set; }
        public int Count { get; set; }
        public string Message { get; set; }
    }

    // 使用独立后台线程扫描 AVI；完成状态由本类直接发布，不依赖界面同步上下文回调。
    public sealed class SidecarIndexWorker
    {
        private readonly object _stateLock = new object();
        private int _progressPercentage;
        private string _progressText = "";
        private SidecarIndexResult[] _lastResults = new SidecarIndexResult[0];
        private string _lastError = "";
        private bool _isRunning;
        private bool _isCompleted;

        public int ProgressPercentage
        {
            get { lock (_stateLock) return _progressPercentage; }
        }

        public string ProgressText
        {
            get { lock (_stateLock) return _progressText; }
        }

        public SidecarIndexResult[] LastResults
        {
            get
            {
                lock (_stateLock)
                    return (SidecarIndexResult[])_lastResults.Clone();
            }
        }

        public string LastError
        {
            get { lock (_stateLock) return _lastError; }
        }

        public bool IsRunning
        {
            get { lock (_stateLock) return _isRunning; }
        }

        public bool IsCompleted
        {
            get { lock (_stateLock) return _isCompleted; }
        }

        public void SetProgress(int percentage, string text)
        {
            lock (_stateLock)
            {
                _progressPercentage = Math.Max(0, Math.Min(100, percentage));
                _progressText = text ?? "";
            }
        }

        public void StartAsync(string[] aviPaths)
        {
            if (aviPaths == null || aviPaths.Length == 0)
                throw new ArgumentException("没有选择需要生成索引的 AVI 文件。", "aviPaths");

            Thread thread = new Thread(new ThreadStart(delegate()
            {
                try
                {
                    SidecarIndexResult[] results = SidecarIndexGenerator.GenerateBatch(aviPaths, this);
                    lock (_stateLock)
                    {
                        _lastResults = results ?? new SidecarIndexResult[0];
                        _progressPercentage = 100;
                        if (_lastResults.Length == 0 && String.IsNullOrEmpty(_lastError))
                            _lastError = "索引扫描结束，但没有返回任何文件结果。";
                    }
                }
                catch (Exception ex)
                {
                    lock (_stateLock)
                    {
                        _lastError = ex.Message;
                    }
                }
                finally
                {
                    // 先写入结果/错误，再发布完成标志；UI Timer 读取时通过同一把锁获得完整状态。
                    lock (_stateLock)
                    {
                        _isRunning = false;
                        _isCompleted = true;
                    }
                }
            }));

            thread.IsBackground = true;
            thread.Name = "FakePod FVIX Index Generator";

            // 线程对象创建成功后再标记 Running，避免线程创建失败时界面永远等待。
            lock (_stateLock)
            {
                if (_isRunning)
                    throw new InvalidOperationException("索引生成任务正在运行，请稍候。");
                _progressPercentage = 0;
                _progressText = "准备启动索引扫描…";
                _lastResults = new SidecarIndexResult[0];
                _lastError = "";
                _isRunning = true;
                _isCompleted = false;
            }

            try
            {
                thread.Start();
            }
            catch (Exception ex)
            {
                lock (_stateLock)
                {
                    _lastError = ex.Message;
                    _isRunning = false;
                    _isCompleted = true;
                }
                throw;
            }
        }
    }

    public static class SidecarIndexGenerator
    {
        private const int GranularityMs = 2000;
        private const int MaxEntries = 100000;
        private const int BufferBytes = 1024 * 1024;
        private const long MaxFileLength = 4294967295L;

        private static readonly uint IdRiff = FourCC("RIFF");
        private static readonly uint IdAvi = FourCC("AVI ");
        private static readonly uint IdAvix = FourCC("AVIX");
        private static readonly uint IdList = FourCC("LIST");
        private static readonly uint IdHdrl = FourCC("hdrl");
        private static readonly uint IdAvih = FourCC("avih");
        private static readonly uint IdMovi = FourCC("movi");
        private static readonly uint IdRec = FourCC("rec ");
        private static readonly uint IdVideoDc = FourCC("00dc");
        private static readonly uint IdVideoDb = FourCC("00db");
        private static readonly uint IdAudio1Wb = FourCC("01wb");
        private static readonly uint IdAudio0Wb = FourCC("00wb");

        private sealed class MoviRange
        {
            public long Start;
            public long End;
        }

        private sealed class RecFrame
        {
            public long End;
            public long Advance;
        }

        private struct IndexEntry
        {
            public uint TimestampMs;
            public uint ChunkOffset;
            public uint FrameIndex;
            public uint AudioBytes;
        }

        // 大块顺序缓存：连续扫描时只在跨出缓存窗口后才访问 FileStream。
        // AVI Chunk 的 Header 通常只有 8 字节，避免为每个 Header 单独执行文件定位和小数组分配。
        private sealed class BufferedReader
        {
            private readonly FileStream stream;
            private readonly byte[] buffer;
            private long bufferStart;
            private int bufferLength;

            public BufferedReader(FileStream input)
            {
                stream = input;
                buffer = new byte[BufferBytes];
                bufferStart = -1;
                bufferLength = 0;
            }

            public void ReadAt(long offset, byte[] destination, int destinationOffset, int count)
            {
                if (offset < 0 || destination == null || destinationOffset < 0 ||
                    count < 0 || destinationOffset + count > destination.Length)
                    throw new ArgumentOutOfRangeException("读取范围无效");

                while (count > 0)
                {
                    if (bufferStart < 0 || offset < bufferStart ||
                        offset >= bufferStart + bufferLength)
                    {
                        stream.Position = offset;
                        bufferStart = offset;
                        bufferLength = stream.Read(buffer, 0, buffer.Length);
                        if (bufferLength <= 0)
                            throw new EndOfStreamException("读取 AVI 文件时意外到达文件末尾");
                    }

                    int inBuffer = (int)(offset - bufferStart);
                    int available = bufferLength - inBuffer;
                    int copy = Math.Min(count, available);
                    Buffer.BlockCopy(buffer, inBuffer, destination, destinationOffset, copy);

                    offset += copy;
                    destinationOffset += copy;
                    count -= copy;
                }
            }
        }

        private static uint FourCC(string value)
        {
            return (uint)(byte)value[0] |
                   ((uint)(byte)value[1] << 8) |
                   ((uint)(byte)value[2] << 16) |
                   ((uint)(byte)value[3] << 24);
        }

        private static uint ReadU32(byte[] data, int offset)
        {
            return (uint)data[offset] |
                   ((uint)data[offset + 1] << 8) |
                   ((uint)data[offset + 2] << 16) |
                   ((uint)data[offset + 3] << 24);
        }

        private static void ReadHeader(BufferedReader reader, long offset, byte[] header)
        {
            reader.ReadAt(offset, header, 0, 8);
        }

        private static string FourCCText(uint value)
        {
            char[] chars = new char[4];
            chars[0] = (char)(value & 0xff);
            chars[1] = (char)((value >> 8) & 0xff);
            chars[2] = (char)((value >> 16) & 0xff);
            chars[3] = (char)((value >> 24) & 0xff);
            return new string(chars);
        }

        private static void Report(SidecarIndexWorker worker, int fileIndex, int fileCount,
            int localPercent, string fileName, string message)
        {
            if (worker == null)
                return;

            int total = Math.Max(1, fileCount);
            int local = Math.Max(0, Math.Min(100, localPercent));
            int overall = (fileIndex * 100 + local) / total;
            string text = String.Format("{0} ({1}/{2})：{3}",
                fileName, fileIndex + 1, total, message);
            worker.SetProgress(Math.Max(0, Math.Min(100, overall)), text);
        }

        public static SidecarIndexResult[] GenerateBatch(string[] aviPaths, SidecarIndexWorker worker)
        {
            if (aviPaths == null)
                return new SidecarIndexResult[0];

            SidecarIndexResult[] results = new SidecarIndexResult[aviPaths.Length];
            for (int i = 0; i < aviPaths.Length; i++)
            {
                // 严格逐文件处理：当前 AVI 完整扫描、写入临时 FVIX 并提交正式 .fvix 后，
                // Generate 才会返回；此时才打开下一个 AVI。单文件异常由 Generate 捕获为失败结果，
                // 不会中断后续文件，也不会撤销此前已经成功生成的索引。
                results[i] = Generate(aviPaths[i], worker, i, aviPaths.Length);
            }
            return results;
        }

        public static SidecarIndexResult Generate(string aviPath, SidecarIndexWorker worker,
            int fileIndex, int fileCount)
        {
            string fullAviPath = "";
            string outputPath = "";
            string tempPath = "";
            string fileName = "AVI";
            string stage = "准备源文件路径";
            SidecarIndexResult result = new SidecarIndexResult();
            int generatedCount = 0;
            result.Ok = false;
            result.Path = "";
            result.SourcePath = aviPath ?? "";
            result.Count = 0;
            result.Message = "";

            try
            {
                if (String.IsNullOrWhiteSpace(aviPath))
                    throw new ArgumentException("没有提供 AVI 源文件路径。");

                // 统一为绝对路径，避免批量选择后相对路径或当前目录变化影响后台线程。
                fullAviPath = Path.GetFullPath(aviPath.Trim());
                result.SourcePath = fullAviPath;
                fileName = Path.GetFileName(fullAviPath);

                // 只允许以 AVI 为输入；避免误选 .fvix 时把输入索引当作输出文件删除。
                if (!String.Equals(Path.GetExtension(fullAviPath), ".avi", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("选中的文件不是 AVI（仅接受 .avi）：" + fullAviPath);

                outputPath = Path.ChangeExtension(fullAviPath, ".fvix");
                tempPath = outputPath + ".tmp";
                result.Path = outputPath;

                Report(worker, fileIndex, fileCount, 0, fileName, "正在打开 AVI…");

                // 直接打开源文件，不用 File.Exists 预判；后者会把设备离线、权限不足等情况误报为文件不存在。
                // 先成功打开 AVI，再处理临时索引，避免路径异常时误删已有索引。
                stage = "打开 AVI 源文件";
                using (FileStream stream = new FileStream(fullAviPath, FileMode.Open, FileAccess.Read,
                    FileShare.Read, 64 * 1024, FileOptions.SequentialScan))
                {
                    stage = "检查索引临时文件";
                    if (File.Exists(tempPath))
                        File.Delete(tempPath);

                    stage = "扫描 AVI 结构并生成索引";
                    long fileLength = stream.Length;
                    if (fileLength < 12 || fileLength > MaxFileLength)
                        throw new InvalidDataException("AVI 文件长度超出 FVIX V2 的 32 位偏移范围");

                    BufferedReader reader = new BufferedReader(stream);
                    byte[] header12 = new byte[12];
                    byte[] header8 = new byte[8];
                    byte[] fourBytes = new byte[4];
                    byte[] signature = new byte[2];

                    reader.ReadAt(0, header12, 0, 12);
                    if (ReadU32(header12, 0) != IdRiff || ReadU32(header12, 8) != IdAvi)
                        throw new InvalidDataException("不是有效的 RIFF AVI 文件");

                    // 第一遍：读取 RIFF AVI / RIFF AVIX 目录，只收集全部 movi 范围。
                    uint microsecondsPerFrame = 0;
                    List<MoviRange> moviRanges = new List<MoviRange>();
                    long position = 0;
                    int riffIndex = 0;

                    while (position + 12 <= fileLength)
                    {
                        reader.ReadAt(position, header12, 0, 12);
                        uint riffId = ReadU32(header12, 0);
                        if (riffId != IdRiff)
                        {
                            if (riffIndex == 0)
                                throw new InvalidDataException("AVI 文件起点不是 RIFF");
                            break;
                        }

                        long riffSize = ReadU32(header12, 4);
                        uint riffType = ReadU32(header12, 8);
                        if (riffSize < 4 || riffSize > fileLength - position - 8)
                            throw new InvalidDataException(String.Format(
                                "AVI RIFF 段长度非法：segment={0} offset={1} size={2}",
                                riffIndex, position, riffSize));

                        if ((riffIndex == 0 && riffType != IdAvi) ||
                            (riffIndex > 0 && riffType != IdAvix && riffType != IdAvi))
                            throw new InvalidDataException("不支持的 AVI RIFF 段类型：" + FourCCText(riffType));

                        long riffEnd = position + 8 + riffSize;
                        long chunkPosition = position + 12;

                        while (chunkPosition + 8 <= riffEnd)
                        {
                            ReadHeader(reader, chunkPosition, header8);
                            uint chunkId = ReadU32(header8, 0);
                            long chunkSize = ReadU32(header8, 4);
                            long dataPosition = chunkPosition + 8;

                            if (chunkSize > riffEnd - dataPosition)
                                throw new InvalidDataException("AVI RIFF 子 chunk 越界：" + chunkPosition);

                            long advance = dataPosition + chunkSize + (chunkSize & 1L);
                            if (advance > riffEnd)
                                throw new InvalidDataException("AVI RIFF 子 chunk 补齐字节越界：" + chunkPosition);

                            if (chunkId == IdList && chunkSize >= 4)
                            {
                                reader.ReadAt(dataPosition, fourBytes, 0, 4);
                                uint listType = ReadU32(fourBytes, 0);

                                if (listType == IdHdrl)
                                {
                                    long p = dataPosition + 4;
                                    long headerEnd = dataPosition + chunkSize;

                                    while (p + 8 <= headerEnd)
                                    {
                                        ReadHeader(reader, p, header8);
                                        uint subId = ReadU32(header8, 0);
                                        long subSize = ReadU32(header8, 4);
                                        long subData = p + 8;

                                        if (subSize > headerEnd - subData)
                                            throw new InvalidDataException("AVI hdrl 子 chunk 越界：" + p);

                                        long subAdvance = subData + subSize + (subSize & 1L);
                                        if (subAdvance > headerEnd)
                                            throw new InvalidDataException("AVI hdrl 补齐字节越界：" + p);

                                        if (subId == IdAvih && subSize >= 56 && microsecondsPerFrame == 0)
                                        {
                                            reader.ReadAt(subData, fourBytes, 0, 4);
                                            microsecondsPerFrame = ReadU32(fourBytes, 0);
                                        }
                                        p = subAdvance;
                                    }
                                }
                                else if (listType == IdMovi)
                                {
                                    long moviStart = dataPosition + 4;
                                    long moviEnd = dataPosition + chunkSize;
                                    if (moviEnd <= moviStart)
                                        throw new InvalidDataException("AVI movi 区间无效");
                                    MoviRange range = new MoviRange();
                                    range.Start = moviStart;
                                    range.End = moviEnd;
                                    moviRanges.Add(range);
                                }
                            }

                            chunkPosition = advance;
                        }

                        long nextRiff = riffEnd + (riffSize & 1L);
                        if (nextRiff <= position)
                            throw new InvalidDataException("AVI RIFF 段偏移没有前进");
                        if (nextRiff >= fileLength)
                            break;

                        position = nextRiff;
                        riffIndex++;
                    }

                    if (microsecondsPerFrame == 0)
                        throw new InvalidDataException("AVI 主头 avih 未提供有效帧间隔");

                    uint fpsX100 = (uint)Math.Floor(100000000m / (decimal)microsecondsPerFrame);
                    if (fpsX100 == 0)
                        throw new InvalidDataException("AVI 帧率超出设备端支持范围");
                    if (moviRanges.Count == 0)
                        throw new InvalidDataException("未找到有效 movi 数据区");

                    long totalMoviBytes = 0;
                    for (int i = 0; i < moviRanges.Count; i++)
                        totalMoviBytes += moviRanges[i].End - moviRanges[i].Start;

                    // 第二遍：顺序扫描所有 movi；1MB 缓存避免每个音视频 Chunk 都执行文件定位。
                    List<IndexEntry> entries = new List<IndexEntry>();
                    ulong frameIndex = 0;
                    ulong audioBytesTotal = 0;
                    ulong nextMarkMs = 0;
                    long processedBytes = 0;
                    long lastProgressBytes = 0;
                    ulong chunkCount = 0;
                    ulong audioChunkCount = 0;
                    ulong videoFrameCount = 0;
                    long startedTicks = DateTime.UtcNow.Ticks;

                    for (int rangeIndex = 0; rangeIndex < moviRanges.Count; rangeIndex++)
                    {
                        long pos = moviRanges[rangeIndex].Start;
                        long moviEnd = moviRanges[rangeIndex].End;
                        Stack<RecFrame> recStack = new Stack<RecFrame>();

                        while (true)
                        {
                            while (recStack.Count > 0 && pos >= recStack.Peek().End)
                            {
                                RecFrame finished = recStack.Pop();
                                long oldPosition = pos;
                                pos = finished.Advance;
                                processedBytes += pos - oldPosition;
                            }

                            long currentEnd = recStack.Count > 0 ? recStack.Peek().End : moviEnd;
                            if (pos + 8 > currentEnd)
                                break;

                            ReadHeader(reader, pos, header8);
                            uint chunkId = ReadU32(header8, 0);
                            long chunkSize = ReadU32(header8, 4);
                            long dataPosition = pos + 8;

                            if (chunkSize > currentEnd - dataPosition)
                                throw new InvalidDataException("AVI movi chunk 越界：" + pos);

                            long advance = dataPosition + chunkSize + (chunkSize & 1L);
                            if (advance > currentEnd)
                                throw new InvalidDataException("AVI movi chunk 补齐字节越界：" + pos);

                            long oldPos = pos;
                            chunkCount++;

                            if (chunkId == IdList && chunkSize >= 4)
                            {
                                reader.ReadAt(dataPosition, fourBytes, 0, 4);
                                if (ReadU32(fourBytes, 0) == IdRec)
                                {
                                    RecFrame rec = new RecFrame();
                                    rec.End = dataPosition + chunkSize;
                                    rec.Advance = advance;
                                    recStack.Push(rec);
                                    pos = dataPosition + 4;
                                    processedBytes += pos - oldPos;
                                    continue;
                                }
                            }

                            if (chunkId == IdVideoDc || chunkId == IdVideoDb)
                            {
                                if (chunkSize >= 2)
                                {
                                    ulong timestampMs = (ulong)Math.Floor(
                                        ((decimal)frameIndex * 100000m) / (decimal)fpsX100);

                                    if (timestampMs > UInt32.MaxValue)
                                        throw new InvalidDataException("AVI 时长超出 FVIX 时间戳范围");

                                    if (timestampMs >= nextMarkMs)
                                    {
                                        reader.ReadAt(dataPosition, signature, 0, 2);
                                        if (signature[0] == 0xff && signature[1] == 0xd8)
                                        {
                                            if (frameIndex > UInt32.MaxValue)
                                                throw new InvalidDataException("视频帧序号超出 FVIX 范围");
                                            if (audioBytesTotal > UInt32.MaxValue)
                                                throw new InvalidDataException("音频累计字节数超出 FVIX 范围");

                                            IndexEntry entry = new IndexEntry();
                                            entry.TimestampMs = (uint)timestampMs;
                                            entry.ChunkOffset = (uint)pos;
                                            entry.FrameIndex = (uint)frameIndex;
                                            entry.AudioBytes = (uint)audioBytesTotal;
                                            entries.Add(entry);
                                            nextMarkMs = timestampMs + (ulong)GranularityMs;
                                        }
                                    }
                                }

                                // 与设备端一致：即使是 0 字节占位视频 chunk，也占用一个帧序号。
                                frameIndex++;
                                videoFrameCount++;
                            }
                            else if (chunkId == IdAudio1Wb || chunkId == IdAudio0Wb)
                            {
                                audioBytesTotal += (ulong)chunkSize;
                                if (audioBytesTotal > UInt32.MaxValue)
                                    throw new InvalidDataException("音频累计字节数超出 FVIX 范围");
                                audioChunkCount++;
                            }

                            pos = advance;
                            processedBytes += pos - oldPos;

                            if (processedBytes - lastProgressBytes >= 8L * 1024L * 1024L ||
                                (chunkCount % 32768UL) == 0UL)
                            {
                                lastProgressBytes = processedBytes;
                                int scanPercent = totalMoviBytes > 0
                                    ? (int)Math.Min(90L, 5L + (processedBytes * 90L / totalMoviBytes))
                                    : 90;
                                double scannedMb = processedBytes / (1024.0 * 1024.0);
                                double totalMb = totalMoviBytes / (1024.0 * 1024.0);
                                double elapsed = (DateTime.UtcNow.Ticks - startedTicks) /
                                    (double)TimeSpan.TicksPerSecond;
                                Report(worker, fileIndex, fileCount, scanPercent, fileName,
                                    String.Format("扫描 {0:0.0}/{1:0.0} MB；Chunk {2:N0}；音频块 {3:N0}；视频帧 {4:N0}；索引点 {5:N0}；{6:0.0}s",
                                        scannedMb, totalMb, chunkCount, audioChunkCount, videoFrameCount, entries.Count, elapsed));
                            }
                        }

                        if (recStack.Count > 0)
                            throw new InvalidDataException("AVI LIST rec 组长度不完整");
                    }

                    if (entries.Count == 0)
                        throw new InvalidDataException("未找到有效视频帧，无法生成索引");
                    if (entries.Count > MaxEntries)
                        throw new InvalidDataException("索引条目超过设备端上限 100000，无法安全加载到 PSRAM");
                    generatedCount = entries.Count;

                    stage = "计算 AVI 来源指纹";
                    Report(worker, fileIndex, fileCount, 94, fileName, "正在计算 AVI 来源指纹…");
                    uint sourceCrc32 = ComputeSampleCrc32(reader, fileLength);

                    stage = "写入 FVIX 临时索引";
                    Report(worker, fileIndex, fileCount, 97, fileName, "正在写入 FVIX V2 索引…");
                    using (FileStream output = new FileStream(tempPath, FileMode.Create, FileAccess.Write,
                        FileShare.None, 64 * 1024, FileOptions.SequentialScan))
                    using (BinaryWriter writer = new BinaryWriter(output))
                    {
                        writer.Write(new byte[] { (byte)'F', (byte)'V', (byte)'I', (byte)'X' });
                        writer.Write((ushort)2);
                        writer.Write((ushort)GranularityMs);
                        writer.Write((uint)entries.Count);
                        writer.Write((uint)fileLength);
                        writer.Write(sourceCrc32);

                        for (int i = 0; i < entries.Count; i++)
                        {
                            writer.Write(entries[i].TimestampMs);
                            writer.Write(entries[i].ChunkOffset);
                            writer.Write(entries[i].FrameIndex);
                            writer.Write(entries[i].AudioBytes);
                        }
                        writer.Flush();
                    }
                }

                // 新索引完整写入临时文件后才替换旧索引；扫描失败不会破坏原有可用索引。
                stage = "提交 FVIX 索引文件";
                if (File.Exists(outputPath))
                {
                    // 不依赖 File.Replace，兼容 FAT32 等外置盘文件系统。
                    string backupPath = outputPath + ".bak";
                    if (File.Exists(backupPath))
                        File.Delete(backupPath);
                    File.Move(outputPath, backupPath);
                    try
                    {
                        File.Move(tempPath, outputPath);
                        if (File.Exists(backupPath))
                        {
                            try { File.Delete(backupPath); }
                            catch { /* 新索引已提交；备份清理失败不应误报索引生成失败。 */ }
                        }
                    }
                    catch
                    {
                        if (!File.Exists(outputPath) && File.Exists(backupPath))
                            File.Move(backupPath, outputPath);
                        throw;
                    }
                }
                else
                {
                    File.Move(tempPath, outputPath);
                }

                result.Ok = true;
                result.Count = generatedCount;
                result.Message = String.Format(
                    "V2 索引已生成：{0:N0} 条，文件长度 {1:N0} 字节；支持全部 OpenDML movi 段",
                    result.Count, new FileInfo(fullAviPath).Length);
                Report(worker, fileIndex, fileCount, 100, fileName, "索引生成完成：" + result.Count + " 条");
                return result;
            }
            catch (Exception ex)
            {
                try
                {
                    if (File.Exists(tempPath))
                        File.Delete(tempPath);
                }
                catch { }

                result.Ok = false;
                string sourceDescription = String.IsNullOrWhiteSpace(fullAviPath) ? (aviPath ?? "（空路径）") : fullAviPath;
                result.Message = String.Format("阶段：{0}\r\n源 AVI：{1}\r\n错误：{2}（HRESULT=0x{3:X8}）",
                    stage, sourceDescription, ex.Message, ex.HResult);
                Report(worker, fileIndex, fileCount, 100, fileName, "索引生成失败：" + ex.Message);
                return result;
            }
        }

        private static uint ComputeSampleCrc32(BufferedReader reader, long fileLength)
        {
            long[] offsets = new long[] {
                0,
                fileLength / 3,
                (fileLength * 2) / 3,
                Math.Max(0, fileLength - 512)
            };

            uint crc = 0xffffffffU;
            byte[] sample = new byte[512];

            for (int i = 0; i < offsets.Length; i++)
            {
                int sampleLength = (int)Math.Min(512L, fileLength - offsets[i]);
                if (sampleLength <= 0)
                    throw new InvalidDataException("AVI 指纹抽样范围无效：" + offsets[i]);

                reader.ReadAt(offsets[i], sample, 0, sampleLength);
                for (int j = 0; j < sampleLength; j++)
                {
                    crc ^= sample[j];
                    for (int bit = 0; bit < 8; bit++)
                    {
                        if ((crc & 1U) != 0)
                            crc = (crc >> 1) ^ 0xedb88320U;
                        else
                            crc >>= 1;
                    }
                }
            }

            return crc ^ 0xffffffffU;
        }
    }
}
