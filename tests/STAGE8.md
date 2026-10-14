# 8단계 — LAN 기기 발견 및 사용자 선택 연결

기준: feature/stage7-1-download-ui `0006f18`. 작업 브랜치: feature/stage8-lan-discovery. 기존 브랜치·커밋과 main을 보존합니다. 이번 결과는 feature 브랜치에만 커밋/push하며 main 병합과 강제 push는 하지 않습니다.

## 설계 및 기존 구조와의 관계

기존 Network는 연결된 TCP 소켓과 전송 스레드·잠금을, Protocol은 requestId 기반 송수신 상태 머신을, TransferStorage는 안전 저장·SHA-256·복구 소유권을 담당했습니다. GUI Controller는 관리자/수신/송신/승인 작업자를 소유하고 typed event queue를 UI 타이머로 전달했습니다.

새 **LanService**는 GUI 관리자 스레드에만 소유되는 발견/연결 승인 계층입니다. UDP와 승인 대기 TCP 소켓을 nonblocking으로 poll합니다. 승인 전에는 Network를 만들거나 파일 프로토콜 파서를 실행하지 않습니다. 승인된 소켓만 `Network::AdoptApprovedSocket`으로 인계하고, 이후 기존 송수신·다운로드 폴더·취소·이어받기·저장 ACK 동작을 재사용합니다. 파일 전송 프로토콜 **6은 변경하지 않았습니다**.

변경 파일: LanService.h/cpp, GUI 프로젝트 등록, Gui.cpp/GuiMain.cpp/Gui.h, Events.h의 발견/승인 이벤트와 snapshot coalescing, Network의 소켓 인계 API, GuiDriver.cs, stage8_lan.ps1, README 및 본 문서. CLI 진입점과 기존 Protocol/TransferStorage 파일 로직은 변경하지 않았습니다.

## UDP 발견 프로토콜 1

주소: **239.255.90.8:9001/UDP**, IPv4 multicast TTL **1**. 일반 PC·장비의 포트를 순회하거나 IP 대역을 스캔하지 않습니다. 이 그룹에서 동일 앱 프로토콜에 응답하는 인스턴스만 표시합니다.

UTF-8 datagram 형식:

```text
FTA-DISC|1|Q|<instance32hex>|<tcpPort>|<displayName>
FTA-DISC|1|R|<instance32hex>|<tcpPort>|<displayName>
```

- 앱 식별자 FTA-DISC, 발견 버전 1, Q=query/R=response.
- instance ID: 프로세스 시작 시 CNG 난수 128비트(소문자 hex 32자), 영속 전송 ID와 무관.
- 표시 이름: Windows 컴퓨터 이름. 유효 UTF-8 최대 96바이트, 구분자/제어 문자/방향 제어 문자 거부. 적합하지 않으면 Windows PC로 대체.
- TCP 포트는 숫자 1~65535. **패킷에는 IP를 넣지 않으며 recvfrom 출발지 IPv4만 사용**합니다.
- 최대 datagram 256바이트, 정확한 필드 수·버전·문자열·숫자 파싱. 잘못된 패킷은 무시합니다.
- Q를 수신하면 R을 같은 그룹으로 송신합니다. 공유 UDP 포트를 사용하는 같은 PC의 여러 프로세스도 응답을 받을 수 있습니다. R은 요청자와 그룹의 다른 앱에도 전달될 수 있습니다.
- 3초마다 검색, 새로고침은 최소 1초 간격. 응답 송신도 최대 1초에 한 번으로 제한합니다.
- 자신의 ID는 제외하고 ID별 중복 제거, 응답 후 8초가 지나면 만료, 최대 64개 후보를 보관합니다. 같은 인스턴스에서 여러 인터페이스 주소가 보이면 최근 응답 주소를 표시합니다.

GetAdaptersAddresses로 활성 IPv4 주소를 최대 16개 선택하여 그룹에 가입하고 각 인터페이스로 송신합니다. 루프백/가상 NIC/VPN도 가입 가능한 경우 포함합니다. NIC 변경은 실행 중 재열거하지 않으므로 네트워크 구성이 바뀌면 재실행하세요. VPN의 multicast 전달/격리, AP client isolation, 방화벽, 다른 서브넷, IGMP 제한에 따라 검색이 실패할 수 있습니다. TTL 1은 일반 라우터를 넘어가는 검색을 위한 기능이 아닙니다. VPN 인터페이스가 multicast를 전달하면 그 연결에서도 이름·앱 존재가 노출될 수 있습니다.

UDP 응답과 표시 이름/ID는 **미인증 정보**입니다. 같은 ID나 컴퓨터 이름이 보인다고 신원을 확인한 것이 아닙니다. 악성 LAN 참가자는 목록을 위조하거나 후보의 주소를 갱신할 수 있습니다. 후보 테이블·패킷·큐·요청 수를 제한하지만 강한 인증/암호화나 공격자에 대한 서비스 가용성을 보장하지 않습니다.

UDP 초기화/바인드/가입이 실패해도 TCP 승인 리스너는 유지하므로 **직접 IP 연결 요청은 사용 가능**합니다. 오류를 GUI 이력에 기록합니다.

## TCP 연결 승인 게이트 1

기본 GUI는 시작 시 수신 승인 준비용 TCP listen만 생성합니다. 기본 포트 9000을 독점 바인드하고 사용 중이면 OS 임시 포트로 대체합니다. 실제 포트는 LAN 상태에 표시하고 UDP로 광고합니다. listen 상태 자체는 파일 전송 세션이 아닙니다.

환경 변수 `FILETRANSFER_LAN_PORT`로 수신 포트 0~65535를 지정할 수 있습니다(0은 OS 선택). 동일 PC 테스트는 0을 사용하여 기존 CLI suite의 TCP 9000과 분리합니다. GUI의 직접 연결 포트 입력은 **상대방** 포트입니다.

연결 순서:

1. 사용자가 목록에서 기기를 선택하고 연결 요청, 또는 IPv4/포트를 입력하고 IP 직접 연결 요청을 누릅니다.
2. 비동기 connect 후 요청자가 아래 C1 한 줄을 전송합니다. 인스턴스 ID, 요청마다 새 128비트 nonce, 표시 이름을 포함합니다.
3. 상대는 요청자의 **실제 TCP source IP**와 미인증 이름을 별도 연결 요청 영역에 표시합니다. 이 단계에서는 텍스트/파일 기능이 비활성화됩니다.
4. 사용자가 연결 승인 또는 연결 거절을 누릅니다. 로컬 증가 token이 현재 후보와 맞는 경우만 처리합니다.
5. 승인 시 상대는 A1을 전송한 뒤 해당 소켓을 Network에 넘깁니다. 요청자는 nonce 및 발견 목록에서 선택한 instance ID가 맞는 A1을 받은 뒤에만 Network로 넘깁니다.
6. 이후 기존 파일 전송 프로토콜을 사용합니다. **연결 승인과 파일 수신 승인은 별개**입니다.

```text
FTA-C1|<requesterInstance32hex>|<requestNonce32hex>|<name>\n
FTA-A1|<responderInstance32hex>|<sameNonce32hex>\n
FTA-N1|<responderInstance32hex>|<sameNonce32hex>\n
```

마지막 `\n`은 실제 LF 한 바이트입니다. 게이트는 최대 256바이트 한 줄만 읽으며 승인 후 파일 프로토콜 바이트를 미리 소비하지 않습니다. C1 이후 승인 대기 중 다른 바이트를 보내면 거부합니다. 잘못된 greeting/버전/nonce/선택 ID, 연결 종료, 거절은 후보를 닫습니다. TCP 연결부터 승인까지 전체 15초 제한이며 부분 입력으로 시간을 연장하지 않습니다. 예전 요청의 응답이나 UI token은 새 요청을 승인할 수 없습니다.

동시에 inbound/outbound 후보는 각각 최대 1개이며 listen backlog 요청값은 4입니다. Poll마다 accept 최대 4개, UDP 최대 64개, 각 TCP 후보의 입력 최대 256바이트를 처리합니다. 이미 전송 세션이 있으면 신규 수신 연결은 닫고 사용자 측 중복 요청도 거부합니다. 후보 하나가 승인되면 다른 후보는 정리합니다.

## 동시 연결 충돌 규칙

서로 요청이 겹치면 **작은 instance ID의 outbound 연결 하나**를 남깁니다(소문자 hex의 사전 순서). 큰 ID 측은 자신의 outbound를 닫고 inbound 승인 화면을 유지합니다. 같은 발견 인스턴스를 선택한 클릭이 상대 greeting보다 조금 늦게 큐에 도착한 경우에도 이 규칙을 적용합니다.

서로 다른 상대를 선택한 요청이나 ID가 없는 직접 연결을 기존 inbound 요청으로 임의 대체하지 않습니다. 이 경우 이미 대기 중인 요청이 있으면 busy로 거부하여 사용자가 먼저 정리하도록 합니다. ID 기반 선출은 중복 소켓을 줄이기 위한 규칙이며 상대 인증이 아닙니다. 선출 후에도 상대 사용자의 승인이 필요합니다. 선출만으로 연결하거나 파일을 받지 않습니다.

초기 Release 테스트에서 늦게 도착한 클릭이 단순 busy 처리되어 ID 선출 규칙과 다른 후보가 남는 경합을 재현했습니다. `Request`에서도 동일한 인스턴스에 한해 선출을 적용하도록 수정했고 양 구성 신규 테스트를 다시 실행했습니다. 승인 없이 세션이 열린 현상은 없었습니다.

## GUI 및 CLI 호환성

기본 실행 파일: `FileTransferApp/x64/Release/FileTransferAppGui.exe`.

- 상단 LAN 기기 목록: 이름 / IPv4:TCP포트 / 발견(미인증) / instance ID 앞 8자리.
- 새로고침, 선택 기기 연결 요청, 연결 승인·거절, 연결 상태.
- 직접 IPv4/포트 입력 및 **IP 직접 연결 요청**도 동일 승인 게이트 사용.
- 연결 해제는 활성 세션 또는 승인 대기 요청을 정리합니다. 재연결은 목록 선택/직접 요청을 다시 실행하고 상대 승인을 다시 받아야 합니다.
- 기존 메시지/파일/다운로드 설정/양방향 진행률/취소/이어받기/완료 조회는 유지합니다.
- 창 크기/DPI 재배치 및 작은 높이의 세로 스크롤을 유지합니다. 새 LAN 영역을 위에 추가했으므로 작은 화면에서는 아래 파일 버튼으로 스크롤하세요.

기존 `FileTransferApp.exe` CLI는 **파일 프로토콜 6만 사용**하며 새로운 연결 승인 greeting을 사용하지 않습니다. 기본 GUI와 직접 연결할 수 없습니다. 기본 GUI가 구버전 CLI 포트에 요청하면 실패/타임아웃하며 자동으로 승인 없는 연결로 낮추지 않습니다.

기존 진단·자동 테스트를 위해 GUI 실행 시 명시적으로 **`--legacy-cli`**를 지정하면 이전 수동 Host/Client 화면을 사용합니다. 이 모드는 LAN 발견/새 연결 승인을 사용하지 않으며 사용자가 수동 Host 시작으로 연결 수락을 허용하는 기존 동작입니다. 기본 GUI의 보안 모델과 다르므로 CLI 상호 운용/기존 회귀 진단에만 명시적으로 사용합니다. 진단 옵션 없이 실행한 GUI에는 이전 Host/Client 버튼을 표시하지 않고 해당 명령도 처리하지 않습니다.

Gate/discovery 버전 1은 현재 파일 프로토콜 6에 대응합니다. 신규 GUI끼리 사용하세요. 연결 동의만으로 강한 신원 인증 또는 암호화가 제공되지 않으며, 계정·클라우드·NAT traversal·인터넷 원격 연결·TLS는 추가하지 않았습니다.

## 스레드·큐·소켓 수명

LanService는 Controller 관리자 스레드에서만 접근합니다. select(0 timeout)/nonblocking send·recv·accept로 승인 단계를 처리하고, 관리자 condition_variable은 대기 작업이 없을 때 최대 100ms 기다립니다. 바쁜 대기와 UI 스레드 네트워크 I/O는 없습니다. 큐 잠금을 잡은 채 socket I/O를 수행하지 않습니다.

기존 UI 50ms drain, 명령 최대 64개, 이벤트 최대 256개를 유지합니다. 발견 snapshot은 마지막 1개로 합쳐 폭주가 연결 승인 알림을 밀어내지 않도록 했습니다. 진행률 coalescing에는 session도 포함하여 재접속의 같은 requestId를 이전 세션 슬롯에 합치지 않습니다.

승인 소켓 인계에는 예외 정리 guard를 사용하고 Network가 실패 경로까지 소유권을 소비합니다. 인계 전 socket은 LanService, 인계 후 socket은 Network만 닫습니다. 창 종료 시 관리자가 LAN 후보/UDP/listen을 먼저 정리하고 기존 전송 작업자를 RequestStop/join한 후 Network를 해제합니다. 종료 중 승인된 소켓은 인계하지 않고 닫습니다. 작업자는 HWND나 UI 컨트롤을 참조하지 않습니다.

## 포트·방화벽 및 실제 두 PC 절차

동일 LAN PC에서 앱을 실행하고 몇 초 후 목록에서 상대를 선택하세요. 상대의 IP와 표시 이름을 확인한 다음 연결 요청 → 상대 연결 승인 → 파일 보내기 → 상대 파일 승인 순서입니다.

방화벽은 관리자 승인 아래 해당 앱에 대한 **Private LAN UDP 9001 multicast 송수신** 및 **실제 listen TCP 포트**의 인바운드를 허용해야 합니다. 회사 장비는 관리자에게 규칙 승인을 요청하세요. 앱/테스트는 방화벽·SAC·Windows 보안 정책을 변경하지 않습니다. 검색이 제한되면 상대 GUI에 표시된 실제 TCP 포트와 LAN IPv4를 직접 입력하세요. 연결 승인 절차는 그대로 적용됩니다.

두 PC 검증은 **미실행**입니다. 실제 Wi-Fi multicast/IGMP, 공유기 client isolation, 방화벽 승인 UI, VPN 경로, 모니터/DPI 및 다른 PC 보안 정책은 이 환경에서 확인하지 못했습니다. localhost/같은 호스트의 가상 NIC 결과를 실제 LAN 성공이나 LAN 처리량으로 해석하지 않습니다.

## 자동 검증 결과

실행 환경: Windows x64, VS 18 Community MSBuild 18.10.1, C++20/Windows SDK, PowerShell 5.1, 대화형 데스크톱. Debug/Release 빌드 성공. 실제 프로세스·UDP 소켓·TCP 소켓·Win32 컨트롤·파일/폴더 대화상자를 사용합니다. 테스트 파일·설정·로그·수신 데이터는 tests/runs에 격리합니다.

| 구분 | Debug | Release |
|---|---:|---:|
| CLI stage1~5 | 121 통과 | 121 통과 |
| CLI stage6 Unicode | 43 통과 | 43 통과 |
| 기존 GUI 기본/고급/다운로드 | 10+8+17 통과 | 10+8+17 통과 |
| 신규 기본 GUI LAN | 25 통과 | 25 통과 |

신규 stage8_lan.ps1은 구성별 25개 PASS 사례를 검증합니다: 세션 없는 초기 준비, 자기 자신 제외, malformed/UTF-8/길이/포트/버전 거부, 출발지 IP/중복 제거, 만료, 두 프로세스 multicast 발견, 자동 연결 없음, 선택 요청/거절, 타임아웃, 승인 인계, 연결 중 중복 거부, Unicode 양방향 텍스트, Unicode 파일/별도 승인/저장 ACK/SHA, 연결 해제 후 재연결, 직접 IP 승인, 동시 요청 선출, 반복 6회 핸들 증가 <=12/메모리 증가 <16MiB, 오프라인 오류, 승인 전 데이터 차단, 잘못된 greeting 거부, 대기 요청 폭주/원래 승인 소켓만 활성화, 승인 대기 중 창 닫기, 오래된 nonce 거부, UDP 포트 사용 불가 시 직접 연결 유지, 발견 테이블 64개/메모리 <16MiB/응답성입니다.

기존 398개 검증은 유지하며 CLI 328개와 **명시적 legacy GUI 모드 70개**입니다. 신규 기본 GUI 검증과 호환 모드 회귀를 구분합니다. 이전 단계의 실제 검증/SAC 기록을 삭제하거나 이번 빌드의 통과로 소급 변경하지 않습니다.

## 재검증 명령

```powershell
msbuild FileTransferApp.slnx /m /p:Configuration=Debug /p:Platform=x64
msbuild FileTransferApp.slnx /m /p:Configuration=Release /p:Platform=x64
foreach ($cfg in @('Debug','Release')) {
  msbuild FileTransferApp/FileTransferApp.vcxproj /m /p:Configuration=$cfg /p:Platform=x64 /p:ForceImportBeforeCppTargets="$PWD/tests/Stage4Faults.props" /p:OutDir="$PWD/tests/runs/stage8-fault/$cfg/bin/" /p:IntDir="$PWD/tests/runs/stage8-fault/$cfg/obj/"
  if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/RunStage5.ps1 -Exe "x64/$cfg/FileTransferApp.exe" -FaultExe "tests/runs/stage8-fault/$cfg/bin/FileTransferApp.exe"
  if ($LASTEXITCODE -ne 0) { throw 'CLI regression failed/blocked' }
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage6_unicode.ps1 -Exe "x64/$cfg/FileTransferApp.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Unicode regression failed/blocked' }
  foreach ($test in @('stage7_gui','stage7_advanced')) {
    powershell -NoProfile -ExecutionPolicy RemoteSigned -File "tests/$test.ps1" -Exe "FileTransferApp/x64/$cfg/FileTransferAppGui.exe"
    if ($LASTEXITCODE -ne 0) { throw 'GUI legacy regression failed/blocked' }
  }
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage7_1_download.ps1 -Exe "FileTransferApp/x64/$cfg/FileTransferAppGui.exe" -Cli "x64/$cfg/FileTransferApp.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Download regression failed/blocked' }
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage8_lan.ps1 -Exe "FileTransferApp/x64/$cfg/FileTransferAppGui.exe"
  if ($LASTEXITCODE -ne 0) { throw 'LAN validation failed/blocked' }
}
```

RemoteSigned는 이전에 승인된 테스트 PowerShell 프로세스 범위에만 지정합니다. CLI TCP 9000 suite 및 신규 UDP 9001 테스트를 동일 종류끼리 병렬 실행하지 마세요. SAC/Application Control 차단 시 정확한 파일 경로·해시·CodeIntegrity 이벤트를 남기고 중단하며 정책 해제/실행 파일 변형으로 회피하지 않습니다.

## 남은 수동 검증/제한

실제 두 PC의 발견·연결 승인·양방향 대용량 파일·취소·이어받기, 실제 IME 조합·탐색기 drop, 100/125/150/200% DPI/모니터 이동의 육안 배치는 미실행입니다. 신규 경로의 대용량/취소/이어받기 전체 조합은 기존 TCP 로직 회귀로 확인했으며 모든 조합을 신규 LAN 경로에서 별도로 반복하지 않았습니다.

기기 이름/컴퓨터 이름을 LAN에 광고하며 암호화되지 않습니다. 기본 GUI의 발견 목록은 영속 신뢰 목록이 아니고 instance ID는 앱 재시작 때 바뀝니다. 파일 복구용 영속 전송 ID와 다운로드 폴더 정책은 변하지 않습니다. 검색이 실패하는 환경은 직접 IP를 사용하며 인터넷/NAT traversal은 지원하지 않습니다.

최종 결과: **448개 PASS = 기존 398개 + 신규 50개**. 필수 자동 검증의 남은 실패는 없습니다. 이번 실행에서 Application Control/SAC 차단은 발생하지 않았습니다. 과거 STAGE5/STAGE7 기록은 유지합니다.

소켓 정리 보강 후 GUI 회귀 재실행 중, 다른 GUI suite와 동시에 수행한 네이티브 폴더 대화상자 자동 조작에서 한 번 타임아웃이 발생했습니다. 해당 Debug 다운로드 17개를 단독 재실행하여 모두 통과했습니다. 병렬 GUI 자동화와의 관련성은 추정이며 GUI suite의 순차 실행을 권장합니다. 실패 실행을 통과 수에 넣지 않았습니다.

최종 검증 로그(모두 tests/runs 아래의 ignore 대상):
- Debug CLI: suite-15899f92c5754262bc9d876a28063c10, Unicode 90483407c63f4554ba4b58b80be02a43
- Release CLI: suite-b0dcbe1fffb04a018c941dbc08747ac0, Unicode 4c5ab3118d7548cb937c2293b5f401b7
- Debug GUI: gui-590854f66285477f9b4c6ef741564333, gui-advanced-42d3a264278042538ffdb76b6803496b, gui-download-2dac339d0fe24532b20480501ad36c8d
- Release GUI: gui-d8969761098e4d79908b1319bf6f324e, gui-advanced-b871e9f83fc44365a17a077db989ba74, gui-download-d38725ec3f024470ad1aeb52d21bd54d
- Debug LAN: lan-0ebe44bba38642e9a54e055c0bc47ee6
- Release LAN: lan-7abbb54a9f15497abb29d799d3042eaa

최종 실행 파일 SHA-256:
- x64/Debug/FileTransferApp.exe: 78A20436A4717C36C19534595F019DD5F1BC9F0C63A0AAE291B269C244CB1EC1
- x64/Release/FileTransferApp.exe: 01C4E64C373BDC0E9AEB9207066F0BB21DF74F3F55168C817CD187AD40152C27
- FileTransferApp/x64/Debug/FileTransferAppGui.exe: F1D74809AC3D53064F01BF1A98E312F741B93714135812EF06E5B20E6E61D4B7
- FileTransferApp/x64/Release/FileTransferAppGui.exe: AFB3964D7986E3895BB1AE8E74B4FF0A8DA6FBE0B32F37B5D6F42D3445E86E48