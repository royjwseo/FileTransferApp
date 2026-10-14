# 7.1단계 — 다운로드 폴더 및 GUI 사용성

기준: feature/stage7-gui `8560942`. 작업: feature/stage7-1-download-ui. main 및 기존 브랜치/커밋은 보존하며 로컬 커밋만 생성합니다. 이전 STAGE5/7의 Application Control 차단 기록은 그대로 유지합니다.

## 구조 분석과 변경 범위

기존 GUI는 UI 타이머가 typed event queue를 읽고 Controller가 Network 및 송신/수신/승인 작업 스레드를 소유했습니다. Protocol은 승인과 requestId 상태를 검증하고 TransferStorage는 `received` 디렉터리를 핸들로 고정하여 임시 파일과 DPAPI 소유권 메타데이터를 관리했습니다. 파일명은 UTF-8 검증 후 UTF-16 경로로 결합합니다.

이 구조를 유지하고 아래 부분만 확장했습니다.

- `GuiSettings.h`: Windows Known Folder, UTF-8 설정 파일 읽기/저장.
- `Gui.cpp`: 다운로드 폴더 이벤트/명령, 폴더 선택 대화상자, 그룹·가변 배치·DPI·스크롤·버튼 상태.
- `Network::AcceptFile`: 선택적 수신 폴더를 승인 작업에 전달. CLI는 빈 인자로 기존 동작 유지.
- `TransferStorage`: 명시적 폴더와 쓰기 가능 여부 검사, 새 GUI 메타데이터의 디렉터리 ID 결합.
- `Events.h`: 다운로드 설정 전용 typed event. 기존 256개 이벤트/64개 명령 제한 유지.
- `GuiDriver.cs`, `stage7_1_download.ps1`: 설정을 테스트 작업 폴더에 격리하고 기존 GUI 회귀 및 신규 검증.

프로토콜 형식과 버전(6), 취소·재접속·SHA-256·ACK 상태 머신은 변경하지 않았습니다. GUI와 CLI 실행 파일은 계속 분리합니다.

## 사용자 동작과 설정 위치

파일 전송 그룹의 현재 다운로드 경로 옆 **폴더 변경**을 누르면 Windows 기본 `IFileOpenDialog` 폴더 선택 창을 엽니다. 한글·공백·일반 Unicode 경로를 UTF-16으로 전달합니다. 취소하면 설정을 바꾸지 않습니다.

처음 설정 파일이 없으면 `SHGetKnownFolderPath(FOLDERID_Downloads)`로 사용자의 실제 Downloads 폴더를 선택합니다. 폴더는 존재해야 하고 leaf reparse point가 아니며 파일 생성 권한이 있어야 합니다. 접근 불가·잘못된 설정은 메시지 이력에 오류와 함께 표시하고 수신 승인을 비활성화합니다. 사용자는 다른 유효 폴더를 선택하여 복구할 수 있습니다. 임의 `received` 폴더로 자동 대체하지 않습니다.

설정 파일은 `%LOCALAPPDATA%\FileTransferApp\download-folder.txt`이며 UTF-8 전체 절대경로 한 개를 저장합니다(줄바꿈 없이, UTF-8 BOM 읽기 허용). 폴더 변경 시 검증 후 별도 임시 설정 파일에 기록/flush하고 rename으로 교체합니다. 저장에 실패하면 기존 설정을 유지합니다. 최초 기본값은 Windows Known Folder를 사용하며 명시적 변경 전까지 별도 설정 파일 생성이 필요하지 않습니다.

진단·휴대 실행에서는 `FILETRANSFER_GUI_SETTINGS` 환경 변수로 **절대경로**의 설정 파일 위치를 지정할 수 있습니다. 데이터 폴더를 우회하는 옵션이 아니며 동일 검증을 적용합니다. 자동 테스트는 이 옵션으로 작업 폴더마다 설정과 수신 파일을 격리하므로 실제 사용자 설정이나 Downloads에 수신 fixture를 남기지 않습니다. 기본 Downloads 검증 때 생성하는 작은 쓰기 probe는 핸들 종료 시 즉시 삭제됩니다.

이번 단계는 공통 기본 폴더를 우선합니다. 수신 요청별 별도 '다른 폴더로 저장' 버튼은 구현하지 않았습니다. 승인 전 기본 폴더를 바꾸면 해당 승인에 적용되며 승인 후 변경은 다음 승인부터 적용됩니다. 현재 진행 중인 파일의 목적지를 이동하는 기능은 없습니다.

CLI 수신은 기존대로 현재 작업 디렉터리의 `received/`입니다. GUI 기본값은 이제 Downloads이므로 서로 다릅니다.

## 안전 저장과 복구 정책

폴더 검증·쓰기 probe·설정 I/O는 Controller 관리자 작업 스레드에서 처리합니다. UI 스레드는 네이티브 대화상자와 컨트롤만 다룹니다. 관리자 스레드가 수신 승인 명령을 처리할 때 폴더 경로를 복사하여 승인 스레드에 전달합니다. 이후 설정 변경과 파일 작업은 같은 경로 변수를 공유하지 않습니다.

GUI fresh 승인은 선택한 폴더에서 TransferStorage를 미리 준비하여 디렉터리/임시 파일 핸들을 확보합니다. 승인 준비 중 요청이 만료·취소·교체되면 requestId와 generation을 재검사하고, 해당 준비 작업이 소유한 새 임시 파일만 정리합니다. 복구 작업에서 기존 부분 파일은 보존합니다.

기존 파일명 검증(경로 탐색/예약 이름/잘못된 UTF-8/제어 문자/길이), CREATE_NEW, 교체 금지 rename, reparse point와 다중 hard link 거부, 핸들 기반 임시 파일 삭제를 유지합니다. SHA-256 검증 → flush → 최종 이름 반영 → ACK 순서를 유지합니다. GUI의 100%와 저장 ACK 기반 성공은 계속 구분합니다.

새 GUI fresh 전송의 DPAPI 봉인 내용에는 기존 transferId + 데이터 파일 ID 외에 **수신 디렉터리의 volume/file ID**를 추가합니다(평문 80→96바이트, 로컬 메타데이터만 변경). 복구 때 같은 디렉터리인지 확인하므로 데이터와 메타데이터를 함께 다른 디렉터리로 이동해도 거부합니다. 실제 폴더 객체가 유지되는 이름 변경은 ID가 유지되지만 사용자는 새 위치를 명시적으로 선택해야 합니다. 폴더를 새로 만들거나 다른 폴더로 파일만 옮겨 복구하는 방식은 지원하지 않습니다.

CLI fresh 메타데이터는 기존 80바이트 형식을 유지합니다. 이전 단계의 80바이트 메타데이터도 읽으며, 이 형식은 과거 디렉터리 ID가 없어 기존 파일 ID/SHA 검증과 사용자가 명시한 폴더만 사용합니다. 과거 기록에 없던 최초 폴더를 소급 추측하지 않습니다. 새 96바이트 GUI 메타데이터는 이전 버전 프로그램에서 읽을 수 없으며 안전하게 거부될 수 있습니다. 네트워크 상호 운용 형식에는 영향이 없습니다.

이어받기/완료 조회는 **현재 선택된 폴더 안에서만** 조회합니다. 다른 폴더를 자동 검색하거나 원본 부분 파일을 이동하지 않습니다. 원래 수신 폴더로 설정을 복원하고 같은 원본과 영속 ID를 사용해야 합니다. 다른 폴더에서 실패한 복구가 원래 부분 파일을 지우지 않는 것을 자동 검증했습니다.

## 레이아웃·스레드·수명

연결 / 메시지 / 파일 전송 / 진행 상태의 네 그룹, Segoe UI 글꼴, 간격·버튼 정렬을 적용했습니다. 가로 확장 시 메시지·파일 경로·진행률 영역을 확장하고 세로 확장 시 메시지 이력을 늘립니다. 최소 창 크기는 DPI 적용 논리 크기 850×450입니다. 작은 높이에서는 세로 스크롤로 아래 작업에 접근할 수 있습니다. 초기 창은 화면 작업 영역의 높이 이내에서 생성합니다.

Per-monitor V2 DPI 인식, WM_DPICHANGED에 따른 글꼴·컨트롤 재배치와 권장 창 크기 반영을 구현했습니다. 실제 여러 배율/모니터에서 시각적으로 잘리지 않는지는 아래 수동 검증 대상입니다. 아주 좁은 화면은 최소 너비보다 작아질 수 없으므로 충분한 화면 너비가 필요합니다.

연결 대기/접속 중에는 새 Host/Connect를 비활성화하고 해제를 허용합니다. 연결 후 메시지·파일 작업을 허용하며 파일 선택과 송신 작업 상태에 따라 보내기/복구/조회 버튼을 제어합니다. 수신 승인/거절은 요청 대기 중, 취소는 해당 방향 활성 상태에서 사용합니다. 백엔드의 session/requestId 검사는 버튼 상태와 별도로 유지합니다.

네트워크 작업자는 HWND를 참조하지 않습니다. UI 타이머가 큐를 읽고, 닫기는 기존 관리자 종료 → RequestStop → 작업자 join → Network 정리 → Done 확인 후 창 파괴 순서를 유지합니다. 폴더 변경 중 전송 스레드가 사용하는 경로/핸들은 변경하지 않습니다. 원격/느린 파일 시스템의 작업 지연은 Windows I/O 특성에 영향을 받으므로 사용 가능한 로컬 폴더를 권장합니다.

## 실행 가능한 자동 검증

실행 환경: Windows x64, Visual Studio 18 Community MSBuild 18.10.1/C++20/Windows SDK, Windows PowerShell 5.1, 활성 대화형 데스크톱. 실제 프로세스·Win32 컨트롤·Windows 파일/폴더 대화상자·localhost TCP를 사용했습니다. 실제 LAN/두 PC 결과가 아닙니다.

| 대상 | 결과 |
|---|---|
| Debug CLI 기존 stage1~5 | 121 통과 |
| Debug CLI stage6 Unicode | 43 통과 |
| Release CLI 기존 stage1~5 | 121 통과 |
| Release CLI stage6 Unicode | 43 통과 |
| Debug GUI 기존 기본/고급 | 10+8 통과 |
| Release GUI 기존 기본/고급 | 10+8 통과 |
| Debug 신규 다운로드/UI | 17 통과 |
| Release 신규 다운로드/UI | 17 통과 |

신규 17개는 Windows Downloads 일치, 크기 변경/최소 배치/연결 전 버튼, 네이티브 폴더 선택과 Unicode, 취소, 재실행 유지, 없는 경로/상대경로/일반 파일/쓰기 거부의 4가지 오류, 연결 후 버튼, 진행 중 설정 변경, 다른 폴더의 완료 조회 거부, 데이터/메타데이터 이동 시 디렉터리 ID 거부, 원래 폴더 복구, 덮어쓰기 방지, 실제 부분 파일의 폴더 변경 후 이어받기, GUI↔CLI 양방향 텍스트/파일/SHA 및 CLI received 유지입니다.

쓰기 거부는 테스트 소유 폴더에만 CreateFiles deny ACL을 임시 지정하고 finally에서 원래 ACL을 복원했습니다. Windows 보안 정책/SAC/방화벽 설정은 변경하지 않았습니다.

초기 테스트에서 helper가 GUI 실행 경로 변수를 덮어쓰는 문제와 GUI HWND 생성 직후 자식 컨트롤 초기화 전 접근하는 테스트 경합을 발견해 수정했습니다. 최종 helper는 자식 컨트롤 생성과 응답을 확인합니다. 중간 실패를 통과로 간주하지 않았고 수정 후 관련 테스트를 재실행했습니다.

이번 테스트에서 기존 Debug CLI의 Application Control 차단은 재현되지 않았습니다. 과거 차단 경로·해시·이벤트와 당시 미검증 상태는 STAGE5/STAGE7에 유지합니다. 현재 빌드 통과가 과거 차단 기록을 무효화하지 않습니다.

## 재실행

```powershell
msbuild FileTransferApp.slnx /m /p:Configuration=Debug /p:Platform=x64
msbuild FileTransferApp.slnx /m /p:Configuration=Release /p:Platform=x64
foreach ($cfg in @('Debug','Release')) {
  msbuild FileTransferApp/FileTransferApp.vcxproj /m /p:Configuration=$cfg /p:Platform=x64 /p:ForceImportBeforeCppTargets="$PWD/tests/Stage4Faults.props" /p:OutDir="$PWD/tests/runs/stage7-1-fault/$cfg/bin/" /p:IntDir="$PWD/tests/runs/stage7-1-fault/$cfg/obj/"
  if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/RunStage5.ps1 -Exe "x64/$cfg/FileTransferApp.exe" -FaultExe "tests/runs/stage7-1-fault/$cfg/bin/FileTransferApp.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Regression failed or blocked' }
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage6_unicode.ps1 -Exe "x64/$cfg/FileTransferApp.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Unicode regression failed or blocked' }
  foreach ($test in @('stage7_gui','stage7_advanced')) {
    powershell -NoProfile -ExecutionPolicy RemoteSigned -File "tests/$test.ps1" -Exe "FileTransferApp/x64/$cfg/FileTransferAppGui.exe"
    if ($LASTEXITCODE -ne 0) { throw 'GUI regression failed or blocked' }
  }
  powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage7_1_download.ps1 -Exe "FileTransferApp/x64/$cfg/FileTransferAppGui.exe" -Cli "x64/$cfg/FileTransferApp.exe"
  if ($LASTEXITCODE -ne 0) { throw 'Download GUI test failed or blocked' }
}
```

CLI suite는 port 9000를 사용하므로 병렬 실행하지 않습니다. RemoteSigned는 이전에 승인된 테스트 프로세스 범위에만 지정합니다. GUI 테스트 중 파일/폴더 창을 수동 조작하지 마세요. 실행 차단 시 해당 파일·해시·CodeIntegrity 이벤트를 기록하고 중단하며 승인된 실행 환경에서 재검증합니다. 바이너리 이동/변형이나 정책 변경으로 차단을 피하지 않습니다.

## 미실행 수동 검증 및 제한

- 실제 두 PC LAN, 방화벽/다른 PC 정책, LAN 처리량.
- 탐색기 드래그 앤 드롭과 실제 한국어 IME 조합(기존 자동 텍스트/파일 선택·전송 검증과 구분).
- 100/125/150/200% DPI 및 모니터 이동 시 한글 버튼·긴 파일명·진행률 시각 확인. 작은 높이에서 스크롤 끝의 복구/조회 버튼 접근과 키보드 포커스도 확인하세요.
- 실제 사용자 `%LOCALAPPDATA%` 설정으로 폴더 변경 후 GUI 재실행(자동 테스트는 같은 저장 코드에 격리된 설정 파일 위치를 사용).
- 네트워크 공유/매우 긴 경로/이동식 드라이브 분리 등은 미검증입니다. reparse point인 Downloads는 기존 안전 규칙상 거부되므로 실제 디렉터리를 선택해야 합니다.
- 수신 요청별 별도 저장 대화상자와 자동 복구 폴더 검색은 포함하지 않습니다. 기본 폴더를 승인 전에 바꾸는 방식을 사용합니다.

테스트 fixture·설정·수신 파일·로그·빌드 산출물은 tests/runs 또는 기존 ignore 대상이며 커밋에 포함하지 않습니다. .gitignore는 변경하지 않습니다.

최종 합계: **398개 통과 = 기존 CLI 328 + 기존 GUI 36 + 신규 34**. Debug/Release x64 GUI 및 CLI 빌드 성공, 실행 가능한 필수 자동 검증의 남은 실패는 없습니다. 실제 두 PC·위 수동 항목은 미실행입니다.

최종 로그 위치(모두 ignore 대상):
- Debug CLI stage1~5: tests/runs/suite-29db8245b3494c83af79b16dc3d3a503
- Debug CLI Unicode: tests/runs/73320cded6264bfe889b96393231dff6
- Release CLI stage1~5: tests/runs/suite-f103352969584b9ea065c8e0568af511
- Release CLI Unicode: tests/runs/bc233a296f0343cfb7b5f07f066f2deb
- Debug GUI 기본/고급: tests/runs/gui-cb7dddde43d74711a72e4cc57081152a, gui-advanced-23a8461e355141b0a5120cd3f3541325
- Release GUI 기본/고급: tests/runs/gui-9b509ac85032413a8fd329921bdd5b20, gui-advanced-c960b658b2434974a2ecbf8403a33a55
- Debug/Release 신규: tests/runs/gui-download-62e4ba5e6efa462a845b0ea03ced8e80, gui-download-bc1f793d38d84b968301d066638ec717

검증한 최종 실행 파일 SHA-256:
- x64/Debug/FileTransferApp.exe: E80FC16047432DAB76EF8CD61D804CA4047CB97D190ABC9A6E1EAB5B5BC4D496
- x64/Release/FileTransferApp.exe: C916F1832D494073C641DCF549252E79E97E4499EBC093A0ECEEE34B499BA6A9
- FileTransferApp/x64/Debug/FileTransferAppGui.exe: FEE61F85E1352C1163B5C0EA33A3C2D5B3801EF3B47512D1CF7446284E5F35B6
- FileTransferApp/x64/Release/FileTransferAppGui.exe: 98E655F2AD4DEECF42A38642736330C3CE1AC55A13FFAF64C65EEE70EE1C5C7F