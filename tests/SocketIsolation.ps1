# Test infrastructure only: prevent a launched app from inheriting our listener.
if (-not ('TestSocketIsolation' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class TestSocketIsolation {
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetHandleInformation(IntPtr handle, out uint flags);
    public static void PreventInheritance(IntPtr handle) {
        uint flags;
        if (!SetHandleInformation(handle, 1, 0) ||
            !GetHandleInformation(handle, out flags))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        if ((flags & 1) != 0) throw new InvalidOperationException("Socket remains inheritable");
    }
}
'@
}
function Protect-TestListener($listener) {
    [TestSocketIsolation]::PreventInheritance($listener.Server.Handle)
}
