using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class ChatProbe {
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int cmd);
 [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr h);
 [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int size,uint flags);
 [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern void keybd_event(byte key,byte scan,uint flags,UIntPtr extra);
 public static void Close(IntPtr h){PostMessage(h,0x10,IntPtr.Zero,IntPtr.Zero);Thread.Sleep(150);}
 public static void Resize(IntPtr h,int w,int height){SetWindowPos(h,IntPtr.Zero,0,0,w,height,0x46);Thread.Sleep(150);}
 public static void Focus(IntPtr h,IntPtr child){uint pid;uint gui=GetWindowThreadProcessId(h,out pid),current=GetCurrentThreadId(),foreground=GetWindowThreadProcessId(GetForegroundWindow(),out pid);bool attachForeground=foreground!=current&&foreground!=gui&&AttachThreadInput(current,foreground,true);bool attached=AttachThreadInput(current,gui,true);try{ShowWindow(h,5);SetForegroundWindow(h);SetFocus(child);}finally{if(attached)AttachThreadInput(current,gui,false);if(attachForeground)AttachThreadInput(current,foreground,false);}Thread.Sleep(100);if(GetForegroundWindow()!=h)throw new Exception("Cannot acquire chat foreground for real-keyboard test");}
 public static void ShiftEnter(IntPtr h,IntPtr input){Focus(h,input);keybd_event(16,0,0,UIntPtr.Zero);keybd_event(13,0,0,UIntPtr.Zero);keybd_event(13,0,2,UIntPtr.Zero);keybd_event(16,0,2,UIntPtr.Zero);Thread.Sleep(200);}
 public static bool Missing(IntPtr h,int id){return GetDlgItem(h,id)==IntPtr.Zero;}
 [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr h,int id);
}
