# N:1 세션 및 클라이언트별 저장 경로

## 작업 기준과 분석

기준 브랜치: `feature/chat-auto-receive`.
시작 커밋: `667d0572000a6fe8e1e8d3deedb8dc317379ce85` (v1.1.0).
작업 브랜치: `feature/n-to-1-multiclient`.
기존 main, 안정 feature 브랜치, v1.0.0/v1.1.0 태그·Release·asset은 변경하지 않는다.
N:1 변경은 미배포 개발 기능이며 이 브랜치를 v1.1.0 패키지로 덮어쓰면 안 된다.

기존 구조에서 Network는 소켓·sendMutex·Protocol·requestId 상태를 연결 하나에 소유한다.
Controller는 해당 Network와 receive/send/action/preparation 작업자 및 64개 텍스트 FIFO를
소유한다. 따라서 Network/Protocol을 전역 다중화하지 않고 Controller를 연결별로 재사용한다.
LanService의 기존 단일 inbound/outbound/approved는 1:1 호환 모드에 유지하고,
N:1 모드에는 제한된 후보 목록·별도 승인 토큰·승인 socket handoff를 추가한다.
ChatWindow의 렌더링·IME·Enter/Shift+Enter·숨김 기능은 재사용한다.

## 구조 및 소유권

- `PeerSettings.h`: 영속 PeerId, 역할, 표시 이름, PeerId별 키·폴더 설정.
- `MultiLanService.cpp` / `LanService.h`: UDP v2 역할 발견 및 C3/H3/P3/A3 승인·인증.
- `MultiController.h`: listener poll, 최대 10개 세션, 명령/이벤트 라우팅, 개별 종료.
- `Gui.cpp`의 기존 Controller: 세션마다 별도 객체로 재사용. 독립 Network/Protocol과
  작업자·송신 큐·다운로드 폴더 snapshot을 소유한다. 기존 CLI/legacy Controller 경로도 유지한다.
- `Events.h`: 새 연결/종료/프로필 DTO. 각 연결은 고유 SessionId로 이벤트를 구분한다.
  PeerId는 DTO에 기록하며 Network의 기존 SessionId 이벤트를 manager에서 보완한다.
- `Gui.cpp` / `ChatWindow`: 연결 목록, 선택 대상의 기존 파일 UI, 클라이언트별 대화 창과 기록.
  HWND는 UI 스레드만 소유하고 세션 작업자는 UI 포인터를 소유하지 않는다.
- `TransferStorage` / `Protocol`: 기존 파일 프로토콜은 유지하고 수신 spec에 로컬 peerScope만 추가한다.

LanService는 승인 socket을 ready queue에 넘기고 MultiController가 이를 세션 Controller에 넘긴다.
중복 PeerId/연결 제한은 handoff 전에 socket guard로 닫는다. Network는 승인 socket을 소비한다.
종료는 RequestStop → 작업자 join → Network Disconnect/close 순서다.
닫는 동안 GUI는 timer로 완료를 기다리며 UI 스레드에서 join/I/O를 수행하지 않는다.
개별 세션 종료는 다른 세션의 소켓·프로토콜·큐를 변경하지 않는다.
한 세션의 네트워크 송신 직렬화는 기존 sendMutex를 그대로 사용한다.

상한: 서버 active+pending+ready 최대 10, 클라이언트 active 최대 1,
발견 테이블 64, manager 명령 큐 128, 세션 명령/텍스트 FIFO 각각 64,
세션 이벤트 큐 256, GUI 통합 이벤트 큐 4096, 대화 이력 최대 32개 PeerId.
32개 초과 시 오래된 비활성 대화 창을 정리하고 안내한다. 대화 기록/입력 길이 제한은 기존과 같다.
정상 메시지는 각 세션 FIFO 순서로 처리하며 큐 포화/생략은 안내한다.
장치·이벤트·버퍼를 무제한 보관하지 않으며 파일은 고정 크기 버퍼로 스트리밍한다.

## 실행 및 GUI

1. 같은 `FileTransferAppGui.exe`를 인자 없이 실행한다. 최초 기본 역할은 클라이언트다.
2. 서버 PC에서 상단 콤보를 **서버 모드**로 고르고 표시 이름과 함께 **역할/이름 적용**한다.
   IP/TCP 포트는 LAN 상태에, 영속 PeerId는 상단에 표시한다.
3. 클라이언트 PC는 **클라이언트 모드**에서 발견된 서버를 선택해 요청하거나 직접 IPv4/포트를 입력한다.
4. 미등록 상대는 서버가 이름/IP/PeerId와 함께 표시된 요청을 승인한다.
   복수 요청은 도착 토큰별로 대기하며 가장 오래된 요청부터 승인/거절한다.
5. 서버의 연결 목록에서 상대를 선택한다. 이름·연결/전송 상태·PeerId·현재 수신 폴더가 표시된다.
6. 기존 파일 선택·송신·취소·이어받기·완료 조회는 현재 선택한 세션에 적용한다.
   클라이언트는 연결된 서버 하나만 대상으로 한다. 서버는 선택한 상대만 개별 연결 해제한다.
7. 대화 창은 각 상대와의 기록만 표시한다. 다른 상대가 메인 창에서 선택돼도 대화 창의
   Enter/버튼은 그 창의 세션으로 보낸다. 숨김은 연결을 종료하지 않으며 미확인 메시지를 표시한다.

역할/이름은 재시작 후 복원한다. 이름은 UTF-8 1~96바이트, 공백만인 이름·구분자·제어/방향 제어
문자를 거부한다. 중복 이름은 허용하며 목록에서 PeerId 앞 8자리로 구분하고 상세에 전체 ID를 표시한다.
연결 중 역할/이름을 적용하면 확인 창을 띄우고 승인 시 모든 관련 세션을 정리한다.
따라서 이름 동기화는 다음 발견/핸드셰이크부터 반영되며 기존 연결에 실시간 갱신 메시지를 보내지 않는다.
취소하면 기존 세션을 유지한다. 부분 파일·복구 기록은 종료만으로 삭제하지 않는다.

실행별 InstanceId는 발견/선택 pinning에 사용하고, 영속 PeerId는 설정/저장 권한의 인덱스다.
새 연결마다 증가하는 SessionId와 별도 Chat HWND를 부여한다. 재접속 시 이력/초안을 복사하되
이전 HWND를 폐기해 오래된 창 명령이 새 세션으로 전송되지 않도록 한다.
이전 SessionId 이벤트는 새 세션에 반영하지 않는다.
동일 설치 설정 파일을 공유한 여러 프로세스는 같은 PeerId다. 동일 PeerId의 중복 활성 연결은 거부한다.
별도 설치/설정 파일을 쓰는 프로세스는 같은 IP에서도 서로 다른 PeerId/키로 연결할 수 있다.

## 저장 경로

기존 `%LOCALAPPDATA%/FileTransferApp/download-folder.txt`는 기본 다운로드 폴더 설정 그대로다.
서버의 새 상대 기본 경로는 `<기본 다운로드 폴더>/peers/<32자리 PeerId>`다.
DisplayName/상대가 보낸 파일명으로 디렉터리를 생성하지 않는다.
기본 하위 폴더 생성 전 기본 폴더와 중간 `peers` 폴더를 검증하고 reparse point를 거부한다.
사용자가 지정한 폴더는 기존 TransferStorage 검증을 재사용한다.

서버에서 상대를 선택하고 **상대 폴더 변경**으로 개별 절대경로를 지정한다.
**기본 경로 복원**은 해당 PeerId override만 제거하고 안전한 기본 하위 폴더로 돌아간다.
설정은 이름 변경·재접속·프로그램 재시작 이후에도 유지한다.
물리적 경로 검증은 해당 세션 작업자에서 수행하므로 느린 경로가 다른 클라이언트 라우팅을 막지 않는다.
구조상 올바른 설정이라도 폴더가 사라지거나 권한이 바뀌면 자동 준비를 거부하고 오류를 표시한다.
다른 폴더/`received`로 조용히 fallback하지 않는다.

자동 수신 준비가 시작될 때 해당 세션의 폴더를 복사한다. 준비 ACK 전후에 GUI 경로를 바꿔도
진행 중인 전송은 이전 snapshot에 저장하고 다음 요청부터 새 경로를 사용한다.
기본 폴더를 바꾸면 override가 없는 서버 상대에게만 새 기본 경로를 적용한다.
클라이언트 모드는 자신의 기존 다운로드 폴더를 그대로 쓰며 서버의 per-peer 정책을 적용하지 않는다.

기본 경로는 상대별로 분리되므로 같은 이름의 파일이 서로 충돌하지 않는다.
사용자가 여러 상대에게 같은 실제 폴더를 지정한 경우 기존 CREATE_NEW/no-replace 정책에 따라
중복 이름은 오류로 거부한다. 자동 덮어쓰기/임의 이름 변경으로 다른 파일을 손상시키지 않는다.
파일명/크기 상한·디스크 공간·SHA-256·취소·저장 ACK는 기존 계층을 사용한다.

## 설정 저장과 기존 등록 키

기존 다운로드 설정 파일에 접미사를 붙인다:

- `.peer-id`: 설치별 128비트 난수 PeerId. CREATE_NEW 후 flush. 비밀 키가 아니므로 평문 식별자다.
  손상되면 오류를 내며 새 ID로 자동 덮어쓰지 않는다.
- `.multiclient`: DPAPI 사용자 범위 보호 역할/이름·PeerId별 경로/키.
  크기·레코드·UTF-8·ID·경로 형식을 검사하고 임시 CREATE_NEW→flush→원자적 교체로 저장한다.
- 기존 `.trusted`: 기존 IP/공유 키 기록 그대로 유지한다.

`.multiclient`가 손상되면 원본을 보존하고 오류를 표시한다. 키/경로를 부분적으로 신뢰하지 않으며,
서버 기본 경로로 조용히 대체하지 않는다. 손상 파일의 별도 백업·복구가 필요하며 자동 삭제/재생성하지 않는다.
경로/등록은 각각 256개, 구형 IP 키 소비/해제 표시는 1024개, 설정 파일은 1MiB로 제한한다.

N:1 키는 PeerId별로 저장한다. 기존 IP 키가 있고 아직 해당 IP를 새 PeerId에 연결하지 않았다면
전체 상호 인증 성공 후 기존 키를 새 PeerId 기록에 복사한다. 기존 `.trusted`는 삭제/수정하지 않는다.
한 IP의 기존 키를 여러 다른 PeerId에 무조건 자동 적용하지 않는다. 추가 프로세스는 별도로 등록한다.
기존 키가 새 PeerId에 연결됐거나 N:1에서 해제된 IP는 소비 표시를 보호 설정에 보존한다.
이후 등록을 해제하고 재시작해도 남겨 둔 .trusted에서 같은 키를 조용히 다시 가져오지 않는다.
등록 변경/해제는 해당 PeerId의 대기 후보만 취소한다. 이미 연결된 세션은 별도 연결 해제로 끝낸다.
N:1 해제는 명시적 v1 호환 모드의 원본 .trusted를 변경하지 않는다. 그 모드의 키 해제는 해당 모드에서 수행한다.
양쪽에서 상대를 선택하고 같은 64자리 키로 등록하는 GUI 흐름을 유지한다.
이미 등록된 PeerId는 HMAC 인증 없이 수동 승인 방식으로 내려갈 수 없다.
등록된 PeerId의 IP 변경은 발견 목록의 PeerId/키로 인증 가능하다.
직접 IP 입력은 해당 주소에 등록된 상대가 하나일 때만 키를 고른다. 여러 상대면 목록에서 선택해야 한다.
IP 변경 후 직접 연결하려면 발견 목록을 쓰거나 등록의 주소를 갱신한다.

## 핸드셰이크/프로토콜 호환

새 발견: `FTA-DISC|2|Q/R|InstanceId|port|name|S/C|PeerId`.
클라이언트는 서버 역할만, 서버는 클라이언트 역할만 발견 목록에 표시한다.
새 요청: `FTA-C3|InstanceId|clientNonce|name|PeerId|C|K/M`.
K는 등록 키 인증, M은 수동 연결 승인이다. 서버만 요청을 수락한다.
H3 응답의 body는 `serverInstanceId|serverNonce|serverName|serverPeerId|S`다.
transcript는 C3 전체 + `|` + H3 body다. server/client/accepted 방향별 접두사를 넣은
HMAC-SHA256을 H3/P3/A3로 교환하며 모든 이름·역할·ID·난수를 인증 범위에 포함한다.
수신자는 새 난수를 생성해 이전 proof 재사용을 거부한다. 키는 전송하지 않는다.
후보 하나의 실패는 그 socket만 닫고 다른 세션을 유지한다.

PeerId/이름만으로 자동 승인하지 않는다. 미등록 M 요청의 PeerId는 **미인증 주장**이며
초기 연결 승인 UI에 이를 표시한다. 수동 승인은 그 PeerId의 파일/복구 공간 접근까지 허용한다.
동일 ID를 주장하는 미확인 요청은 승인하지 않아야 한다. 원래 상대를 인증해야 하는 환경에서는
각 PC를 개별 공유 키로 등록해야 한다. 공유 키 인증도 데이터 프레임 암호화/TLS를 제공하지 않는다.

파일 protocol v6의 FILE_REQUEST/FILE_ACCEPT/BEGIN/CHUNK/END·requestId·transferId·ACK는 변경하지 않는다.
기본 N:1은 구형 discovery v1/C1/C2를 자동 수락하지 않는다.
이전 GUI와 통신하려면 양쪽 호환 상대에 맞춰 `FileTransferAppGui.exe --compat-v1`을 명시적으로 사용한다.
이는 v1.1.0의 1:1 LAN/공유 키/자동 수신 화면이다. CLI 및 `--legacy-cli`는 기존대로 별도 수동 파일 승인이다.
기존 GUI suite는 GuiDriver의 명시적 `--compat-v1` 경로로 유지하고, 새 N:1 suite는 인자 없는 기본 GUI를 검사한다.

## 복구 메타데이터

`.fta`는 기존 `%LOCALAPPDATA%/FileTransferApp/transfer-metadata`, `.part`는 수신 폴더에 유지한다.
N:1의 중앙 파일명은 SHA-256(directory identity + transferId + peerScope)이다.
DPAPI 보호 payload는 기존 96바이트 소유권 정보 + 32바이트 PeerId다.
다른 PeerId는 같은 폴더·transferId를 알고 기록을 복사해도 owner 검증에 실패한다.
복구 시 원래 수신 폴더/원본/전송 ID/PeerId가 모두 필요하다.
경로 변경 후 복구하려면 해당 상대의 수신 폴더를 이전 경로로 돌려야 한다.
메타데이터를 새 폴더에 자동 이동하거나 전체 디스크에서 임의 탐색하지 않는다.

기존 1:1 기록에는 원래 PeerId 정보가 없으므로 N:1 상대에게 조용히 귀속시키지 않는다.
이전 전송은 원래 폴더에서 명시적 `--compat-v1` 또는 기존 CLI/legacy 방식으로 복구한다.
이 경로는 기존 80/96바이트 기록과 파일 옆 `.fta`의 검증 후 중앙 이동을 그대로 지원한다.
N:1은 scope 없는 이전 기록으로 자동 fallback하지 않는다. 이전 기록/데이터는 삭제하지 않는다.

## 자동 검증 기록

신규 목표: 구성별 74개, 기존 612개를 포함한 전체 runner 목표 760개.

| suite | 목표/구성 | 주요 범위 |
|---|---:|---|
| n_to_1 | 18 | 3개 연결, 대화 격리, 동시 파일, 중복 이름/파일명, 폴더 snapshot/재시작 |
| n1_auth | 14 | 독립 키, replay/변조, 구버전 거부, IP 변경, stale HWND, 승인 토큰, 10개 제한 |
| n1_storage | 11 | 부분 복구/완료, PeerId owner, 경로 오류, 공간 부족, 취소, 역할 전환, 설정 손상 |
| n1_settings | 10 | 기존 IP 키 이동·보존·해제 지속, 1:1 폴더 호환, Unicode 이름, ID 손상 |
| n1_lifecycle | 5 | 비선택 대화 routing, 숨김, Enter, 6회 재접속, 60초 이상 통신 |
| n1_gui | 16 | 서버/클라이언트 신규 컨트롤 좌표, 휠/끝 위치, resize/restore, 정지 후 픽셀 비교 |

초기 Debug 통합 실행에서 3개 연결·양방향 대화/격리·동시 파일 시작·C 송신/대화까지 8개 PASS였다.
이후 의도적 relay pause가 기존 소켓 timeout을 넘어 전송 완료 검사가 실패했다.
네트워크 timeout을 바꾸지 않고 relay를 지연 스트리밍 방식으로 보정했다.
그 다음 Debug 빌드는 Application Control에 차단됐다. 정책 변경/우회는 하지 않았다.
이 숫자는 개발 중 빌드 기록이며 최종 커밋의 통과 수에 합산하지 않는다.
새 테스트 함수 이름의 PowerShell Select 별칭 충돌도 고쳤으며 실패 run 자료를 보존한다.

추가로 확인·수정한 실제 문제:
- 선택 세션의 기존 필터가 세션 없는 manager 로그를 버려 등록 완료/이름 오류를 표시하지 않았다.
  전역 로그를 세션 필터 앞에서 처리하도록 수정했다.
- 구형 IP 키를 보존한 상태에서 N:1 등록을 해제하면 fallback으로 다시 등록될 수 있었다.
  영속 소비/해제 표시와 대상 후보 취소를 추가하고 재시작 후 수동 승인 검사를 추가했다.
- 기존 1:1 호환 모드의 ReceiveLoop가 disconnect를 보고한 직후 worker의 이전 busy snapshot이
  남아 빠른 동시 재요청을 거부했다. Request 직전에 실제 worker 상태로 gate를 nonblocking poll한다.
- 시험 도구의 PowerShell 예약 Error 변수 및 빈 취소 ACK 오류 메시지 처리도 수정했다.
- 실제 키보드 검사는 활성화 경쟁 시 정상 Win32 활성화를 2초 안에서 재시도하며,
  실제 전경 창 확인 없이 키를 주입하거나 실패를 PASS로 대체하지 않는다.

최종 검증일: 2026-10-15. Debug/Release x64 및 기존 fault 빌드 성공, 오류·경고 없음.

| 최종 산출물 기준 | Debug | Release |
|---|---:|---:|
| 기존 CLI/파일/Unicode/metadata (fault 포함) | 169/169 PASS | 169/169 PASS |
| 기존 GUI/1:1 호환/발견/자동 수신/스크롤 | 137/137 PASS | 137/137 PASS |
| 신규 N:1 기능 및 GUI | 74/74 PASS | 74/74 PASS |
| 합계 | **380/380 PASS** | **380/380 PASS** |

최종 산출물별 총 **760/760 PASS**다. 단일 runner 한 번의 결과라고 주장하지 않는다.
초기 전체 runner는 Debug 341/363, Release 346/363으로 실패했고 해당 JSON/로그를 보존한다.
확인된 GUI 진단/등록 해제/호환 모드 경쟁을 수정하고 GUI 전체를 다시 검사했다.
GUI만 재빌드했으며 CLI 바이너리 SHA-256이 이전 통과 산출물과 동일한 것을 확인했다.
CLI/GUI의 최신 증거와 스크롤 재검사만 합산하고 이전 실패/중복 PASS는 더하지 않는다.

마지막 Debug 스크롤 최초 실행은 숨겨진 창의 GetDC 픽셀을 비교해 8/32에서 중단됐다.
before/repaint 이미지를 확인했고, 테스트가 창을 실제로 표시하지 않은 상태임을 확인했다.
제품 렌더링을 변경하지 않고 창을 표시한 뒤 같은 최종 실행 파일로 32/32를 재검사했다.
새 N:1 GUI도 서버/클라이언트 양쪽을 표시한 상태에서 각각 8개씩 통과했다.
정지 후 픽셀 일치가 움직이는 동안의 모든 잔상이나 모든 DPI를 보증하지는 않는다.

10개 제한 검사는 실제 GUI 클라이언트 2개와 독립 raw TCP peer 8개로 확인했다.
기본 통합 검사는 서버 GUI 1개+클라이언트 GUI 3개로 실행했다. 실제 10대 PC 처리량 검증은 아니다.
수명 검사는 구성별 6회 재접속과 60초 이상 지속 통신이며 수시간/수일 soak를 완료한 것은 아니다.
개발 중 Application Control 차단 기록과 키보드 포커스 실패도 보존하며 정책 변경/우회는 하지 않았다.
최종 빌드는 실행 가능했지만 다른 Windows 환경의 실행 허용을 보장하지 않는다.

증거(로컬 `tests/runs`는 Git 제외):
- 통합 최종 목록: `tests/runs/n1-final-results.json` (suite별 원본 evidence 경로 포함).
- 최초 전체 실행: `tests/runs/release-validation-135da50941034ee5a8d7e35161b9222d/`.
- 최종 GUI/빌드: `tests/runs/n1-gui-final-250f627e1b794b61870f3dfce5330287/`.
- 마지막 Debug 스크롤: `tests/runs/n1-gui-scroll-Debug-visible-recheck.log`.
- 산출물 hash: `tests/runs/n1-final-artifact-hashes.json`와 CLI 변경 없음 비교 자료.
- 개발 중 실패/차단: `n1-integration-Debug.log`, `n1-final-regression.log`,
  `n1-gui-final-regression.log`, `n1-gui-final-regression-2.log`, 각 run의 `ui.log`/CodeIntegrity/PNG.

최종 GUI SHA-256:
- Debug: `565FA9619E56F81BB301DF8CC67E9FFC8656CE97B53693040583EBC25041D498`.
- Release: `D7319420E5AAB6D401E5E582267144ED5341559FEB8680C3B655983DC200C783`.

## 남은 실제 다중 PC 확인

- 실제 서버+클라이언트 3대 이상 LAN 발견/승인/키 등록·IP 변경과 최대 10개 운영.
- 서로 다른 디스크/권한/공간 조건에서 동시 양방향 파일 전송, 장시간 soak, 케이블 단절·재접속.
- 이전 v1.1.0 전송의 명시적 호환 모드 복구, N:1 경로 변경 후 원래 경로 복원.
- 100/125/150% DPI·실제 모니터 이동·가림/복원·빠른 스크롤의 시각적 잔상.
- 실제 Enter/Shift+Enter·한글 IME 조합·파일 drop, 다수 대화 창의 입력 포커스.
- 보안 정책을 정상적으로 만족하는 환경에서 실행. 차단된 기능을 성공으로 보고하지 않는다.
