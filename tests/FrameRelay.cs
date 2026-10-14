// Test-only, frame-aware localhost relay. It never changes application framing,
// file data or machine/network settings; only explicit test fault modes act.
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;

public sealed class FrameRelay : IDisposable {
    readonly TcpListener listener;
    readonly TcpClient server = new TcpClient();
    TcpClient client;
    readonly Thread acceptThread;
    Thread upstream, downstream;
    readonly object stateLock = new object(), serverWrite = new object(), clientWrite = new object();
    readonly ManualResetEventSlim ackSeen = new ManualResetEventSlim(false);
    readonly ManualResetEventSlim chunkSeen = new ManualResetEventSlim(false);
    readonly ManualResetEventSlim ackGate = new ManualResetEventSlim(false);
    readonly ManualResetEventSlim chunkGate = new ManualResetEventSlim(false);
    volatile bool stopped;
    public volatile bool DropAck, HoldAck, PauseChunks;
    public volatile int ChunkDelayMs, FragmentSize;
    byte[] lastAck;
    TcpClient ackTarget;
    object ackWrite;
    public readonly int Port;

    public FrameRelay(int targetPort) {
        listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        try { server.Connect(IPAddress.Loopback, targetPort); }
        catch { listener.Stop(); server.Close(); throw; }
        acceptThread = new Thread(Accept);
        acceptThread.IsBackground = true;
        acceptThread.Start();
    }
    void Accept() {
        try {
            TcpClient accepted = listener.AcceptTcpClient();
            lock(stateLock) {
                if (stopped) { accepted.Close(); return; }
                client = accepted;
                upstream = new Thread(() => Pump(client, server, serverWrite));
                downstream = new Thread(() => Pump(server, client, clientWrite));
                upstream.IsBackground = downstream.IsBackground = true;
                upstream.Start(); downstream.Start();
            }
        } catch (SocketException) { Cut(); }
          catch (ObjectDisposedException) { Cut(); }
    }
    static byte[] Exact(Stream stream, int length) {
        byte[] bytes = new byte[length];
        int offset = 0;
        while (offset < length) {
            int count = stream.Read(bytes, offset, length - offset);
            if (count == 0) throw new EndOfStreamException();
            offset += count;
        }
        return bytes;
    }
    void Pump(TcpClient source, TcpClient target, object writeLock) {
        try {
            Stream input = source.GetStream();
            while (!stopped) {
                byte[] header = Exact(input, 5);
                int length = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];
                if (length < 0 || length > 1024 * 1024) throw new IOException("Invalid relay frame length");
                byte[] payload = Exact(input, length);
                byte[] frame = new byte[length + 5];
                Array.Copy(header, frame, 5); Array.Copy(payload, 0, frame, 5, length);
                if (header[0] == 3 && PauseChunks) {
                    chunkSeen.Set(); chunkGate.Wait();
                }
                if (header[0] == 5) {
                    lock(stateLock) { lastAck = frame; ackTarget = target; ackWrite = writeLock; }
                    ackSeen.Set();
                    if (DropAck) continue;
                    if (HoldAck) ackGate.Wait();
                }
                if (stopped) return;
                if (header[0] == 3 && ChunkDelayMs > 0) Thread.Sleep(ChunkDelayMs);
                lock(writeLock) {
                    int fragment = FragmentSize > 0 ? FragmentSize : frame.Length;
                    for (int offset = 0; offset < frame.Length; offset += fragment)
                        target.GetStream().Write(frame, offset, Math.Min(fragment, frame.Length - offset));
                }
            }
        } catch (IOException) { Cut(); }
          catch (SocketException) { Cut(); }
          catch (ObjectDisposedException) { Cut(); }
    }
    public bool WaitAck(int milliseconds) { return ackSeen.Wait(milliseconds); }
    public bool WaitChunk(int milliseconds) { return chunkSeen.Wait(milliseconds); }
    public void ReleaseAck() { HoldAck = false; ackGate.Set(); }
    public void ReleaseChunks() { PauseChunks = false; chunkGate.Set(); }
    public void ReplayAck() {
        byte[] frame; TcpClient target; object writeLock;
        lock(stateLock) { frame = lastAck; target = ackTarget; writeLock = ackWrite; }
        if (frame == null) throw new InvalidOperationException("No saved ACK");
        lock(writeLock) { target.GetStream().Write(frame, 0, frame.Length); }
    }
    public void Cut() {
        lock(stateLock) {
            if (stopped) return;
            stopped = true;
            listener.Stop(); server.Close();
            if (client != null) client.Close();
            ackGate.Set(); chunkGate.Set();
        }
    }
    public void Dispose() {
        Cut();
        foreach (Thread thread in new Thread[] { acceptThread, upstream, downstream })
            if (thread != null && !thread.Join(5000)) throw new Exception("Relay worker did not stop");
        ackSeen.Dispose(); chunkSeen.Dispose(); ackGate.Dispose(); chunkGate.Dispose();
    }
}
