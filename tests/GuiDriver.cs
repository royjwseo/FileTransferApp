using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public sealed class GuiDriver : IDisposable {
    public readonly Process Process;
    public IntPtr Window;
    delegate bool EnumProc(IntPtr hwnd, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback,IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent,EnumProc callback,IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint id);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd,StringBuilder text,int max);
    [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr hwnd,int id);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd,uint msg,IntPtr w,IntPtr l);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint msg,IntPtr w,StringBuilder l,uint flags,uint timeout,out IntPtr result);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint msg,IntPtr w,string l,uint flags,uint timeout,out IntPtr result);
    static string Class(IntPtr h){var b=new StringBuilder(128);GetClassName(h,b,b.Capacity);return b.ToString();}
    public GuiDriver(string exe,string cwd){
        Process=new Process {StartInfo=new ProcessStartInfo(exe){WorkingDirectory=cwd,UseShellExecute=false,WindowStyle=ProcessWindowStyle.Hidden}};
        Process.Start();
        var watch=Stopwatch.StartNew();
        while(watch.ElapsedMilliseconds<5000){
            EnumWindows((h,d)=>{uint id;GetWindowThreadProcessId(h,out id);if(id==Process.Id&&Class(h)=="FileTransferAppGui")Window=h;return true;},IntPtr.Zero);
            if(Window!=IntPtr.Zero)return;
            if(Process.HasExited)throw new Exception("GUI exited before window creation");Thread.Sleep(20);
        }
        Dispose();throw new Exception("GUI window creation timed out");
    }
    public void Click(int id){if(!PostMessage(Window,0x111,new IntPtr(id),IntPtr.Zero))throw new Exception("Post failed");}
    public void Set(int id,string text){SetHandle(GetDlgItem(Window,id),text);}
    static void SetHandle(IntPtr hwnd,string text){IntPtr r;if(hwnd==IntPtr.Zero||SendMessageTimeout(hwnd,0xc,IntPtr.Zero,text,2,1000,out r)==IntPtr.Zero)throw new Exception("Set text timeout");}
    public string Text(int id){
        IntPtr r;var b=new StringBuilder(65536);
        if(SendMessageTimeout(GetDlgItem(Window,id),0xd,new IntPtr(b.Capacity),b,2,1000,out r)==IntPtr.Zero)throw new Exception("Get text timeout");return b.ToString();
    }
    public void Wait(int id,string text,int timeout=10000){var w=Stopwatch.StartNew();while(w.ElapsedMilliseconds<timeout){if(Text(id).Contains(text))return;Thread.Sleep(20);}throw new Exception("GUI missing "+text+"; status="+Text(107)+" log="+Text(108));}
    public void Responsive(){IntPtr r;if(SendMessageTimeout(Window,0,IntPtr.Zero,"",2,500,out r)==IntPtr.Zero)throw new Exception("UI blocked");}
    public void Enter(){PostMessage(GetDlgItem(Window,109),0x100,new IntPtr(13),IntPtr.Zero);}
    public void Pick(string path){
        Click(111);IntPtr dialog=IntPtr.Zero;var watch=Stopwatch.StartNew();
        while(watch.ElapsedMilliseconds<5000){EnumWindows((h,d)=>{uint id;GetWindowThreadProcessId(h,out id);if(id==Process.Id&&Class(h)=="#32770")dialog=h;return true;},IntPtr.Zero);if(dialog!=IntPtr.Zero)break;Thread.Sleep(20);}
        if(dialog==IntPtr.Zero)throw new Exception("File dialog missing");
        Thread.Sleep(300); // Let the common dialog replace its transient edt1.
        IntPtr edit=IntPtr.Zero;var description=new StringBuilder();
        EnumChildWindows(dialog,(h,d)=>{description.Append(Class(h)+":"+GetDlgCtrlID(h)+" parent="+GetDlgCtrlID(GetParent(h))+"; ");if(Class(h)=="Edit" && (GetDlgCtrlID(h)==1148 || GetDlgCtrlID(GetParent(h))==1148 || GetDlgCtrlID(GetParent(h))==1152))edit=h;return true;},IntPtr.Zero);
        if(edit==IntPtr.Zero)throw new Exception("File name edit missing: "+description);
        SetHandle(edit,"\""+path+"\"");PostMessage(dialog,0x111,new IntPtr(1),IntPtr.Zero);
        // The legacy dialog may first navigate to the absolute path's folder.
        // Confirm the now-populated basename through the same native button.
        Thread.Sleep(300);PostMessage(dialog,0x111,new IntPtr(1),IntPtr.Zero);
        try { Wait(112,System.IO.Path.GetFileName(path)); }
        catch(Exception e){IntPtr r;var b=new StringBuilder(2048);SendMessageTimeout(edit,0xd,new IntPtr(b.Capacity),b,2,500,out r);throw new Exception(e.Message+" selected="+Text(112)+" edit="+b+" controls="+description);}
    }
    public void Close(){PostMessage(Window,0x10,IntPtr.Zero,IntPtr.Zero);if(!Process.WaitForExit(7000))throw new Exception("GUI shutdown timed out");if(Process.ExitCode!=0)throw new Exception("GUI exit failure");}
    public void Dispose(){if(!Process.HasExited){Process.Kill();Process.WaitForExit();}Process.Dispose();}
}
