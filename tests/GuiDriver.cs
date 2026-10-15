using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public sealed class GuiDriver : IDisposable {
    public readonly Process Process;
    public IntPtr Window;
    public IntPtr Chat;
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
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint msg,IntPtr w,IntPtr l,uint flags,uint timeout,out IntPtr result);
    static string Class(IntPtr h){var b=new StringBuilder(128);GetClassName(h,b,b.Capacity);return b.ToString();}
    public GuiDriver(string exe,string cwd):this(exe,cwd,true) {}
    public GuiDriver(string exe,string cwd,bool seedSettings):this(exe,cwd,seedSettings,true) {}
    public GuiDriver(string exe,string cwd,bool seedSettings,bool legacy):this(exe,cwd,seedSettings,legacy,false){}
    public GuiDriver(string exe,string cwd,bool seedSettings,bool legacy,bool multiclient){
        Process=new Process {StartInfo=new ProcessStartInfo(exe){WorkingDirectory=cwd,UseShellExecute=false,WindowStyle=ProcessWindowStyle.Hidden}};
        string settings=System.IO.Path.Combine(cwd,"gui-settings.txt");
        Process.StartInfo.EnvironmentVariables["FILETRANSFER_GUI_SETTINGS"]=settings;
        Process.StartInfo.EnvironmentVariables["FILETRANSFER_METADATA_ROOT"]=System.IO.Path.Combine(cwd,"transfer-metadata");
        if(seedSettings&&!System.IO.File.Exists(settings)){
            string received=System.IO.Path.Combine(cwd,"received");System.IO.Directory.CreateDirectory(received);
            System.IO.File.WriteAllText(settings,received,new UTF8Encoding(false));
        }
        if(legacy)Process.StartInfo.Arguments="--legacy-cli";else {if(!multiclient)Process.StartInfo.Arguments="--compat-v1";Process.StartInfo.EnvironmentVariables["FILETRANSFER_LAN_PORT"]="0";}
        Process.Start();
        var watch=Stopwatch.StartNew();
        while(watch.ElapsedMilliseconds<5000){
            EnumWindows((h,d)=>{uint id;GetWindowThreadProcessId(h,out id);if(id==Process.Id){if(Class(h)=="FileTransferAppGui")Window=h;if(Class(h)=="FileTransferAppChat")Chat=h;}return true;},IntPtr.Zero);
            if(Window!=IntPtr.Zero&&Chat!=IntPtr.Zero&&GetDlgItem(Window,128)!=IntPtr.Zero){Responsive();return;}
            if(Process.HasExited)throw new Exception("GUI exited before window creation");Thread.Sleep(20);
        }
        Dispose();throw new Exception("GUI window creation timed out");
    }
    public void Click(int id){if(!PostMessage(Window,0x111,new IntPtr(id),IntPtr.Zero))throw new Exception("Post failed");}
    public IntPtr Control(int id){return GetDlgItem(id>=108&&id<=110?Chat:Window,id);}
    public void Set(int id,string text){SetHandle(Control(id),text);}
    static void SetHandle(IntPtr hwnd,string text){IntPtr r;if(hwnd==IntPtr.Zero||SendMessageTimeout(hwnd,0xc,IntPtr.Zero,text,2,1000,out r)==IntPtr.Zero)throw new Exception("Set text timeout");}
    public string Text(int id){
        // Historical suites used one transcript for messages and activity.
        // Keep that diagnostic view while targeting the real separate windows.
        if(id==108){string all=Read(GetDlgItem(Window,143))+"\r\n"+Read(Control(108));return all.Length>80000?all.Substring(all.Length-80000):all;}
        return Read(Control(id));
    }
    static string Read(IntPtr control){
        IntPtr r;var b=new StringBuilder(65536);
        if(SendMessageTimeout(control,0xd,new IntPtr(b.Capacity),b,2,1000,out r)==IntPtr.Zero)throw new Exception("Get text timeout");return b.ToString();
    }
    public void Wait(int id,string text,int timeout=10000){var w=Stopwatch.StartNew();while(w.ElapsedMilliseconds<timeout){if(Text(id).Contains(text))return;Thread.Sleep(20);}throw new Exception("GUI missing "+text+"; status="+Text(107)+" log="+Text(108));}
    public void Responsive(){IntPtr r;if(SendMessageTimeout(Window,0,IntPtr.Zero,"",2,500,out r)==IntPtr.Zero)throw new Exception("UI blocked");}
    public void Enter(){IntPtr r;if(SendMessageTimeout(Control(109),0x100,new IntPtr(13),IntPtr.Zero,2,1000,out r)==IntPtr.Zero)throw new Exception("Enter timeout");}
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
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd,IntPtr after,int x,int y,int w,int h,uint flags);
    [DllImport("shell32.dll")] static extern int SHGetKnownFolderPath(ref Guid id,uint flags,IntPtr token,out IntPtr path);
    public static string Downloads(){Guid id=new Guid("374DE290-123F-4565-9164-39C4925E467B");IntPtr path;int hr=SHGetKnownFolderPath(ref id,0,IntPtr.Zero,out path);if(hr!=0)Marshal.ThrowExceptionForHR(hr);try{return Marshal.PtrToStringUni(path);}finally{Marshal.FreeCoTaskMem(path);}}
    public bool Enabled(int id){return IsWindowEnabled(Control(id));}
    public void Resize(int width,int height){SetWindowPos(Window,IntPtr.Zero,0,0,width,height,6);Thread.Sleep(100);Responsive();}
    public void Folder(string path,bool cancel=false){Folder(path,cancel,128);}
    public void Folder(string path,bool cancel,int button){
        Click(button);IntPtr dialog=IntPtr.Zero;var watch=Stopwatch.StartNew();
        while(watch.ElapsedMilliseconds<5000){EnumWindows((h,d)=>{uint id;GetWindowThreadProcessId(h,out id);if(id==Process.Id&&Class(h)=="#32770")dialog=h;return true;},IntPtr.Zero);if(dialog!=IntPtr.Zero)break;Thread.Sleep(20);}
        if(dialog==IntPtr.Zero)throw new Exception("Folder dialog missing");
        if(cancel){PostMessage(dialog,0x111,new IntPtr(2),IntPtr.Zero);Thread.Sleep(200);return;}
        Thread.Sleep(300);IntPtr edit=IntPtr.Zero;var desc=new StringBuilder();
        EnumChildWindows(dialog,(h,d)=>{desc.Append(Class(h)+":"+GetDlgCtrlID(h)+" parent="+GetDlgCtrlID(GetParent(h))+"; ");if(Class(h)=="Edit"&&(GetDlgCtrlID(h)==1148||GetDlgCtrlID(GetParent(h))==1148||GetDlgCtrlID(h)==1152||GetDlgCtrlID(GetParent(h))==1152))edit=h;return true;},IntPtr.Zero);
        if(edit==IntPtr.Zero)throw new Exception("Folder edit missing: "+desc);
        SetHandle(edit,path);PostMessage(dialog,0x111,new IntPtr(1),IntPtr.Zero);Thread.Sleep(300);PostMessage(dialog,0x111,new IntPtr(1),IntPtr.Zero);
        Wait(button==155?154:127,path);
    }
    public string[] Devices(){IntPtr r;if(SendMessageTimeout(GetDlgItem(Window,134),0x18b,IntPtr.Zero,"",2,1000,out r)==IntPtr.Zero)throw new Exception("List timeout");int count=r.ToInt32();var rows=new string[count];for(int i=0;i<count;i++){var b=new StringBuilder(1024);SendMessageTimeout(GetDlgItem(Window,134),0x189,new IntPtr(i),b,2,1000,out r);rows[i]=b.ToString();}return rows;}
    public void SelectDevice(int index){IntPtr r;SendMessageTimeout(GetDlgItem(Window,134),0x186,new IntPtr(index),"",2,1000,out r);}
    public string[] List(int id){IntPtr r;var h=GetDlgItem(Window,id);SendMessageTimeout(h,0x18b,IntPtr.Zero,"",2,1000,out r);int count=r.ToInt32();var rows=new string[count];for(int i=0;i<count;i++){var b=new StringBuilder(1024);SendMessageTimeout(h,0x189,new IntPtr(i),b,2,1000,out r);rows[i]=b.ToString();}return rows;}
    public void SelectSession(string peerId){var rows=List(152);for(int i=0;i<rows.Length;i++)if(rows[i].Contains("["+peerId.Substring(0,8)+"]")){IntPtr r;SendMessageTimeout(GetDlgItem(Window,152),0x186,new IntPtr(i),"",2,1000,out r);PostMessage(Window,0x111,new IntPtr((1<<16)|152),IntPtr.Zero);Thread.Sleep(150);return;}throw new Exception("Active session missing: "+peerId);}
    public void SetRole(bool server){IntPtr r;SendMessageTimeout(GetDlgItem(Window,149),0x14e,new IntPtr(server?1:0),"",2,1000,out r);}
    public void ApplyProfileWithConfirmation(bool accept){Click(151);IntPtr dialog=IntPtr.Zero;var w=Stopwatch.StartNew();while(w.ElapsedMilliseconds<5000){EnumWindows((h,d)=>{uint pid;GetWindowThreadProcessId(h,out pid);if(pid==Process.Id&&Class(h)=="#32770")dialog=h;return true;},IntPtr.Zero);if(dialog!=IntPtr.Zero)break;Thread.Sleep(20);}if(dialog==IntPtr.Zero)throw new Exception("Profile confirmation missing");PostMessage(dialog,0x111,new IntPtr(accept?1:2),IntPtr.Zero);Thread.Sleep(200);}
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,StringBuilder b,int count);
    public void UseChat(string peerId){IntPtr found=IntPtr.Zero;EnumWindows((h,d)=>{uint id;GetWindowThreadProcessId(h,out id);if(id==Process.Id&&Class(h)=="FileTransferAppChat"){var b=new StringBuilder(256);GetWindowText(h,b,b.Capacity);if(b.ToString().Contains("["+peerId.Substring(0,8)+"]"))found=h;}return true;},IntPtr.Zero);if(found==IntPtr.Zero)throw new Exception("Peer chat missing");Chat=found;}
    public string ChatText(){return Read(GetDlgItem(Chat,108));}
    public void StaleChatCommand(IntPtr source){PostMessage(Window,0x111,new IntPtr(110),source);}
    public void ChatButton(){IntPtr r;if(SendMessageTimeout(Control(110),0xf5,IntPtr.Zero,IntPtr.Zero,2,1000,out r)==IntPtr.Zero)throw new Exception("Chat button timeout");}
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetHandleInformation(IntPtr handle,uint mask,uint flags);
    public static void PreventInheritance(IntPtr handle){if(!SetHandleInformation(handle,1,0))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());}
    public void Close(){PostMessage(Window,0x10,IntPtr.Zero,IntPtr.Zero);if(!Process.WaitForExit(7000))throw new Exception("GUI shutdown timed out");if(Process.ExitCode!=0)throw new Exception("GUI exit failure");}
    public void Dispose(){if(!Process.HasExited){Process.Kill();Process.WaitForExit();}Process.Dispose();}
}
