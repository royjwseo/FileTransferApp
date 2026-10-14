using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;
public static class Stage4Data {
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr input,
        uint inSize, IntPtr output, uint outSize, out uint returned, IntPtr overlapped);
    public static void Sparse(string path, long size) {
        using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write)) {
            uint returned;
            if (!DeviceIoControl(file.SafeFileHandle, 0x900C4, IntPtr.Zero, 0,
                IntPtr.Zero, 0, out returned, IntPtr.Zero))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            file.SetLength(size);
            long[] offsets = { 0, size / 2, (1L << 32) - 65536, 1L << 32, size - 65536 };
            int seed = 40;
            foreach (long offset in offsets) {
                if (offset < 0 || offset >= size) continue;
                byte[] data = new byte[(int)Math.Min(65536, size - offset)];
                new Random(seed++).NextBytes(data);
                file.Position = offset; file.Write(data, 0, data.Length);
            }
            file.Flush();
        }
    }
}
public sealed class Stage4Monitor : IDisposable {
    readonly Process process;
    readonly Thread worker;
    volatile bool stop;
    public long PeakPrivateBytes, PeakWorkingSet;
    public int PeakHandles, Samples;
    public Stage4Monitor(Process process) {
        this.process = process; Sample();
        worker = new Thread(() => { while (!stop) { Sample(); Thread.Sleep(20); } });
        worker.IsBackground = true; worker.Start();
    }
    void Sample() {
        try {
            process.Refresh();
            PeakPrivateBytes = Math.Max(PeakPrivateBytes, process.PrivateMemorySize64);
            PeakWorkingSet = Math.Max(PeakWorkingSet, process.WorkingSet64);
            PeakHandles = Math.Max(PeakHandles, process.HandleCount); Samples++;
        } catch (InvalidOperationException) { }
    }
    public void Dispose() { stop = true; worker.Join(); Sample(); }
}
