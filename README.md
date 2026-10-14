# FileTransferApp

Windows LAN 파일·텍스트 전송 프로그램(C++20 / Win32 / Winsock). 기본 사용자 앱은 **FileTransferAppGui.exe**입니다. 진단·자동 테스트용 **FileTransferApp.exe** CLI도 유지합니다. 현재 개발 브랜치의 검증 제한은 [STAGE8](tests/STAGE8.md)에 기록했습니다.

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

기본 GUI는 실행 중인 앱을 LAN 기기 목록에서 발견합니다. **목록에서 상대를 선택 → 선택 기기 연결 요청 → 상대의 연결 승인** 순서로 연결합니다. 검색만으로 자동 연결하지 않습니다. 이름과 표시된 ID는 인증된 신원이 아니므로 IP도 확인하세요. 연결 승인과 파일 수신 승인은 별개입니다.

검색이 되지 않으면 상대 GUI의 LAN 상태에 표시된 TCP 포트와 IPv4를 직접 입력하고 **IP 직접 연결 요청**을 사용하세요. 기본 수신 포트는 9000이며 사용 중이면 임시 포트로 대체합니다. `FILETRANSFER_LAN_PORT` 환경 변수로 고정 포트 또는 0(OS 선택)을 지정할 수 있습니다. 연결 후 양쪽 모두 메시지와 파일을 보낼 수 있습니다. Enter로 텍스트를 전송합니다.

발견은 UDP multicast **239.255.90.8:9001**, TTL 1을 사용합니다. 방화벽/공유기/VPN/멀티 NIC 환경에서는 제한될 수 있으며, UDP 실패 시에도 직접 연결은 유지합니다. 이름·앱 존재가 활성 IPv4 네트워크(VPN 포함)에 광고될 수 있습니다. 연결 승인은 강한 신원 인증이나 암호화를 제공하지 않습니다.

**파일 선택** 또는 탐색기에서 파일 하나를 창에 놓아 선택하고 **파일 보내기**를 누릅니다. 상대는 요청의 파일명·크기를 확인하고 승인 또는 거절합니다. 송신/수신 취소 버튼은 각 방향에 적용됩니다. 100%는 데이터 진행률이며 송신 성공은 저장 완료 ACK 또는 검증된 완료 조회 응답 후 표시됩니다.

연결이 끊기면 목록 선택 또는 직접 IP로 다시 연결 요청하고 상대 승인을 받습니다. 원본 파일을 다시 선택하고 기존 전송 ID를 입력하여 **이어받기** 또는 **완료 조회**를 실행합니다. ID는 새 전송 때 표시되므로 별도로 보관하세요. 동일 원본·복구 메타데이터·수신 저장 위치가 필요합니다. 원본 변경이나 검증 실패는 거부됩니다.

GUI 수신은 처음에는 Windows **Downloads** 폴더를 사용합니다. 파일 전송 영역의 **폴더 변경**에서 저장 폴더를 선택하면 `%LOCALAPPDATA%\FileTransferApp\download-folder.txt`에 저장되어 재실행 후에도 유지합니다. 취소하거나 검증/설정 저장에 실패하면 기존 설정을 유지합니다. 폴더 오류는 메시지 기록에 표시하며 유효한 폴더 선택 전 수신 승인을 막습니다.

승인 시 저장 폴더가 확정되므로 전송 도중 변경은 다음 승인부터 적용됩니다. 이어받기·완료 조회는 원래 수신 폴더를 선택해야 합니다. 새 GUI 복구 정보는 디렉터리 ID에도 묶여 있으므로 파일과 메타데이터를 다른 폴더로 옮겨 복구할 수 없습니다. 이전 버전의 복구 정보는 기존 파일 ID 검증을 유지합니다. 수신 요청별 별도 저장 버튼은 없으며 승인 전에 기본 폴더를 변경할 수 있습니다. 기존 파일은 덮어쓰지 않습니다.

CLI 수신은 계속 **현재 작업 디렉터리의 received/** 아래에 저장합니다. GUI와 다르므로 복구할 때 사용한 프로그램과 저장 폴더를 확인하세요. 진단·휴대 실행은 `FILETRANSFER_GUI_SETTINGS` 환경 변수로 절대경로의 설정 파일 위치를 지정할 수 있습니다.

창은 네 그룹으로 구분되며 크기 변경과 DPI에 맞춰 배치됩니다. 작은 높이에서는 세로 스크롤로 아래 진행률·이어받기 버튼에 접근하세요. 연결·파일 작업 상태에 따라 사용 가능한 버튼만 활성화합니다.

LAN에서는 상대 PC의 `ipconfig`에 표시되는 공유기 내부 IPv4를 사용합니다. Windows 방화벽에서 앱의 실제 TCP 수신 포트와 UDP 9001 multicast 송수신 허용이 필요할 수 있습니다. 회사 장비는 관리자에게 승인된 Private 네트워크 범위의 규칙을 요청하세요. 프로그램이나 테스트는 방화벽·SAC·보안 정책을 변경하지 않습니다. 프로토콜 버전은 양쪽 CLI의 `--version`으로 확인하세요(현재 6).

기존 CLI는 파일 프로토콜 6만 사용하며 새 연결 승인 greeting을 사용하지 않습니다. 기본 GUI와 자동 호환되지 않고 승인 없는 경로로 자동 대체하지 않습니다. 진단·기존 회귀에 한해 GUI를 `--legacy-cli`로 실행하면 이전 수동 Host/Client 모드를 사용합니다. 이 명시적 모드는 새 연결 승인을 제공하지 않습니다.

```powershell
& ./FileTransferApp/x64/Release/FileTransferAppGui.exe --legacy-cli
```

CLI 예:

```powershell
& ./x64/Release/FileTransferApp.exe --version
& ./x64/Release/FileTransferApp.exe --host --port 9000
& ./x64/Release/FileTransferApp.exe --connect 127.0.0.1 --port 9000
```

실제 두 PC, 탐색기 드래그 앤 드롭 및 DPI/IME 수동 검증은 아직 수행하지 않았습니다. GUI 전송 이력 영속화, 계정, 인터넷 원격 연결은 포함하지 않습니다. 실제 두 PC LAN 검증 절차와 제한은 STAGE8 문서를 확인하세요.
