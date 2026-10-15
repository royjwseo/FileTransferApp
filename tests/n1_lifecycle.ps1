param([string]$Exe="$PSScriptRoot/../FileTransferApp/x64/Debug/FileTransferAppGui.exe",[int]$SoakSeconds=60)
$ErrorActionPreference='Stop';. "$PSScriptRoot/n_to_1.ps1" -Exe $Exe -HelpersOnly
Add-Type -Path "$PSScriptRoot/ChatProbe.cs"
$s=$null;$a=$null;$b=$null
try{
 $s=App 'server' $true 'lifecycle-server';$a=App 'a' $false 'a';$b=App 'b' $false 'b';Connect $a $s;Connect $b $s;Count $s 2
 Select-Peer $s $a;$s.G.Click(142);$chatA=$s.G.Chat;Select-Peer $s $b;$s.G.UseChat($a.ID);$s.G.Set(109,'chat-window-a-with-b-selected');$s.G.ChatButton();ChatWait $a $s 'chat-window-a-with-b-selected';$b.G.UseChat($s.ID);Assert (!$b.G.ChatText().Contains('chat-window-a-with-b-selected')) 'Selected B received A chat window send';Pass 'chat HWND routes its own session independently of main selection'
 $s.G.UseChat($a.ID);[ChatProbe]::Close($s.G.Chat);Assert (![ChatProbe]::IsWindowVisible($s.G.Chat)) 'Chat close did not hide';Count $s 2;Send $a $s 'hidden-chat-receives';ChatWait $s $a 'hidden-chat-receives';Pass 'closing peer chat preserves TCP and receives into its isolated history'
 Select-Peer $s $b;$s.G.Click(142);$s.G.UseChat($b.ID);$s.G.Set(109,'enter-owner-route');$s.G.Enter();ChatWait $b $s 'enter-owner-route';Pass 'Enter in a peer chat invokes that chat owner routing'
 $beforeHandles=$s.G.Process.HandleCount
 for($i=0;$i -lt 6;$i++){Select-Peer $a $s;$a.G.Click(105);Count $s 1;Count $a 0;Connect $a $s;Count $s 2;Send $b $s ('b-still-live-'+$i);ChatWait $s $b ('b-still-live-'+$i);Send $a $s ('new-session-'+$i);ChatWait $s $a ('new-session-'+$i)}
 $s.G.Process.Refresh();Assert (($s.G.Process.HandleCount-$beforeHandles) -le 24) 'Reconnect handle growth';Pass 'six reconnects preserve other session, reject stale state and bound resource growth'
 $watch=[Diagnostics.Stopwatch]::StartNew();$n=0
 while($watch.Elapsed.TotalSeconds -lt $SoakSeconds){Send $a $s ('soak-a-'+$n);ChatWait $s $a ('soak-a-'+$n);Send $s $b ('soak-b-'+$n);ChatWait $b $s ('soak-b-'+$n);Count $s 2;$n++;Start-Sleep -Milliseconds 1000}
 Assert ($SoakSeconds -ge 60) 'Soak fixture must run at least 60 seconds';Pass 'at least 60 seconds of sustained independent messaging and stable connection counts'
}finally{foreach($x in @($a,$b,$s)){if($x){try{[IO.File]::WriteAllText((Join-Path $x.Dir 'ui.log'),$x.G.Text(108))}catch{};try{$x.G.Dispose()}catch{}}};"N1 lifecycle artifacts: $run"}
