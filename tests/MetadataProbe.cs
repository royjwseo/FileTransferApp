using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class MetadataProbe {
 [StructLayout(LayoutKind.Sequential)] struct Info {public uint attributes;public System.Runtime.InteropServices.ComTypes.FILETIME created,accessed,written;public uint volume,sizeHigh,sizeLow,links,indexHigh,indexLow;}
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFile(string p,uint access,uint share,IntPtr security,uint disposition,uint flags,IntPtr template);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle h,out Info i);
 static string Prefix(string directory){using(var h=CreateFile(directory,0x80,3,IntPtr.Zero,3,0x02200000,IntPtr.Zero)){
  Info i;if(h.IsInvalid||!GetFileInformationByHandle(h,out i)||(i.attributes&0x400)!=0)throw new Exception("Unsafe metadata fixture directory");
  return ((ulong)i.volume).ToString("x16")+(((ulong)i.indexHigh<<32)|i.indexLow).ToString("x16")+"-";
 }}
 public static string PathFor(string cwd,string directory,string id){return PathFor(cwd,directory,id,"");}
 public static string PathFor(string cwd,string directory,string id,string peer){
  string prefix=Prefix(directory);byte[] identity=new byte[16];for(int i=0;i<16;i++)identity[i]=Convert.ToByte(prefix.Substring(i*2,2),16);
  byte[] text=System.Text.Encoding.ASCII.GetBytes(id+peer),data=new byte[16+text.Length];Array.Copy(identity,data,16);Array.Copy(text,0,data,16,text.Length);
  using(var hash=System.Security.Cryptography.SHA256.Create()){return Path.Combine(cwd,"transfer-metadata",BitConverter.ToString(hash.ComputeHash(data)).Replace("-","").ToLowerInvariant()+".fta");}
 }
 public static string Find(string cwd,string directory){var files=Directory.GetFiles(Path.Combine(cwd,"transfer-metadata"),"*.fta");if(files.Length!=1)throw new Exception("Expected exactly one fixture metadata record");return files[0];}
}
