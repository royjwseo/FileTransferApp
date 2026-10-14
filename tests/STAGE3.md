# 3단계: requestId 기반 파일 전송 상태 머신

3단계는 기존 소켓 수명 안정화와 승인 후 안전 저장을 보존하면서, 파일 전송의 식별자·상태 검증·저장 완료 확인을 추가한다. `Protocol`에 송신/수신 상태를 모으고 `Network`는 I/O와 대기를 담당한다. 기존 저장 핸들을 별도 `FileTransfer`로 옮기면 상태가 분산되고 변경 범위가 커지므로 이번 단계에서는 이동하지 않는다.

## 메시지 형식

기존 5바이트 프레임 헤더 `type:u8 + payloadLength:u32 big-endian`은 유지한다. 모든 파일 메시지 payload의 첫 8바이트는 **0이 아닌 requestId:u64 big-endian**이다. TEXT는 변경하지 않는다. payload 길이 제한 1MiB에는 ID 8바이트도 포함된다.

| 메시지 | requestId 뒤의 내용 |
|---|---|
| FILE_REQUEST(6), FILE_BEGIN(2) | fileSize:u64 big-endian + 안전한 ASCII 파일명, 기존 메타데이터 검증 유지 |
| FILE_ACCEPT(7), FILE_REJECT(8), FILE_END(4), FILE_ACK(5) | 없음 |
| FILE_CHUNK(3) | 1바이트 이상의 파일 데이터; 앱 송신 청크는 64KiB |
| FILE_ERROR(9) | 출력 가능한 ASCII 사유 1~256바이트 |

호스트는 1,3,5,…, 클라이언트는 2,4,6,…을 발급한다. ID는 연결 안에서 재사용하지 않으며 오버플로 시 새 송신을 거부한다. 수신 REQUEST는 상대방의 홀짝 영역과 마지막 요청보다 큰 ID인지 검사해 재생을 차단한다. 양방향 동시 요청이 같은 숫자를 사용하지 않으므로 FILE_ERROR의 방향도 명확하다. 새 연결은 새 ID 영역을 시작한다.

**2단계와 wire format이 호환되지 않으므로 양쪽 모두 새 실행 파일을 사용해야 한다.** 자동 호환 모드나 requestId 없는 파일 메시지는 지원하지 않는다.

## 송신 측 상태 전환

| 현재 상태 | 허용 이벤트 | 다음 상태 / 처리 |
|---|---|---|
| Idle / Completed / Failed | 새 로컬 SendFile | 새 ID 발급 → AwaitAccept, FILE_REQUEST 전송 |
| AwaitAccept | 일치하는 FILE_ACCEPT, 빈 본문 | Sending |
| AwaitAccept | 일치하는 FILE_REJECT, 빈 본문 | Failed, SendFile false |
| Sending | 로컬 BEGIN, CHUNK 전송 | Sending, 매 프레임 전 취소/상태 확인 |
| Sending | 로컬 FILE_END 전송 | AwaitAck, 아직 성공 아님 |
| AwaitAck | 일치하는 FILE_ACK, 빈 본문 | Completed, SendFile true |
| AwaitAccept / Sending / AwaitAck | 일치하는 FILE_ERROR | Failed, 사유 출력, 오류에 재응답하지 않음 |
| AwaitAccept / AwaitAck | 기한 만료 | Failed, 같은 ID의 FILE_ERROR 통지, SendFile false |
| 진행 중 | 연결 종료 / 로컬 종료 / I/O 실패 | 취소, SendFile false, 기존 shutdown → join → close |

- **ID가 같아도** AwaitAccept의 ACK, Sending의 ACK/중복 ACCEPT, AwaitAck의 ACCEPT/REJECT는 거부한다. 진행 중 상태 위반은 해당 ID로 FILE_ERROR를 통지하고 연결을 종료한다.
- 완료·실패한 요청의 중복 응답이나 활성 송신과 다른 ID의 ACCEPT/REJECT/ACK/ERROR는 로그를 남기고 무시한다. 새 요청에 이전 응답을 적용하지 않는다. 단, 메시지 형식 자체가 잘못됐으면 연결을 종료한다.
- `FILE_END` 직전에 AwaitAck를 설정해 즉시 도착하는 ACK를 놓치지 않는다. 성공 반환·완료 출력은 END의 실제 송신 성공과 해당 ID의 ACK 수신이 모두 확인된 후에만 수행한다. 네트워크 I/O 중에는 상태 잠금을 보유하지 않는다.
- 승인 제한은 60초, 저장 완료 ACK 제한은 15초다. 응답 처리 전에 절대 기한을 확인하므로 만료 후 ACCEPT/ACK가 실패한 요청을 되살리지 않는다.
- 송신 파일 읽기 실패는 Failed 및 해당 ID의 FILE_ERROR를 예약한다. 소켓 송신 정체는 기존 2초 SO_SNDTIMEO로 연결을 중단한다.

## 수신 측 상태 전환

| 현재 상태 | 허용 이벤트 | 다음 상태 / 처리 |
|---|---|---|
| Idle | 새 ID의 유효한 FILE_REQUEST | PendingApproval; 아직 폴더·파일을 생성하지 않음 |
| PendingApproval | 로컬 /accept | AwaitBegin, 같은 ID의 FILE_ACCEPT |
| PendingApproval | 로컬 /reject | Idle, 같은 ID의 FILE_REJECT |
| AwaitBegin | 같은 ID·승인 메타데이터의 FILE_BEGIN | Receiving, 안전 검증 후 소유한 .part 생성 |
| Receiving | 같은 ID의 비어 있지 않은 FILE_CHUNK | Receiving, 승인 크기 초과 거부 |
| Receiving | 같은 ID의 FILE_END, 빈 본문·정확한 총 크기 | flush + 덮어쓰기 없는 핸들 rename 성공 후 FILE_ACK 예약 → Idle |
| PendingApproval / AwaitBegin / Receiving | 같은 ID의 FILE_ERROR | 소유한 .part 정리 → Idle, 재응답하지 않음 |
| PendingApproval / AwaitBegin / Receiving | 기한 만료 | 같은 ID의 FILE_ERROR, 소유한 .part 정리 → Idle |
| 진행 중 | 연결 종료 / 로컬 종료 | 수신 루프 종료에서 소유한 .part 정리 → Idle |

- 승인 대기는 60초, 승인 후 BEGIN 대기와 수신 데이터 무진행 제한은 각각 15초다. 유효한 BEGIN/CHUNK 후에만 수신 기한을 갱신한다. TEXT·부분 프레임·다른 요청의 응답은 갱신하지 않는다.
- recv의 250ms 폴링과 새 바이트 처리 시 만료를 검사한다. 지속적인 TEXT 수신으로 만료를 피할 수 없다. 실제 정리 시점은 I/O/잠금 스케줄링 영향을 받는다.
- 잘못된 ID의 BEGIN/CHUNK/END, 승인 전 데이터, 중복 BEGIN/REQUEST, 재사용 ID, 잘못된 메타데이터 및 저장 오류는 FILE_ERROR 후 연결을 종료한다. ID 누락·0처럼 안전하게 식별할 수 없는 형식 오류는 통지 없이 연결을 종료한다. 이 경우 진행 중 양방향 요청 모두 취소된다.
- 정상 거절·요청별 FILE_ERROR·타임아웃 뒤에는 같은 연결에서 더 큰 새 ID로 다시 요청할 수 있다. 이미 종료한 수신 요청의 늦은 데이터는 새 요청에 적용하지 않고 위반으로 종료한다.
- .part 삭제는 소유한 핸들에만 적용하며 기존 파일·기존 .part는 보존한다. 최종 저장 후 ACK 유실/전송 실패가 발생하면 송신은 실패하지만 수신된 최종 파일은 보존된다. 이 경우 자동 재전송이나 덮어쓰기는 하지 않는다.

## 동시성과 회귀 범위

송신과 수신은 독립 상태를 갖는다. 한 연결에 방향별 하나씩 파일 요청을 진행할 수 있으며 승인 대기 중에도 반대편 요청을 승인/거절할 수 있다. `protocolMutex_`가 상태 및 저장을 보호하고 기존 sendMutex/receiveMutex/socketMutex의 소켓 수명 규칙은 유지한다. BEGIN/CHUNK/END/ACCEPT/REJECT의 네트워크 I/O는 상태 잠금 밖에서 수행해 양방향 송신 중에도 수신이 진행된다. 상태 확인 직후 취소가 도착하면 이미 시작한 프레임 하나가 전송될 수 있으나 상대방은 종료된 ID의 데이터를 새 전송에 적용하지 않는다. 종료 취소 플래그는 atomic이다. 조건 변수 알림 유실 가능성은 최대 250ms 간격 재확인으로 제한한다.

변경 파일: `Protocol.h/.cpp`, `Network.h/.cpp`, `tests/stage1.ps1`, `tests/stage2.ps1`, `tests/stage3.ps1`, 이 문서와 README. `.gitignore`, 빌드 구성, FileTransfer 빈 클래스는 유지한다.

## 검증 실행

Visual Studio Community 2026 / C++20 / v145 / x64. 현재 솔루션 빌드 출력은 프로젝트 루트의 `x64/<Configuration>/FileTransferApp.exe`다. 과거 `FileTransferApp/x64` 경로의 실행 파일을 테스트하지 않는다. 기존 1·2단계 스크립트의 검사 항목은 유지하고 requestId wire format과 ACK 본문을 갱신했다.

사용자 허가를 받은 **테스트 PowerShell 프로세스에만 RemoteSigned**를 지정했다. 시스템/사용자 정책 및 Smart App Control은 변경하지 않았다.

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' FileTransferApp.slnx /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' FileTransferApp.slnx /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage1.ps1 -Exe x64/Release/FileTransferApp.exe
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage2.ps1 -Exe x64/Release/FileTransferApp.exe
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage3.ps1 -Exe x64/Release/FileTransferApp.exe
# Debug도 -Exe x64/Debug/FileTransferApp.exe로 동일 실행
```

신규 테스트는 실제 프로세스와 raw TCP peer로 다음을 검증한다:

- 송신 동일 ID의 조기 ACK, 중복 ACCEPT, 합쳐진 ACCEPT+ACK, END 후 REJECT 거부 (4).
- 다른 ID 응답 무시·END 후 ACK 전 작업 유지, 완료된 요청 중복 응답, 이전 요청 응답과 새 요청 분리 (3).
- ACK 대기 중 FILE_ERROR·연결 종료·ACK 타임아웃 및 늦은 ACK 거부 (3).
- BEGIN/CHUNK/END의 잘못된 ID, BEGIN 전 CHUNK/END, 빈 CHUNK, 중복/재생 REQUEST, 누락/0/홀짝 오류 ID 및 임시 파일 정리 (11).
- BEGIN 대기·수신 중 타임아웃, 송신측 FILE_ERROR의 임시 파일 정리와 동일 연결 복구 (3).
- 송신 승인 타임아웃과 늦은 ACCEPT, 연속 TEXT 중 수신 승인 만료와 늦은 사용자 승인 거부 (2).
- 실제 두 앱의 양방향 동시 1MiB 파일 저장·각 ACK 완료·SHA-256 (1).

총 **신규 27개**, 기존 12+20개를 합쳐 구성당 59개다. 60초/15초 실제 기한을 기다리며 테스트용 타임아웃 단축이나 보안 우회를 사용하지 않는다.

### 최종 결과 (2026-10-14)

| 항목 | Release x64 | Debug x64 |
|---|---|---|
| 빌드 | PASS, 오류·경고 없음 | PASS, 오류·경고 없음 |
| 기존 1단계 | 12 PASS | 12 PASS |
| 기존 2단계 | 20 PASS | 20 PASS |
| 신규 3단계 | 27 PASS | 27 PASS |
| 합계 | 59 PASS | 59 PASS |

두 구성 총 118개 PASS. 2단계 당시의 Debug 실행 차단은 이번 새 빌드에서 재현되지 않았으며, 실제 Debug 프로세스로 전체 테스트를 실행했다. 보안 설정 변경이나 Smart App Control 우회는 하지 않았다.

로컬 상세 로그와 테스트 파일은 .gitignore에 따라 Git에 포함하지 않는다:

| 구성 | 스위트 | tests/runs/ 아래 경로 |
|---|---|---|
| Release | stage1 | `02b2b64297e94c82856a9455927b5165/` |
| Release | stage2 | `2fb00e09f3f0403dbfd9a5691fe7bf41/` |
| Release | stage3 | `a4f2325dbbc346cdaf8814dea83de03a/` |
| Debug | stage1 | `2bce90e297fb4fdb9483b51f2eba59e6/` |
| Debug | stage2 | `0b73698dc05c420aa54dd0cb563790d3/` |
| Debug | stage3 | `4f60610ba9ca47b2bb26ee60abf27860/` |

신규 양방향 완료 테스트에서 드러난 상태 잠금 중 송신 정체는 I/O를 잠금 밖으로 이동해 수정했고, 위 결과는 해당 수정 및 절대 기한 검사까지 반영한 최종 빌드의 전체 재실행 결과다.

## 제한

localhost 검증이며 실제 두 PC의 LAN/Wi-Fi 환경은 미검증이다. 디스크/정리 API 실패의 강제 주입 및 강제 프로세스 종료 후 정리는 포함하지 않는다. SHA-256은 테스트에서 외부 비교하며 프로토콜 자체의 내용 해시는 추가하지 않았다. ACK는 상대 프로그램이 저장 완료를 통지했다는 의미이며 피어의 악의적 허위 ACK를 인증하는 기능은 아니다.
