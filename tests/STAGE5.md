# 5단계: 취소·재접속·이어받기·무결성 — 최종 검증 미완료

기준 커밋 `b87ff47`, 최초 로컬 보존 커밋 `32391b3` (`feature/stage5-recovery`)를 유지한다. **2026-10-14 최종 검증 시도에서도 Debug 및 오류 주입 Release의 SAC 차단으로 전체 188개 회귀를 완료하지 못했다.** 실행 가능한 보존 Release의 회귀 89개·신규 26개가 통과했고, 이후 안정화 수정의 Release 관련 테스트 21개가 통과했다. 서로 다른 빌드의 중복 항목을 합산하지 않는다. 이번 안정화 수정은 같은 브랜치의 후속 로컬 보존 커밋으로 기록하며 출시·검증 완료를 뜻하지 않는다. `main`은 안정적인 4단계 `b87ff47`에 유지하고 원격 push와 main 병합은 하지 않는다. 아래 이전 실행 기록은 당시 빌드의 결과다. 현재 결과는 마지막 절을 기준으로 확인한다.

## 실행 경로 및 보안 진단

| 구분 | 경로 | 생성·실행 방식 |
| --- | --- | --- |
| 표준 Debug | `FileTransferApp/x64/Debug/FileTransferApp.exe` | MSBuild → MSVC cl/link, 테스트 PowerShell → TestApp의 .NET Process.Start |
| 표준 Release | `FileTransferApp/x64/Release/FileTransferApp.exe` | 위와 같음 |
| 과거 차단된 별도 출력 | `tests/runs/stage5-build/Release/bin/FileTransferApp.exe` | OutDir/IntDir만 분리한 일반 Release, 오류 주입 매크로 없음 |
| 오류 주입 전용 | `tests/runs/stage5-fault/{Debug,Release}/bin/FileTransferApp.exe` | `Stage4Faults.props`로 FILETRANSFER_TESTING 정의. 운영 빌드에는 없음 |

실행은 `UseShellExecute=false`, `CreateNoWindow=true`, stdin/stdout/stderr 리디렉션이다. 테스트마다 작업 디렉터리를 `tests/runs/<GUID>/...`로 분리하지만 exe는 지정된 원래 빌드 경로에서 실행한다. 알려진 빌드 호출은 MSBuild 기록으로 확인했고, Windows Code Integrity 이벤트는 실행 요청 프로세스가 Windows 기본 powershell.exe임을 확인한다. 모든 과거 생성 PID를 증명하는 프로세스 감사 로그는 확보하지 않았다.

사용자가 정상 실행을 확인한 표준 빌드로 다시 시작하여 **재접속 4개가 Debug와 Release 각각 모두 통과**했다. `tests/runs/1b3497f4b6df4d1daa318a96b8a7f0a9/`(Release), `02d21678e1b1416fb58a9e5e4a787285/`(Debug). 기존 실패는 테스트의 TcpListener 핸들이 자식 앱에 상속되어 부모 Stop 이후에도 수신 대기 소켓이 남은 문제였다. `SocketIsolation.ps1`에서 HANDLE_FLAG_INHERIT를 제거했고, Windows PowerShell 자식과 실제 TCP로 재현/수정 확인 2개도 통과했다. endpoint 자기 연결 검사만으로 해결되는 문제는 아니었다.

이후 소스 수정·새 빌드까지 일부 테스트가 실행되었으나, 최종 빌드에서는 다음 차단이 다시 발생했다. 시스템 보안 설정이나 실행 위치를 바꾸어 우회하지 않았다.

| KST 2026-10-14 | 대상 | Code Integrity 이벤트 | SHA-256 flat hash |
| --- | --- | --- | --- |
| 10:18:55 | 별도 일반 Release | 3077/3118, record 187/189 | `CA0E60384E3F60120A33D8AE01BC3DE49A221EEFAF8C1A89DCB098BAAB2AEF1D` |
| 11:05:15 | **표준 Release** | 3077 record 192, 3118 | `A90595EC8F91B3FCF8319A6CDFEA15397A0F8224BF013FFE2B4200E6821C6001` |
| 11:05:55 | **표준 Debug** | 3077 record 197, 3118 | `C70F4D16F88EA2087CF73145D0BAB23B5D9E602D9BB4CAB22E68F4D0E7A9DDF7` |

두 표준 바이너리의 Get-FileHash가 이벤트와 일치한다. 오류는 `An Application Control policy has blocked this file`, `tests/stage1.ps1:59 [TestApp]::new(...)`. PolicyName=`VerifiedAndReputableDesktop`, GUID=`{0283ac0f-fff1-49ae-ada1-8a933130cad6}`. 최초 진단에서 status=`0xc0e90002`, requested signing level 2 / validated 1, 서명 상태 NotSigned였다. 3118이 Smart App Control을 명시한다. 해당 시각 AppLocker EXE/DLL 차단 이벤트는 발견하지 못했다.

최신 실패 로그: `tests/runs/suite-19cb3f873fbc4a118b67b5433c30324b/stage1.log`(Release), `suite-60cf098a13204a098c32d729a374827a/stage1.log`(Debug). 이벤트 원문은 전자의 `CodeIntegrity-events.xml`에 보존했다. 이것은 기능 테스트 실패가 아니라 **실행 차단으로 0개 실행**된 결과다.

프로세스 범위 RemoteSigned는 사용자 승인대로 테스트 스크립트에만 사용했다. Smart App Control, AppLocker, 방화벽 및 시스템/사용자 정책을 변경하지 않았다. SAC 개별 앱 허용 예외는 없으며 정식 신뢰된 코드 서명 또는 개발 실행이 정식 허용되는 검증 환경이 필요하다. 관리자 실행 자체는 신뢰 판정을 대신하지 않는다. 서명용 개인 키를 생성하거나 신뢰 루트를 추가하지 않았다.

공식 근거: [Microsoft SAC FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions), [Microsoft Learn](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/overview).

## 식별자와 wire 형식

프레이밍은 기존 `type:u8 + payloadLength:u32 BE`다. 모든 파일 메시지는 먼저 `requestId:u64 BE`를 갖는다. 호스트 홀수/클라이언트 짝수, 연결별 단조 증가 규칙을 유지한다. 새 연결에서는 파서·요청 상태·watermark·취소/완료 캐시를 초기화하고 새 세대로 구분한다. 이전 송신 종료 및 I/O 소켓 정리가 끝나야 새 연결을 채택한다.

영속 transferId는 SHA-256(`FTA5\0` 5바이트 + size:u64 BE + digest:64 lowercase hex + ASCII filename)이다. 동일 내용/이름/크기의 작업을 새 requestId에서 식별한다. source digest는 요청 전 전체 원본을 스트리밍 해시하여 계산한다.

requestId 뒤의 body:

| 메시지 | body |
| --- | --- |
| FILE_REQUEST=6, FILE_BEGIN=2 | transferId:64hex + digest:64hex + mode:u8 + size:u64 BE + safe ASCII filename |
| FILE_ACCEPT=7 | verified prefix offset:u64 BE + prefix SHA-256:64hex |
| FILE_CHUNK=3 | 파일 바이트, 앱 송신은 최대 64KiB |
| FILE_END=4, FILE_REJECT=8, FILE_CANCEL=10 | 비어 있음 |
| FILE_ACK=5, FILE_COMPLETE=11 | transferId:64hex + 전체 digest:64hex |
| FILE_ERROR=9 | 인쇄 가능한 ASCII 오류, 최대 256바이트 |

mode 0=fresh, 1=explicit resume, 2=completion query. BEGIN은 승인된 REQUEST와 모든 메타데이터가 같아야 한다. COMPLETE는 resume/status의 AwaitAccept에서만 허용된다. ACK는 AwaitAck/CancelPending에서만 허용된다. requestId, 상태, proof가 모두 일치해야 Completed다. 100% 데이터 전송과 END 자체는 성공이 아니다. 이전 ID 응답은 무시하고 같은 ID의 불법 상태 전환은 거부한다.

**protocol=5는 protocol=4와 호환되지 않는다.** 양쪽 PC에서 `--version`과 exe SHA-256을 비교해야 한다. 자동 버전 협상·상대방 인증은 없다. 테스트의 raw TCP helper를 새 형식으로 갱신했으며 기존 항목을 삭제하지 않았다.

## 상태 전환과 취소

송신: Idle/Failed/Cancelled/Completed → AwaitAccept → Sending → AwaitAck → Completed. 거절/오류/타임아웃/연결 종료는 Failed, 명시적 취소는 Cancelled다. AwaitAck에서 로컬 취소하면 CancelPending으로 결과를 기다린다. 상대방 취소 확인이면 Cancelled, 이미 저장된 동일 proof ACK가 먼저 확인되면 Completed다. 타임아웃이나 종료로 결과가 불명확하면 성공으로 추정하지 않고 이후 `/status`를 사용한다. 이미 Completed인 결과는 연결 종료나 취소로 되돌리지 않는다.

수신: Idle/Cancelled → PendingApproval → AwaitBegin → Receiving → 검증/flush/commit/ACK → Idle. 거절은 Idle, 활성 취소는 Cancelled 및 소유 part 삭제다. 저장 완료 요청에 뒤늦게 CANCEL이 오면 bounded 완료 캐시로 ACK를 다시 보낸다. 취소/파일 I/O 실패의 in-flight 데이터는 최대 64개 requestId tombstone으로 무시한다. 완료 캐시도 최대 64개다. 문법/상태/크기 위반은 연결을 종료하고 소유 부분 파일을 정리한다. 파일 저장 I/O 및 해시 실패는 해당 방향만 실패시키고 상대 방향의 TCP 흐름을 유지한다.

재접속은 `/reconnect` 수동 또는 `--reconnect` 선택적 자동이다. 자동은 250/500/1000ms 백오프와 최대 3회 시도, 시도별 select/connect/accept 최대 2초다. shutdown은 I/O를 깨우고, send/recv 잠금 확보 후 closesocket한다. fileSendMutex로 이전 송신의 반환을 기다린 뒤 ResetSession한다. 명시적 quit은 백오프를 깨우고 재접속을 중단한다. 자동 파일 재송신은 없다. 스케줄링/이전 작업 정리 시간은 대기 상한 합과 별개다.

## 영속 저장·이어받기·무결성

`TransferStorage.h`와 FileTransfer.cpp의 RAII 구현이 디렉터리·데이터·manifest 핸들을 소유한다. Network는 소켓 및 작업 스레드, Protocol은 프레이밍/상태/승인과 storage 수명을 관리한다. 소켓 잠금 순서는 기존대로 유지했다.

- 원본은 CreateFileW GENERIC_READ/FILE_SHARE_READ로 열어 전송·해시·ACK 대기 동안 쓰기/삭제를 막는다. 이미 쓰는 프로세스와 충돌하면 시작하지 않는다. 요청 전 전체 SHA-256, 재개 prefix, 전송한 전체 데이터의 SHA-256을 확인한다.
- 수신은 pinned non-reparse `received/` 아래 `<name>.part`를 CREATE_NEW/독점 핸들로 생성한다. 기존 최종 파일이나 알 수 없는 part를 덮어쓰지 않는다.
- `<name>.part.fta.new`를 CREATE_NEW로 생성하여 DPAPI로 보호한 transferId+volume/file index를 기록하고 flush한다. 이후 owned handle의 no-replace rename으로 `<name>.part.fta`를 원자적으로 게시한다. 불완전한 게시 파일은 자동으로 믿거나 삭제하지 않는다.
- 연결 종료 시 owned part를 flush하고 핸들을 닫아 보존한다. 비정상 프로세스 종료 후에도 part와 게시된 manifest가 남으면 재개 후보가 된다. 기록된 offset을 믿지 않고 **열린 실제 파일 크기와 전체 prefix 해시**로 offer를 계산한다.
- manifest를 DPAPI 검증하고 단일 hardlink/non-reparse 데이터 객체의 volume/file ID가 일치해야 연다. 동일 이름의 다른 파일, 교체된 part, 조작된 manifest는 거부한다. 기존 알 수 없는 파일은 그대로 보존한다.
- `/resume <transferId> <sourcePath>`는 source의 새 전체 identity가 일치하고 offer offset이 범위 안이며 prefix 해시가 일치해야 전송한다. `/file`은 기존 part를 자동 재사용하지 않는다.
- 수신 SHA-256에는 기존 prefix와 추가 바이트를 모두 포함한다. 전체 크기/해시 검증 → FlushFileBuffers → no-replace rename → 동일 proof ACK 순서다. 해시 불일치 시 최종 저장/성공 ACK를 금지한다.
- `/status <transferId> <sourcePath>`는 수신자 `/accept` 후 소유 identity·전체 크기·해시가 확인된 최종 파일만 COMPLETE로 답한다. 기존 데이터를 재전송/덮어쓰기하지 않는다. 반복 조회는 파일을 수정하지 않는다. ACK 유실 직후 임의 성공 판정은 없다.
- 소유 part는 명시적 취소·프로토콜 위반·수신 타임아웃·파일 오류 시 삭제한다. 정상 저장의 manifest는 최종 파일과 함께 무기한 보존하며 자동 만료/정리하지 않는다. 앱이 종료되고 소유 관계를 확인한 뒤 사용자가 해당 part/manifest를 수동 정리할 수 있다. 최종 파일은 자동 삭제하지 않는다.

긴 소스/prefix/final 해시는 protocolMutex 밖의 로컬 RAII 핸들로 수행한다. 복구 완료 시 requestId와 세대를 다시 검사한다. 해시 중 종료·상대 취소·세대 변경을 64KiB마다 확인한다. 수신 청크 WriteFile은 기존 짧은 상태 잠금 구간에서 직렬화하며, 동기 I/O의 커널 지연에 대한 강제 취소는 없다. 전체/prefix 해시에 BCrypt streaming/snapshot을 사용한다.

고정 64KiB 송신/수신/해시 작업 버퍼, 기존 프레임 상한 1MiB+5, TCP 송수신 버퍼 각 64KiB를 유지한다. 파일 크기는 1TiB 상한 uint64_t다. 초기 hash와 전송/hash 단계는 순차 처리이며 파일 전체 적재나 무제한 청크 큐는 없다. 진행률의 초기 offset은 속도 계산에서 제외하고 500ms 주기로 출력한다. 동시 콘솔 메시지는 osyncstream으로 묶어 완료 로그가 진행 출력 사이에 잘리는 문제를 보강했다.

## 테스트 결과 — 빌드별로 구분

모두 단일 Windows PC의 실제 프로세스/localhost TCP다. 실제 두 PC LAN 검증은 하지 않았다.

| 항목 | 이번 작업 중 결과 | 최신 빌드 상태 |
| --- | --- | --- |
| Debug/Release 표준 및 오류 주입 빌드 | 모두 컴파일/링크 성공 | 성공 |
| 재접속 테스트 | Debug 4, Release 4 통과 | 전체 재검증 미완료 |
| 취소 테스트 | Release 6 통과 | 미재검증 |
| 기존 stage1 | Release 12 통과 | 실행 차단 |
| 기존 stage2 | 초기 Release 20 통과; 후속 실행 크기 초과 케이스 실패 | 오류 격리에서 프로토콜 크기 위반을 다시 fatal로 수정, 컴파일 성공 후 실행 차단 |
| 기존 stage3 | ACK/상태/timeout 항목 실행, 마지막 양방향 완료 로그 검사 실패 | 콘솔 메시지 원자 출력 수정, 컴파일 후 실행 차단 |
| 기존 stage3_validation | Release 16 통과 | 미재검증 |
| 기존 stage4 | Release 19 통과, 전용 오류 주입 포함 | 미재검증 |
| 초기 stage5_resume | Release 9개 집계 통과 | 보강 후 미재검증 |
| stage5_boundaries | 추가 작성 및 구문 검사 | 미실행 |
| 최종 전체 runner | Release/Debug 각각 stage1 시작 시 차단 | **188개 회귀와 모든 신규 테스트 완료 아님** |

통과한 초기 stage5_resume에는 8MiB 중간 재개, 두 번 재중단, part 변조/원본 변경/잘못된 ID 거부, **1GiB+64KiB 실제 재개**, ACK 유실 뒤 재시작/반복 조회, TCP 데이터 손상 시 전체 해시 실패, 저장 후 취소·ACK 경합 10쌍이 포함된다. 로그 `tests/runs/664b95e3ac534acbb333950e6b340347/`.

이후 보강한 manifest 변조/foreign part/새 요청 구분/완료 파일 변조/원본 쓰기 잠금 검사는 미실행이다. `stage5_boundaries.ps1`은 4GiB+1 offset의 실제 source/prefix 해시와 tail만 raw TCP로 전송하는 **경계 시뮬레이션** 및 잘못된 offset/prefix/proof와 양방향 한쪽 취소를 검증하도록 작성했으나 아직 실행하지 않았다. 이를 실제 수신자 4GiB checkpoint 재개 성공으로 보고하지 않는다.

기존 stage4의 실제 **4GiB+64KiB 전체 TCP 전송과 SHA-256**은 이번 중간 Release 빌드에서 통과했다. 측정 로그 `tests/runs/22783ed049b246c692fff0c963aeb282/measurements.json`(실제 파일명은 해당 폴더 참고):

| 실제 localhost 전송 | 시간 | 평균 MiB/s | 송신/수신 peak private bytes |
| --- | --- | --- | --- |
| 1GiB+64KiB | 2.535s | 403.98 | 2486272 / 2371584 |
| 4GiB+64KiB | 6.425s | 637.56 | 2486272 / 2375680 |
| 8MiB 느린 중계 | 2.736s | 2.92 | 송신 2490368 |

1→4GiB에서 private bytes가 선형 증가하지 않았다. 동일 연결 12회 반복의 송신/수신 핸들 수는 각각 101→101, 114→114였다. Windows 11 Pro 10.0.26200 / Core Ultra 5 245K / NTFS, localhost 측정이며 실제 LAN 처리량이 아니다. 소스 hash 및 승인 등의 시간이 포함된 테스트 구간 평균이다. 디스크/캐시 상태 영향을 받는다.

## 재실행과 남은 항목

필요한 실행 환경:

- Windows x64, Visual Studio의 C++ 데스크톱 개발 도구와 Windows SDK, C++20 지원 MSVC/MSBuild. 이번 빌드는 Visual Studio 18 Community 도구로 수행했다.
- Windows PowerShell 5.1/.NET Framework와 Add-Type 사용이 허용된 환경. 테스트 스크립트에만 프로세스 범위 `RemoteSigned`를 지정한다.
- **새로 빌드한 표준 및 오류 주입 실행 파일 모두** 현재 Windows 보안 정책상 실행 가능해야 한다. 기존 버전 수동 실행 성공만으로 최신 파일의 실행 허용을 추정하지 않는다. 정책 해제나 다른 실행 경로를 이용한 우회는 하지 않는다.
- sparse 파일 생성과 DPAPI를 지원하는 쓰기 가능한 로컬 NTFS 작업 공간. source/part/최종 파일 및 측정용 여유를 위해 최소 10GiB 이상의 여유 공간을 권장하며 테스트가 실제 가용 공간도 확인한다. 생성 데이터·로그·산출물은 `tests/runs/`에 남을 수 있다.
- localhost TCP 사용과 고정 포트 9000이 가능해야 한다. 같은 포트를 사용하는 앱/테스트와 동시에 실행하지 않는다. 방화벽을 임의로 변경하지 않는다. 두 PC LAN 시험은 별도의 후속 검증이다.

프로젝트 루트에서 표준/전용 빌드를 같은 소스로 만든다. `$msbuild`에는 설치된 MSBuild.exe 경로를 지정한다. 아래는 이번 환경의 경로다. 빌드 오류가 있으면 테스트를 시작하지 않는다.

```powershell
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
foreach ($configuration in @('Release','Debug')) {
    & $msbuild FileTransferApp/FileTransferApp.vcxproj /nologo /v:minimal "/p:Configuration=$configuration" /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "Standard build failed: $configuration" }
    & $msbuild FileTransferApp/FileTransferApp.vcxproj /nologo /v:minimal "/p:Configuration=$configuration" /p:Platform=x64 "/p:ForceImportBeforeCppTargets=$pwd/tests/Stage4Faults.props" "/p:OutDir=$pwd/tests/runs/stage5-fault/$configuration/bin/" "/p:IntDir=$pwd/tests/runs/stage5-fault/$configuration/obj/"
    if ($LASTEXITCODE -ne 0) { throw "Fault build failed: $configuration" }
}
```

보안 정책상 실행이 허용되는 것을 확인한 뒤 프로젝트 루트에서 다음 명령을 사용한다. 빌드와 테스트는 동일 소스의 표준/전용 exe를 구분해야 한다. 고정 9000 포트 회귀 때문에 전체 suite끼리는 순차 실행한다.

```powershell
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/RunStage5.ps1 -Exe FileTransferApp/x64/Release/FileTransferApp.exe -FaultExe tests/runs/stage5-fault/Release/bin/FileTransferApp.exe
if ($LASTEXITCODE -ne 0) { throw 'Release validation failed or blocked; inspect logs/events' }
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/RunStage5.ps1 -Exe FileTransferApp/x64/Debug/FileTransferApp.exe -FaultExe tests/runs/stage5-fault/Debug/bin/FileTransferApp.exe
if ($LASTEXITCODE -ne 0) { throw 'Debug validation failed or blocked; inspect logs/events' }
```

Runner는 각 스크립트의 exit code/PASS 수/시간/로그를 tests/runs/suite-GUID에 기록하고 첫 실패에서 중단한다. 실패 시 Code Integrity 이벤트도 조회하며 지연 기록된 이벤트는 별도 확인해야 한다. 기존 5개 스크립트는 구성당 94개=총188개 항목을 유지했다. 의도적으로 바뀐 연결 종료 보존·파일 오류 격리·wire 형식의 기대값만 갱신했다.

남은 필수 작업: 최신 두 빌드의 전체 회귀·신규 테스트 실행, 새 경계 테스트 실패 수정, 반복 메모리/핸들 검증 재확인. 실패/차단 없는 최종 결과와 별도 사용자 지시 없이 main 병합이나 원격 push를 진행하지 않는다. 현재 보존 커밋은 `feature/stage5-recovery`에만 기록하며 `main`은 `b87ff47`이다.

## 로컬 보존 범위 (2026-10-14)

이번 작업은 기능 추가나 테스트 재실행 없이 현재 소스·프로젝트 설정·테스트 코드·이 문서를 보존하는 작업이다. 변경/추가된 텍스트 파일만 명시적으로 stage한다. 회사 내부 정보·인증 토큰·비밀번호·개인 키 패턴과 로컬 내부 경로/주소를 점검했으며 커밋 후보에서 해당 민감 정보를 발견하지 않았다. 이 점검은 자동 패턴 검사와 내용 확인 범위의 결과다.

기존 `.gitignore`는 유지한다. `tests/runs/`, `received/`, `.env`, `.codex/`, `.agents/`, `.aws/`, 빌드 디렉터리와 바이너리·인증서·키·로그는 커밋에 포함하지 않는다. 로컬 테스트 증거 경로와 exe 해시는 문서에만 기록하며 원시 이벤트 XML/로그·테스트 데이터·대용량 파일·산출물은 추적하지 않는다. 제외된 로컬 파일은 삭제하지 않는다. reset/변경 폐기/강제 push를 수행하지 않는다.

알려진 제한: ASCII 파일명, IPv4 수동 연결, 자동 버전 협상/암호화/상대 인증 없음. SHA-256과 DPAPI는 상대방 인증을 대신하지 않는다. DPAPI manifest는 같은 Windows 사용자 환경에 묶인다. 파일명이 달라지면 transferId도 달라진다. 단일 송신/수신 작업 구조이며 동시 같은 목적지 파일은 독점 핸들로 거부한다. 승인/복구 검사는 기존 60초 요청 기한을 따르므로 매우 느린 대용량 해시 스캔은 타임아웃될 수 있다. 전원 손실·스토리지 장치 오류까지 내구성을 보장하지 않으며 crash 게시 중 orphan 파일은 자동으로 삭제/복구하지 않는다. GUI/LAN 자동 검색은 미구현이다. 향후 UI/검색은 Network 소켓·스레드 소유권을 유지하고 명령/상태/진행 이벤트를 연결해야 한다.

## 최종 검증 시도 (2026-10-14, 보존 커밋 이후)

시작 시 feature/stage5-recovery=`32391b3`, main=`b87ff47`, 작업 트리 clean을 확인했다. 보존 커밋의 표준 바이너리를 각각 한 번 `--version`으로 .NET Process.Start에서 실행했다. Debug는 차단, Release는 exit 0이었다. 반복 빌드로 신뢰 판정을 바꾸거나 차단 파일을 다른 위치/이름으로 실행하지 않았다.

### 실행 환경 결과와 증거

| KST | 실제 실행 파일 | SHA-256 | 결과 |
| --- | --- | --- | --- |
| 12:33:38 | `FileTransferApp/x64/Debug/FileTransferApp.exe` | `C70F4D16F88EA2087CF73145D0BAB23B5D9E602D9BB4CAB22E68F4D0E7A9DDF7` | SAC, 3077 record 202 / 3118 record 204 |
| 검증 시작 | `FileTransferApp/x64/Release/FileTransferApp.exe` | `A90595EC8F91B3FCF8319A6CDFEA15397A0F8224BF013FFE2B4200E6821C6001` | 실행 가능, 보존 코드 테스트에 사용 |
| 12:36:47 | `tests/runs/stage5-fault/Release/bin/FileTransferApp.exe` | `0428437116D98178D930651F4B5CC5DC362121F5A872A894BA074F9B1C9D63D4` | SAC, 3077 record 207 / 3118 record 209 |

차단 오류는 두 경우 모두 `An Application Control policy has blocked this file`, 실행 요청은 Windows 기본 powershell.exe, 정책 GUID는 `{0283ac0f-fff1-49ae-ada1-8a933130cad6}`다. Debug 차단은 이전과 동일 파일 해시에서 지속됨을 확인했다. 일반 Release의 이전 차단이 이번에는 발생하지 않은 이유는 확보한 이벤트만으로 확정하지 않는다. 보안 정책이나 exe 내용은 실행 확인을 위해 변경하지 않았다.

실행 확인 JSON과 이벤트 XML: `tests/runs/final-probe-170941f32f0d442682e6428da0a5878a/`. 전체 runner 결과: `tests/runs/suite-ba048921dd1e4113abc74931e2bf61ec/results.json` 및 stage별 로그. 오류 주입 빌드의 첫 차단에서 해당 suite는 중단했다. 차단된 Debug와 오류 주입 exe를 같은 조건으로 반복 실행하지 않았다. 실행 가능한 일반 Release에만 의존하는 신규 테스트는 별도로 진행했다.

### 실제 테스트 집계

| 대상 빌드/스크립트 | 통과 | 나머지 결과 |
| --- | ---: | --- |
| 보존 Release stage1 | 12 | 모두 통과 |
| 보존 Release stage2 | 20 | 모두 통과 |
| 보존 Release stage3 | 27 | 모두 통과 |
| 보존 Release stage3_validation | 16 | 모두 통과 |
| 보존 Release stage4 | 14 | 오류 주입 5개는 실행 파일 차단으로 미실행 |
| 보존 Release stage5_cancel | 6 | 모두 통과 |
| 보존 Release stage5_reconnect | 4 | 모두 통과 |
| 보존 Release stage5_resume | 12 | 보강한 변조·원본 잠금·완료 조회 포함 모두 통과 |
| 보존 Release stage5_boundaries | 4 | 양방향 한쪽 취소 검사 실패, 아래 테스트 수정 후 통과 |
| 안정화 후 Release stage5_reconnect | 4 | 모두 통과 |
| 안정화 후 Release stage5_boundaries | 5 | 모두 통과 |
| 안정화 후 Release stage5_resume | 12 | 모두 통과 |
| Debug 전체 | 0 | 실행 차단으로 미실행. 수정 후 새 Debug는 컴파일만 확인하고 반복 실행하지 않음 |

보존 Release에서는 **기존 89개 + 신규 26개 = 115개 통과**, 신규 1개 실패를 기록했다. 기존 필수 188개 중 이번 시도에서 통과한 항목은 89개이며 Debug 94개 및 Release 오류 주입 5개는 미검증이다. 안정화 후 Release의 **관련 21개 통과**는 다른 빌드의 중복 항목이므로 115개에 더하지 않는다. 최신 코드의 188개 전체 회귀가 통과한 결과는 아니다.

이번에도 실제 1GiB+64KiB와 4GiB+64KiB 전체 TCP 전송 및 SHA-256, 양방향 256MiB, 느린 수신자, 동일 연결 12회 메모리/핸들 검사가 통과했다. 신규 테스트에서는 프로세스 종료/재시작과 두 번 재중단 후 복구, 1GiB 이상 실제 재개, part/manifest/foreign 파일 변조 거부, 원본 쓰기 잠금·변경 거부, ACK 유실 뒤 반복 /status와 완료 파일 변조 거부, 취소/ACK 경합 10쌍을 확인했다. 4GiB+1 offset 시험은 실제 원본/prefix hash와 tail TCP를 사용한 raw-peer 경계 시뮬레이션이며 실제 수신자 checkpoint의 4GiB 재개 성공을 뜻하지 않는다. 두 PC LAN은 미검증이다.

보존 신규 테스트 로그: `fd173da6844b4a46941e54302795cd6b/`(cancel), `2a9cb83aa0d94e308ecf1ec3aeb52d42/`(reconnect), `56df15668d154a5ebfb44902256e2969/`(resume), 모두 tests/runs 아래다. 수정 후 로그는 `tests/runs/final-targeted-55e3680e93da42349e7125eaa9d60db7/`의 3개 스크립트 로그와 builds.json에 기록했다.

### 최종 코드 검토 및 안정화 수정

- 소켓 shutdown→I/O 잠금 확보→close와 종료 시 sender/receiver join, 재접속 시 fileSendMutex 확보 순서를 검토했다. ACK 확인 후 Completed를 종료로 되돌리지 않는 규칙을 유지했다.
- 승인/거절의 Protocol 검증 후 실제 send 전에 자동 재접속이 소켓을 교체할 수 있는 경합을 발견했다. `Network::controlMutex_`로 콘솔 응답 확정·발행과 Reconnect의 소켓 교체를 직렬화했다. 긴 복구 해시에는 이 잠금을 잡지 않는다. Reconnect 잠금 순서는 fileSendMutex→controlMutex이며 반대로 획득하는 경로는 추가하지 않았다. protocolMutex를 잡은 채 소켓 I/O를 하지 않는다.
- 오래된 승인 작업이 세대 검사에서 거부됐을 때 새 세션의 응답 큐를 TakeReplies로 가져와 버리는 경로도 막았다. 유효한 승인일 때만 큐를 가져온다. 세션 경합의 정확한 스케줄을 고정한 전용 재현 테스트는 아직 없으며 관련 기능 테스트 21개와 정적 잠금 검토 범위의 검증이다.
- 신규 양방향 취소 테스트 실패는 첫 파일의 CHUNK를 너무 일찍 멈추어 반대 방향 ACCEPT도 같은 TCP 흐름에서 막은 시나리오였다. 양쪽 Sending을 확인한 뒤에만 데이터 중계를 멈추도록 수정했다. 수정 후 반대 방향 파일·ACK·SHA-256이 유지됨을 실제 TCP로 확인했다.
- RAII data/manifest/directory 핸들, CREATE_NEW/no-replace rename, DPAPI+volume/file identity, 전체/prefix SHA-256, 완료 캐시/취소 tombstone 상한, 잘못된 상태/크기 처리 및 최종 파일 보존 경로를 검토했다. 전원 손실 내구성, 강제 디스크 I/O 취소와 모든 스케줄의 데이터 레이스 부재를 입증한 것은 아니다.

안정화 후 표준 Debug/Release x64 빌드 모두 성공했다. 최신 Release SHA-256은 `1226AE722315956CD7BE3D20F3D48E9E3DFA4D2E072068B3E8A180FDB77195EE`, 최신 Debug는 `2E1D0BFDEB48C1093EC9A30BBBDE22986DA900FA2D1BE973315895E535631106`이다. 이전 차단 이벤트 해시는 이전 바이너리에 대한 증거이며 새 Debug가 차단됐다고 추정하지 않는다. 최신 Debug 및 동일 수정의 오류 주입 빌드/전체 suite는 승인된 환경에서 재검증해야 한다. 바이너리 해시 변경은 위 안전성 수정의 정상 컴파일 결과이며 차단 회피용 변형을 하지 않았다.

### 승인된 테스트 환경에서 완료하는 절차

1. 담당 관리자/보안 담당자에게 위 두 차단 경로·해시·SAC 이벤트와 최초 보존 커밋 `32391b3`, 후속 안정화 로컬 커밋 및 두 커밋 간 diff를 제공한다. 현재 검증 대상 커밋은 `git rev-parse feature/stage5-recovery`로 확인한다. 개인정보를 포함할 수 있는 원시 이벤트 로그는 Git에 넣지 않는다.
2. 조직이 승인한 Windows x64 개발/CI 검증 호스트를 준비하거나, 신뢰할 수 있는 제공자의 코드 서명 서비스로 **같은 소스의 표준 Debug/Release와 오류 주입 Debug/Release 4개**를 정식 서명한다. 인증서/개인 키를 저장소에 넣지 않는다. 이 작업에서 인증서 발급·서명·정책 변경을 수행하지 않았다.
3. 해당 환경에서도 SAC/방화벽/실행 정책을 임의로 끄거나 예외 우회를 만들지 않는다. 일반 관리자 실행이나 자체 서명 신뢰 루트 추가를 해결책으로 사용하지 않는다. [Microsoft 정식 코드 서명 안내](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/code-signing-for-smart-app-control), [개별 앱 예외가 없는 SAC FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions)를 따른다.
4. 앞 절의 C++20/Windows SDK/PowerShell/NTFS/10GiB/localhost 포트 요구 사항을 확인한다. 서명 전후 파일 해시 및 서명 검증 결과와 소스 diff를 기록하고 실행 허용을 각각 확인한다. DPAPI 재개 데이터는 같은 Windows 사용자에서 생성·재시작하는 새 테스트 데이터로 검증한다.
5. 앞 절의 RunStage5 명령을 구성별로 순차 실행해 기존 94×2개와 모든 신규 항목을 모두 통과시킨다. 실행 차단과 기능 실패를 구분하며 차단 시 동일 빌드 실행을 반복하지 않는다. JSON/로그와 4GiB 실전송·경계 시뮬레이션·자원 측정을 구분하여 기록한다.
6. 모두 통과한 결과를 확보하고 이후 별도 사용자 지시가 있을 때에만 main 비교·병합, 충돌 없는 일반 push와 원격 커밋 확인을 진행한다. 앞선 최종 검증 시도는 조건을 충족하지 못하여 커밋·병합·push하지 않았다. 이후 사용자가 안정화 수정의 **로컬 보존 커밋만** 명시적으로 요청했으므로 아래 보존 작업을 수행한다. main 병합 및 원격 push는 계속 금지한다.

### 안정화 수정의 후속 로컬 보존

`feature/stage5-recovery`에서 Network.h, Network.cpp, stage5_boundaries.ps1, 이 문서의 4개 변경을 검토하여 최초 보존 커밋 `32391b3` 위에 로컬 커밋한다. 커밋 메시지에 **final validation incomplete**를 명시한다. 이번 작업에서는 새 기능·빌드·테스트 실행·보안 정책 변경을 수행하지 않았으며 위 테스트 수와 SAC 차단 기록을 그대로 유지한다. 커밋은 검증 완료 선언이 아니다.

소스 변경은 승인/거절 발행과 재접속의 직렬화 및 유효한 승인 시에만 응답 큐를 가져오도록 하는 안정화다. 테스트 변경은 양방향 승인 완료 후 청크 중계를 멈추는 순서 수정이다. 로그·이벤트 XML·빌드 산출물·대용량 데이터와 인증 정보는 포함하지 않고 기존 `.gitignore`와 제외된 로컬 자료를 유지한다. main=`b87ff47`과 기존 커밋 이력을 보존하고 원격 push/병합/변경 폐기를 하지 않는다. 최종 검증 명령과 환경 요구 사항은 앞 절의 빌드 명령 및 RunStage5 명령을 그대로 사용한다.
