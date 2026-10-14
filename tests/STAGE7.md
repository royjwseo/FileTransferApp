# Stage 7 — Win32 GUI 연동

## 기준 및 결과 (2026-10-14)

기준은 feature/stage6-unicode의 5fb438e이며 feature/stage7-gui에서 작업했습니다. main b87ff47과 이전 작업 브랜치/커밋은 보존합니다. main 병합과 원격 push는 수행하지 않습니다. STAGE5의 과거 SAC 차단·미검증 기록은 변경하지 않았습니다.

Debug/Release x64의 CLI, GUI 및 별도 stage4 장애 주입 빌드가 성공했습니다. **Release CLI 기존 회귀 164개 통과**, **GUI Debug 18개 + Release 18개 통과**입니다. 합계는 서로 다른 실행 대상의 200개 실행 사례이며, 기존 Debug/Release CLI 328개 전체 재통과를 의미하지 않습니다. Debug CLI 회귀는 실행 차단으로 완료하지 못했습니다. 실제 두 PC 검증은 수행하지 않았습니다.

## 화면 및 역할

고정 크기 단일 Win32 창에 IPv4/포트(9000), 연결 대기/연결/해제/재연결, 상태·상대 IP, 텍스트 이력/입력/전송, 파일 선택·선택 크기, 송신, 수신 요청 승인·거절, 방향별 취소, 방향별 진행률, 영속 전송 ID/이어받기/완료 조회를 배치했습니다. Enter 전송, UTF-16 파일 대화상자, 단일 파일 WM_DROPFILES를 지원합니다. 드롭은 선택만 하며 명시적 보내기와 상대 승인이 필요합니다.

Network/Protocol/FileTransfer는 HWND나 UI 컨트롤을 소유하지 않습니다. 기존 스트리밍·안전 저장·Unicode 검증·SHA-256·복구 메타데이터·requestId 상태 머신과 프로토콜 6을 재사용합니다. main.cpp의 CLI 진입점과 기존 출력 형식은 유지합니다. 별도 GUI 프로젝트는 공통 코드를 같은 설정으로 컴파일하고 wWinMain에서 typed event를 이용합니다. GUI에서만 콘솔 출력을 버립니다.

변경: Events.h(typed event/queue), Gui.h/Gui.cpp/GuiMain.cpp, GUI vcxproj 및 솔루션 등록, Network/Protocol의 선택적 이벤트와 GUI용 중단 가능한 연결 설정, 수신 승인/거절의 예상 requestId 검사, 준비 해시 취소 토큰, GuiDriver.cs와 GUI 테스트 두 개. FrameRelay는 자식 프로세스에 TCP 핸들이 상속되지 않도록 명시하여 연결 종료 장애를 재현합니다.

## 스레드·수명·상태

- UI 스레드는 Win32 메시지 루프와 컨트롤만 처리합니다. 소켓 I/O, 원본 열기/크기 조회/해시/전송/수신 승인 해시는 작업 스레드에서 실행합니다. 파일 대화상자는 네이티브 모달 UI입니다.
- Controller 관리자 스레드는 최대 64개 명령과 condition_variable을 사용합니다. 연결/수신 스레드, 송신 스레드, 승인·메시지 작업 스레드를 소유합니다. 작업 중 같은 종류 요청은 거부합니다. 스레드 필드 초기화가 완료된 후 관리자 스레드를 시작합니다.
- 공통 EventQueue는 mutex로 보호하고 최대 256개 이벤트를 보관합니다. 이벤트에 session/requestId를 실어 이전 연결 이벤트·명령을 무시하고 승인/거절에도 예상 requestId를 확인합니다. 포인터 또는 동기 UI callback을 전달하지 않습니다.
- 진행률은 기존 약 500ms 제한을 유지하며 동일 방향/ID 이벤트를 합칩니다. UI는 50ms 타이머로 drain합니다. 텍스트 이벤트는 16KiB 제한, UI 한 tick 텍스트는 약 32k UTF-16, 표시 이력은 80k UTF-16 이내로 제한합니다. 초과 시 로그/텍스트/진행률을 먼저 버리고 경고합니다. 극단적인 상태 이벤트 폭주에서는 오래된 이벤트도 생략될 수 있습니다. UI 로그는 신뢰할 수 있는 영속 감사 기록이 아닙니다.
- GUI accept는 select로 250ms마다 중단을 확인하며 최대 24시간 대기합니다. connect는 2초 제한입니다. CLI 초기 accept 동작은 유지합니다. 기존 send timeout/TCP backpressure를 재사용합니다.
- 창 닫기는 종료 플래그만 설정합니다. 관리자가 RequestStop, 작업 스레드 join, Disconnect 및 Network 파괴를 수행합니다. UI는 Done 확인 후 창을 파괴합니다. 네트워크 작업자는 HWND에 접근하지 않아 종료 후 오래된 이벤트가 컨트롤에 접근하지 않습니다.
- 송신/수신 지표는 파일명, requestId, 바이트/총량, %, MiB/s, 경과 시간과 상태를 분리합니다. AwaitAck는 100%와 별도로 표시합니다. Completed는 해당 요청 ACK/검증된 완료 응답에만 발생합니다. 수신 Saved는 안전 저장 후 표시합니다. 준비 해시 취소도 지원합니다.

## 자동 검증

| 대상 | 실제 결과 |
|---|---|
| Release CLI stage1/2/3/3_validation/4 | 12+20+27+16+19 = 94 통과 |
| Release CLI stage5 cancel/reconnect/resume/boundaries | 6+4+12+5 = 27 통과 |
| Release CLI stage6 Unicode | 43 통과 (1GiB+64KiB 실제 TCP/SHA 포함) |
| Debug CLI 전체 회귀 | 첫 프로세스 시작 차단, 0개 통과; 나머지 미실행 |
| Debug GUI 기본/고급 | 10+8 통과 |
| Release GUI 기본/고급 | 10+8 통과 |

GUI 기본 10개: 창 생성/정상 종료, 중단 가능한 Host 대기, TCP 연결/상대 IP, Unicode 양방향 텍스트/Enter, 실제 파일 선택 대화상자/Unicode 파일/승인/저장 ACK/SHA, 완료 조회, 거절, 연결 해제/응답성, 재연결/연결 중 닫기, accept 대기 중 닫기.

GUI 고급 8개: 0바이트 100% 후 ACK 보류/응답성, 송신 취소, 수신 취소, 8MiB 양방향 동시 TCP/SHA, 송신 GUI 비정상 종료 후 부분 파일 재연결/ID 이어받기, 백프레셔 중 닫기, 연결·해제 8회 핸들 증가 <=8/잘못된 IPv4 오류, 1000개 UTF-8 텍스트 이벤트의 bounded 이력/메모리 증가 <32MiB/응답성.

GUI 테스트는 실제 프로세스·Win32 컨트롤·네이티브 파일 대화상자와 실제 localhost TCP를 사용합니다. 장애는 TCP relay로 ACK 보류/연결 단절 등을 주입합니다. GUI 코드 전용 테스트 우회 진입점은 없습니다. localhost 수치를 LAN 성능으로 해석하지 않습니다.

Release 회귀 로그: tests/runs/suite-1c0807f35def408ea66145965895378c 및 Unicode 642baf43adc14166bd8551f2e8105737. 로그와 모든 fixture/빌드/수신 파일은 기존 .gitignore로 제외합니다.

## Application Control 기록

2026-10-14 15:43:21 KST, powershell.exe가 stage1.ps1 TestApp 생성자(line 65)의 Process.Start로 다음 일반 CLI를 실행하다 차단됐습니다. 테스트 전용 장애 주입 바이너리가 아닙니다.

경로: C:\Users\royjw\Desktop\Visual Studio Projects\FileTransferApp\FileTransferApp\x64\Debug\FileTransferApp.exe

차단 당시 SHA-256: FC6601CB3EC508BCCD1206595E54530A482762AED2527A969EAEDBAAFEE17714

오류: An Application Control policy has blocked this file.

Microsoft-Windows-CodeIntegrity/Operational: 3077(record 212), 3118(record 214), policy GUID {0283ac0f-fff1-49ae-ada1-8a933130cad6}. XML/log: tests/runs/suite-b062619050fd4fd79f37edb55fb893d3/CodeIntegrity.xml 및 stage1.log. 이벤트는 Application Control 정책 차단을 입증하며 정책 제품명을 SAC/AppLocker로 추가 단정하지 않습니다.

이후 정상적인 GUI 연결 수명 수정으로 공통 코드를 재빌드했으나 차단된 Debug CLI를 반복 실행하지 않았습니다. 최종 Debug CLI SHA-256은 4C48A5CB1B218513D5F76A50B75A609A54D195C80B195C981B927FFD3DE01F15이며 이 해시가 차단되었다고 주장하지 않습니다. 최종 Release CLI SHA-256은 696436BF36BF5D19FC9FDE22A02E739ED980A7560ACDB4F5770EB551FFE0F78C입니다. Debug GUI/Release GUI/Release CLI는 정상 실행됐습니다. 보안 설정 변경·정책 우회·차단 회피용 바이너리 변형은 하지 않았습니다.

Debug CLI 전체 회귀 완료에는 관리자가 정책/서명 요구 사항을 확인하여 승인한 Windows 테스트 환경이 필요합니다. 위 경로·해시·이벤트를 관리자에게 제공하고 승인된 실행 절차를 받은 뒤 아래 명령을 실행하세요. 현재 결과로 전체 검증 완료 또는 main 병합을 승인할 수 없습니다.

## 재실행 명령과 요구 사항

Windows x64, Visual Studio C++20/SDK, .NET Framework PowerShell 5.1, 활성 대화형 데스크톱과 파일 대화상자 조작 가능한 세션이 필요합니다. CLI suite는 TCP 9000을 사용하므로 순차 실행합니다. GUI 자동 검증 중 마우스/키보드로 테스트 창을 조작하지 마세요. RemoteSigned는 이전에 허용된 테스트 PowerShell 프로세스 범위에만 지정합니다.

```powershell
msbuild FileTransferApp.slnx /m /p:Configuration=Debug /p:Platform=x64
msbuild FileTransferApp.slnx /m /p:Configuration=Release /p:Platform=x64
# 아래는 승인된 실행 환경에서 구성별로 순차 실행
foreach ($cfg in @('Debug','Release')) {
  msbuild FileTransferApp/FileTransferApp.vcxproj /t:Rebuild /p:Configuration=$cfg /p:Platform=x64 /p:ForceImportBeforeCppTargets="$PWD/tests/Stage4Faults.props" /p:OutDir="$PWD/tests/runs/stage7-fault/$cfg/bin/" /p:IntDir="$PWD/tests/runs/stage7-fault/$cfg/obj/"
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/RunStage5.ps1 -Exe "FileTransferApp/x64/$cfg/FileTransferApp.exe" -FaultExe "tests/runs/stage7-fault/$cfg/bin/FileTransferApp.exe"
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage6_unicode.ps1 -Exe "FileTransferApp/x64/$cfg/FileTransferApp.exe"
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage7_gui.ps1 -Exe "FileTransferApp/x64/$cfg/FileTransferAppGui.exe"
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage7_advanced.ps1 -Exe "FileTransferApp/x64/$cfg/FileTransferAppGui.exe"
}
```

실패/차단 시 즉시 중단하고 로그·해시·CodeIntegrity 이벤트를 수집하세요. 차단 파일을 이름/위치 변경하거나 보안 기능을 해제하여 재시도하지 마세요.

## 수동 검증 — 아직 미실행

1. 같은 권한 수준의 Explorer에서 한글 PDF/사진 하나를 GUI에 드롭: 선택명/크기 표시, 보내기/승인 후 SHA 확인. 두 파일 드롭은 안내 후 거부하는지 확인. 드롭 자체는 자동 테스트에서 수행하지 않았습니다.
2. Windows 한국어 IME로 메시지를 조합하여 Enter 전송/포커스/Tab 탐색 확인. 자동 테스트는 Unicode 텍스트 설정 및 Enter 메시지를 검증했으며 실제 IME 조합은 검증하지 않았습니다.
3. 100/125/150/200% DPI에서 라벨/버튼/진행률 잘림, 화면 크기/키보드 접근성을 확인. 현재 고정 크기 창이며 반응형 레이아웃은 없습니다.
4. 두 Windows LAN PC에서 동일 --version 확인, 승인된 방화벽 규칙 아래 Host/Client 연결·양방향 텍스트/Unicode 파일/동시 전송/취소/재연결/이어받기를 확인. 실제 LAN 성능/방화벽/다른 PC 정책은 미검증입니다.
5. 별도 승인 환경에서 Debug CLI 전체164를 실행하여 기존328 회귀 요건을 완료합니다. 이전 stage5/6 검증 이력은 이번 빌드의 미실행 결과를 대체하지 않습니다.

GUI는 한 번에 방향별 한 파일, 단일 파일 드롭을 지원합니다. 역할은 수동 Host/Client이며 LAN 검색/역할 자동화/계정/새 프로토콜/GUI 이력 영속화는 없습니다. GUI 재시작 후 원본 재선택과 ID 입력이 필요합니다. 새 기능 없이 기존 상태 머신을 보존했고 소켓·파일 핸들은 기존 소유권과 종료 경로로 정리합니다.

최종 GUI 실행 로그 폴더:
- Debug 기본: tests/runs/gui-73c5a07ff1af43c6814bd6922af7b8ed
- Debug 고급: tests/runs/gui-advanced-4e033234d943431090bf9e9565203b19
- Release 기본: tests/runs/gui-008ebb23b3dd4644b291ae35a25937f0
- Release 고급: tests/runs/gui-advanced-3799192d361945f4b73764cca0d50246

최종 판단: GUI 구현과 실행 가능한 localhost 자동 검증은 완료했습니다. Debug CLI 회귀와 위 수동 항목은 미검증이므로 전체 단계의 실환경 검증 완료로 간주하지 않습니다. 변경은 로컬 기능 브랜치에만 보존합니다.
