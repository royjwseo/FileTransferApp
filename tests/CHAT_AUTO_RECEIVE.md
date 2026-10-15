# 독립 대화 창과 자동 파일 수신

## 기준과 사용자 피드백

기준 `6241c66` (게시된 v1.0.0), 작업 브랜치 `feature/chat-auto-receive`.
사용자가 기존 버전으로 실제 2PC LAN 연결·파일 전송 성공을 확인했다고 알려 주었다.
이는 기존 버전에 대한 사용자 확인이며 이번 수정본의 실제 2PC 검증 결과는 아니다.
게시된 v1.0.0 태그·ZIP·main은 변경하지 않는다.

## 구조 분석과 변경

기존 `Window`는 메인 HWND의 transcript/input/send를 소유하고,
`Controller` 관리자·송신·수신·action 스레드가 `EventQueue`로 상태를 전달했다.
기존 파일 요청은 `Protocol::PendingApproval`에서 UI의 Accept 명령을 기다렸고,
`Network::AcceptFile`은 저장소 생성·복구 검증 뒤 `FILE_ACCEPT`/`FILE_COMPLETE`/`FILE_ERROR`를 반환했다.

### ChatWindow

- `include/ChatWindow.h`, `src/ChatWindow.cpp`에 독립적인 owned top-level Win32 창을 추가했다.
- `WS_OVERLAPPEDWINDOW`로 이동·크기 조절·최대화 가능. transcript는 읽기 전용 multiline edit,
  하단은 스크롤 가능한 multiline 입력창과 전송 버튼이다.
- Enter는 입력을 메인 창의 명령 큐로 복사한다. Shift+Enter는 기본 edit 줄바꿈을 사용한다.
  Shift를 빨리 놓아도 WM_CHAR 처리에서 잘못 제거하지 않도록 key-down 시점의 수정키 상태를 보존한다.
  IME 조합 중에는 기본 edit 처리를 유지한다. 실제 IME 조합은 수동 확인이 필요하다.
- 같은 UI 스레드에서 WM_COMMAND를 동기 전달하여 입력을 즉시 캡처한다.
  여기서 네트워크 I/O를 하지 않고 Controller 큐에 넣는다.
- 기존 action이 작업 중이라는 이유로 연속 메시지를 버리지 않도록 관리자 소유의
  최대 64개 FIFO 메시지 대기열을 추가했다. 연결 종료 시 오래된 세션의 메시지는 폐기한다.
  포화 시 작업 기록에 전송 실패를 표시한다.
- WM_CLOSE는 숨김만 수행한다. 기록·초안은 유지되고 수신 텍스트는 숨겨진 창에도 기록한다.
  메인 창 종료 때 owned 창이 파괴되며 네트워크 작업자를 정리한다.
- 크기 변경 시 이동 전/후 영역을 모아 배경·자식·테두리를 갱신한다.
  부모 WS_CLIPCHILDREN과 서로 겹치지 않는 자식 WS_CLIPSIBLINGS를 사용한다.
  이동 중 즉시 paint를 피하고 정규 메시지 루프에서 갱신한다. 별도 더블 버퍼링은 추가하지 않았다.
- 대화 창은 자체 DPI·폰트를 소유한다. WM_DPICHANGED로 독립 모니터 이동 시 재배치한다.

메인 `Gui.cpp`에는 대화 열기 버튼, 작업 기록, 연결·파일·다운로드 설정을 남겼다.
대화의 컨트롤은 메인 스크롤 레이아웃에서 이동하지 않는다.
기존 컨트롤 ID는 유지하고 OpenChat=142, ActivityLog=143, AutoInfo=144를 추가했다.
기본 GUI에는 파일별 승인/거절 버튼을 생성하지 않는다.
연결 요청 영역과 작업 기록 아래에 연결 승인 후 자동 파일 수신임을 표시한다.

### 자동 수신 준비

- `Network::PendingFileRequestId`는 protocolMutex 아래 pending 요청 ID만 읽는다.
  UI event queue와 독립적으로 Controller가 poll하므로 UI drain 지연/창 숨김 때문에 수신 준비가 빠지지 않는다.
- 기본 GUI Controller의 전용 preparation 스레드가 `AcceptFile(id, folderSnapshot)`을 호출한다.
  기존 메시지 action 스레드와 분리해 복구 해시 검증 중에도 채팅을 처리할 수 있다.
- 다운로드 폴더는 관리자 스레드에서 스냅샷을 만들고, 이미 준비 중인 요청에는 폴더 변경을 적용하지 않는다.
- 유효한 폴더가 없으면 FILE_REJECT를 보내고 원인을 작업 기록에 남긴다.
  빈 경로로 AcceptFile을 호출하여 CLI의 cwd/received로 대체되는 일을 막았다.
- 저장소 생성/복구 검증 실패는 기존 FILE_ERROR를 반환한다. 수신 측 상태에는 상세 준비 실패 사유를 표시한다.
- 종료/연결 교체 시 RequestStop → preparation 포함 작업자 join → Network 해제 순서다.
  오래된 요청의 ID/generation과 취소 여부는 기존 AcceptFile에서 다시 확인한다.
- 실제 파일 쓰기·해시·디스크 확인은 준비/수신 작업자에서 처리하며 UI 스레드에서 대기하지 않는다.

## 상태 머신 및 호환성

wire protocol **6**, UDP discovery/greeting **1**, requestId 규칙은 그대로다.

```text
연결 요청 → 사용자 연결 승인 → 기존 Network 소켓 인계
FILE_REQUEST → PendingApproval (내부 준비 상태)
  → 자동 저장소/복구 검증 성공 → FILE_ACCEPT → AwaitBegin → BEGIN/CHUNK/END
     → SHA-256 + 저장 Commit → 기존 FILE_ACK
  → 이미 완료된 복구/조회 검증 성공 → 기존 FILE_COMPLETE
  → 저장소/복구 검증 실패 → 기존 FILE_ERROR
  → 유효한 설정 없음 → 기존 FILE_REJECT
```

PendingApproval 열거형은 호환성 때문에 이름을 유지한다. 기본 GUI에서는 사람을 기다리는 상태가 아니라
자동 수신 준비 상태로 사용한다. 기존 취소·타임아웃·generation·ACK·복구 경로를 재사용한다.
파일명 안전성, 1 TiB 상한, 기존 파일/part 덮어쓰기 금지, 디렉터리·파일 핸들 고정,
복구 메타데이터 소유권·SHA-256·디스크 공간 검사도 기존 구현을 사용한다.
메타데이터 공격에 대한 기존 연결 종료 정책도 바꾸지 않았다.

구버전 GUI와 연결 형식이 같으므로 호환된다. 새 수신자는 자동 준비하고,
구버전 수신자는 기존 파일 승인 버튼을 눌러야 한다. 송신자는 같은 FILE_ACCEPT를 기다린다.
기본 GUI와 기존 CLI는 여전히 연결 greeting 차이로 직접 호환되지 않는다.
**명시적 --legacy-cli 진단 모드 및 CLI는 파일 수동 승인을 유지한다.**
이 모드에는 최초 연결 승인 게이트가 없으므로 자동 수신으로 신뢰 경계를 확대하지 않았다.

## 실행 및 자동 검사

```powershell
msbuild FileTransferApp.slnx /m /p:Configuration=Debug /p:Platform=x64
msbuild FileTransferApp.slnx /m /p:Configuration=Release /p:Platform=x64
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/chat_auto_receive.ps1 -Exe FileTransferApp/x64/Release/FileTransferAppGui.exe
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/auto_receive_wire.ps1 -Exe FileTransferApp/x64/Release/FileTransferAppGui.exe
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tools/test-release.ps1
```

전체 runner에는 기존 512개와 새 GUI 16 + wire 15를 두 구성에서 실행하는 62개를 등록했다.
총 목표는 **574개**다. CI는 실제 키보드 포커스/대화상자/픽셀 검사가 실패하면 성공으로 대체하지 않는다.

`GuiDriver`는 별도 대화 HWND를 찾아 실제 입력·버튼을 대상으로 한다.
기존 suite의 transcript 조회는 작업 기록과 대화를 합친 진단 뷰로 제공한다.
기존 LAN 파일 사례는 파일 수신 승인 클릭을 제거하고 자동 수신 완료/ACK/SHA로 검증한다.
CLI와 legacy 수동 파일 승인/거절 회귀는 유지한다.

검증일: 2026-10-15 (Asia/Seoul). Debug/Release x64 빌드 성공, 최종 빌드 오류·경고 없음.

| 기준 항목 | Debug | Release |
|---|---:|---:|
| 기존 GUI 기본/고급/다운로드 | 10+8+17 PASS | 10+8+17 PASS |
| 기존 메인 스크롤 | 32 PASS | 32 PASS |
| 신규 대화/자동 수신 UX | 16 PASS | 16 PASS |
| 신규 원시 자동 수신·복구/오류 | 15 PASS | 15 PASS |
| 기존 CLI 취소/재연결/이어받기 | 6+4+12 PASS | 6+4+12 PASS |
| 기존 Unicode (부분 실행) | 13/43 PASS | 13/43 PASS |
| 기존 CLI stage1/2/3/3_validation/4/boundaries | 포트 충돌로 미완료 | 포트 충돌로 미완료 |
| 기존 UDP/LAN suite | 외부 인스턴스 충돌로 미완료 | 외부 인스턴스 충돌로 미완료 |

기준 항목별 **266 PASS / 목표 574**, **308개 미검증·미완료**. 전체 회귀 통과 아님.
새 기능 62개는 전부 통과했다. 수정 후 최종 GUI 회귀는 별도 순차 실행해 기록했다.
최초 전체 runner 실행의 실패를 삭제하거나 이후 일부 통과로 전체 성공 처리하지 않는다.

전체 회귀 실행 시 기존 사용자 v1.0.0 앱이 실행 중이었다:
- PID 6952, `C:/Users/royjw/Desktop/FileTransferApp-v1.0.0-win-x64/FileTransferAppGui.exe`.
- `netstat -ano`에서 TCP `0.0.0.0:9000` LISTEN 확인.
- 고정 TCP 9000을 사용하는 기존 raw helper는 bind access-permission 오류로 실패했다.
- LAN suite의 기기 개수 검사는 외부 앱의 발견 응답과 충돌했다.
- 사용자 앱의 연결·파일 전송을 방해할 수 있어 정상 종료 허가를 요청했으며,
  허가 없이 종료하지 않았다. 포트 정책을 변경하거나 테스트를 성공으로 건너뛰지 않았다.
- 격리 실행 후 남은 suite를 다시 확인해야 실제 코드 회귀 유무를 판단할 수 있다.

로그:
- 초기 전체 회귀: `tests/runs/release-validation-0335042195be4fab95e5d4e59e74e4ce/results.json`.
- 실행 출력: `tests/runs/chat-auto/regression.log`.
- 최종 GUI: `tests/runs/chat-auto/{suite}-{Debug,Release}-final.log`.
- 빌드: `tests/runs/chat-auto/build-{Debug,Release}.log`.
- PNG·수신 파일·각 프로세스 로그: 각 테스트 출력에 기록한 `tests/runs/chat-auto-*`, `auto-wire-*`, `scroll-*`.

실제 키보드 Shift+Enter와 Enter 전송, 연속 12개 메시지 순서, 숨김/재열기·초안/기록 유지,
크기 조절·최대화/복원 후 전체 repaint 전후 픽셀 차이 0을 확인했다.
자동 수신의 SHA-256/ACK·역방향/동시 양방향·새 폴더 적용·기존 파일 보존·잘못된 폴더 거부,
경로 공격·용량 상한·공간 부족·취소·해시 불일치·연결 단절·정확한 offset 이어받기·완료 조회·
requestId replay·설정 검증 후 쓰기 권한 철회 시 오류 응답을 실제 GUI/소켓으로 확인했다.
쓰기 권한 시험은 해당 run이 만든 폴더에만 적용하고 finally에서 원래 ACL을 복원했다.

개발 도중 키보드 포커스 확보 및 이전 파일의 Completed 상태를 다음 파일로 오인하는
자동화 문제가 있었다. 실제 대상 창을 표시하고 포커스를 확인하며, 새 파일명을 먼저
기다리도록 고쳤다. 실패 실행은 통과 수에 중복 합산하지 않았다.
정지 후 픽셀 일치는 움직이는 동안의 잔상이나 모든 DPI 환경을 보증하지 않는다.

## 사용자 앱 정상 종료 후 추가 검증

사용자 허가를 받아 PID 6952의 경로를 확인하고 CloseMainWindow로 정상 종료했다.
강제 종료하지 않았으며 TCP 9000 리스너가 해제된 것을 확인했다.
2026-10-15 추가 Rebuild에서 Debug/Release x64는 모두 성공했다.

| 최신 Rebuild 산출물 기준 | Debug | Release |
|---|---:|---:|
| CLI/네트워크/Unicode 회귀 | 164/164 PASS | 164/164 PASS |
| 기존 GUI·LAN·스크롤 | 92/92 PASS | 92개 실행 차단 |
| 신규 대화·자동 수신 | 31/31 PASS | 31개 실행 차단 |
| 합계 | **287/287 PASS** | **164/287 PASS, 123개 실행 차단** |

최신 산출물 기준 총 **451/574 PASS**다. 앞서 통과한 Release GUI 결과는 이전 빌드의
결과이며, 이를 최신 실행 차단 결과와 합쳐 전체 회귀가 통과했다고 보고하지 않는다.
전체 runner는 실패 종료했고 새 배포 패키지나 Release를 게시하지 않았다.

Debug stage4의 최초 실행은 삭제 중인 part 파일의 속성 조회가 일시적으로 거부되어
14/19에서 중단됐다. Wait-FileState는 PermissionDenied만 기존 제한 시간 내에서
재조회하도록 수정했다. 접근 거부를 파일 부재로 간주하지 않으며 지속되면 실패한다.
stage3_validation 16개 및 stage4 19개 재실행은 모두 통과했다.

LAN 검사에는 다른 실제 PC(192.168.0.191)의 발견 응답이 남아 있었다.
테스트 시작 시 이미 발견된 외부 peer ID를 기준값으로 기록하고 시험용 기기 수에서만
제외하도록 자동화 도구를 수정했다. 새로 주입한 peer, 자기 자신 제외 검증과 전체 테이블
용량 제한 검사는 유지한다. 해당 PC를 종료하거나 제품의 발견 동작을 바꾸지 않았다.
Debug LAN 25개 재실행은 모두 통과했다. 두 수정은 테스트 관찰 코드에 한정된다.

Release GUI 시작은 Windows Application Control에 의해 차단됐다.
CodeIntegrity 3033/3077 이벤트를 읽기 전용으로 수집했다.
차단 파일 SHA-256: `A642DDEDC39CC75E0A863140287524165560C7BEB023685F71BAF788681F5FBB`.
정책 변경, 파일명 변경 또는 재빌드를 통한 차단 우회는 하지 않았다.
Release GUI의 최신 산출물 실행 검증은 정상 허용된 환경에서 추가로 필요하다.

추가 로그(기존 실패 로그 보존):
- `tests/runs/chat-auto/isolated-regression.log`
- `tests/runs/release-validation-680acd52677e462b9c7145db96229570/results.json` (최초 추가 runner 결과)
- 같은 디렉터리의 `stage3_validation-Debug-recheck.log`, `stage4-Debug-recheck.log`, `stage8_lan-Debug-recheck.log`
- 같은 디렉터리의 `GUI-ApplicationControl.json`

## 남은 실제 2PC 검증

- 이번 수정본끼리 연결 승인 → 자동 양방향 파일 전송·실제 저장 위치·중복/오류 안내 확인.
- 대화 창을 파일 전송 도중 닫기/재열기, 실제 빠른 Enter·Shift+Enter, IME 한글 조합.
- 실제 모니터 이동과 100/125/150% DPI, 최대화/복원·빠른 크기 조절·가림 후 잔상 육안 확인.
- 실제 네트워크 단절·재접속·취소·이어받기·완료 조회, 구버전 수신자의 수동 승인 호환성.
- 정지 후 픽셀 비교나 좌표 검사 통과를 모든 환경의 시각적 잔상 해결로 해석하지 않는다.

새 수정본은 아직 Release로 게시하지 않았다. 배포할 때 새 제품 버전을 정하고 기존 v1.0.0 asset/tag를 덮어쓰지 않는다.
