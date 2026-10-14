# FileTransferApp

Windows LAN 파일·텍스트 전송 프로그램(C++20 / Win32 / Winsock). 기본 사용자 앱은 **FileTransferAppGui.exe**입니다. 진단·자동 테스트용 **FileTransferApp.exe** CLI도 유지합니다. 현재 개발 브랜치의 검증 제한은 [STAGE7](tests/STAGE7.md)에 기록했습니다.

## 빌드와 실행

Visual Studio 2026 Community, Desktop development with C++, Windows SDK가 필요합니다. `FileTransferApp.slnx`를 열고 `FileTransferAppGui`를 시작 프로젝트로 지정하여 Debug/Release x64로 빌드합니다. 외부 GUI 프레임워크는 없습니다.

Developer PowerShell에서:

```powershell
msbuild FileTransferApp.slnx /m /p:Configuration=Debug /p:Platform=x64
msbuild FileTransferApp.slnx /m /p:Configuration=Release /p:Platform=x64
& ./FileTransferApp/x64/Release/FileTransferAppGui.exe
```

GUI는 `wWinMain`, CLI는 기존 `main`을 사용하는 별도 실행 파일입니다. GUI 인자 없는 실행은 창을 열며, CLI 인자 없는 실행은 기존 콘솔 메뉴를 유지합니다. 기존 자동 테스트는 CLI 실행 파일을 사용하므로 GUI 대화상자가 나타나지 않습니다.

## 두 사용자 연결

한쪽은 포트(기본 9000)를 입력하고 **연결 대기**, 다른 쪽은 상대 PC의 LAN IPv4 주소와 같은 포트를 입력하고 **연결**을 누릅니다. localhost 검증은 `127.0.0.1`을 사용합니다. 연결 후 양쪽 모두 메시지와 파일을 보낼 수 있습니다. Enter로 텍스트를 전송합니다.

**파일 선택** 또는 탐색기에서 파일 하나를 창에 놓아 선택하고 **파일 보내기**를 누릅니다. 상대는 요청의 파일명·크기를 확인하고 승인 또는 거절합니다. 송신/수신 취소 버튼은 각 방향에 적용됩니다. 100%는 데이터 진행률이며 송신 성공은 저장 완료 ACK 또는 검증된 완료 조회 응답 후 표시됩니다.

연결이 끊기면 양쪽에서 이전 역할의 **재연결**을 사용합니다. 원본 파일을 다시 선택하고 기존 전송 ID를 입력하여 **이어받기** 또는 **완료 조회**를 실행합니다. ID는 새 전송 때 표시되므로 별도로 보관하세요. 동일 원본·복구 메타데이터·수신 저장 위치가 필요합니다. 원본 변경이나 검증 실패는 거부됩니다.

수신 파일과 복구 정보는 **현재 작업 디렉터리의 received/** 아래에 저장합니다. Explorer와 Visual Studio의 작업 디렉터리가 다를 수 있으므로 복구 때 동일한 폴더에서 실행하세요. 기존 파일은 덮어쓰지 않습니다.

LAN에서는 상대 PC의 `ipconfig`에 표시되는 공유기 내부 IPv4를 사용합니다. Host의 Windows 방화벽에서 해당 앱/포트의 TCP 인바운드 허용이 필요할 수 있습니다. 회사 장비는 관리자에게 승인된 Private 네트워크 범위의 규칙을 요청하세요. 프로그램이나 테스트는 방화벽·SAC·보안 정책을 변경하지 않습니다. 프로토콜 버전은 양쪽 CLI의 `--version`으로 확인하세요(현재 6).

CLI 예:

```powershell
& ./FileTransferApp/x64/Release/FileTransferApp.exe --version
& ./FileTransferApp/x64/Release/FileTransferApp.exe --host --port 9000
& ./FileTransferApp/x64/Release/FileTransferApp.exe --connect 127.0.0.1 --port 9000
```

실제 두 PC, 탐색기 드래그 앤 드롭 및 DPI/IME 수동 검증은 아직 수행하지 않았습니다. LAN 검색, 역할 자동화, GUI 전송 이력 영속화는 포함하지 않습니다.
