using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
public static class ScrollProbe {
 [StructLayout(LayoutKind.Sequential)] public struct Info {public uint cb,mask; public int min,max; public uint page; public int pos,track;}
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int left,top,right,bottom;}
 [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern bool GetScrollInfo(IntPtr h,int bar,ref Info info);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] static extern int MapWindowPoints(IntPtr from,IntPtr to,ref Rect r,uint count);
 [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] static extern bool RedrawWindow(IntPtr h,IntPtr r,IntPtr region,uint flags);
 [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
 [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h,IntPtr dc);
 [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr dest,int x,int y,int w,int h,IntPtr src,int sx,int sy,uint op);
 [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h,int cmd);
 public static Info State(IntPtr h){Info i=new Info{cb=(uint)Marshal.SizeOf(typeof(Info)),mask=23};if(!GetScrollInfo(h,1,ref i))throw new Exception("scroll info");return i;}
 public static int Y(IntPtr h,int id){Rect r;GetWindowRect(GetDlgItem(h,id),out r);MapWindowPoints(IntPtr.Zero,h,ref r,2);return r.top;}
 public static void Bar(IntPtr h,int cmd){SendMessage(h,0x115,new IntPtr(cmd),IntPtr.Zero);}
 public static void Wheel(IntPtr h,int delta){SendMessage(h,0x20a,new IntPtr(unchecked(delta<<16)),IntPtr.Zero);}
 public static void Restore(IntPtr h){ShowWindow(h,6);Thread.Sleep(50);ShowWindow(h,9);Thread.Sleep(100);}
 static Bitmap Capture(IntPtr h){Rect r;GetClientRect(h,out r);Bitmap b=new Bitmap(r.right,r.bottom);using(Graphics g=Graphics.FromImage(b)){IntPtr d=g.GetHdc(),s=GetDC(h);try{if(!BitBlt(d,0,0,b.Width,b.Height,s,0,0,0x00CC0020))throw new Exception("capture failed");}finally{ReleaseDC(h,s);g.ReleaseHdc(d);}}return b;}
 public static int CompareRepaint(IntPtr h,string path){Thread.Sleep(100);using(Bitmap a=Capture(h)){a.Save(path+"-before.png");RedrawWindow(h,IntPtr.Zero,IntPtr.Zero,0x585);Thread.Sleep(100);using(Bitmap b=Capture(h)){b.Save(path+"-repaint.png");int changed=0;for(int y=0;y<a.Height;y++)for(int x=0;x<a.Width;x++)if(a.GetPixel(x,y)!=b.GetPixel(x,y))changed++;return changed;}}}
}
