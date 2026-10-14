# 4단계: 대용량 스트리밍·백프레셔·진행률

## 설계와 호환성

기준은 [3단계 상태 머신](STAGE3.md), [추가 검증](STAGE3_VALIDATION.md), 커밋 `732fc14`다. GUI, LAN 검색, 자동 재접속, 이어받기는 구현하지 않았다. 실제 두 Windows PC의 LAN/Wi-Fi 검증도 하지 않았다. 아래 실전송은 한 PC의 실제 프로세스와 localhost TCP 소켓을 사용한다.

- 파일 상한: 기존 100MiB에서 **1TiB = 1099511627776바이트**로 확장했다. 크기·전송량·수신량·디스크 공간 비교는 uint64_t다. 청크 길이만 최대 64KiB 또는 수신 프레임 상한 1MiB 안에서 DWORD/ULONG/streamsize로 변환한다.
- **wire 형식은 변경하지 않았다.** 기존 5바이트 헤더, 64비트 requestId, 64비트 파일 크기, 메시지 타입·메타데이터 구성을 유지한다. 기능 수준을 구분하도록 `--version`의 로컬 프로토콜 표시는 4로 올렸다.
- 이전 버전은 100MiB를 초과하는 요청을 거부한다. 양쪽 모두 새 실행 파일을 사용하고 `--version` 및 exe SHA-256을 사전 비교해야 한다. 자동 버전 협상은 추가하지 않았다. 1~3단계 문서의 상한·버전은 당시 기록이다.
- 기존 테스트 항목을 삭제하지 않았다. stage2의 상한 초과 입력을 1TiB+1로, 버전 확인을 4로 갱신했다. 100MiB 전송은 기존 추가 검증에서 계속 실행한다.

## 기존 구조 분석과 변경된 데이터 흐름

기존 송신은 ifstream → 64KiB 배열 → requestId 문자열 복사 → 프레임 vector 복사 → send 루프였다. 수신은 1KiB recv → vector 누적/앞부분 erase → 동기 WriteFile이었다. 파일 전체 적재나 파일 데이터 작업 큐는 원래도 없었지만, 청크별 복사·할당과 수신 파싱 버퍼 용량 증가의 상한을 더 명확하게 만들 필요가 있었다.

현재 흐름:

```text
송신 작업 스레드
  승인 대기 → ifstream.read(고정 64KiB)
  → 스택의 헤더+requestId 13바이트와 기존 청크를 WSABUF 2개로 송신
  → 완료된 바이트만 uint64_t 누적 → 다음 청크
  → FILE_END → 동일 requestId의 저장 완료 ACK 대기

수신 작업 스레드
  recv(고정 64KiB) → 최대 한 프레임만 보관하는 Protocol 파서
  → 상태·requestId·크기 검증 → 동기 WriteFile 반복
  → 실제 기록한 바이트만 uint64_t 누적
  → FILE_END 크기 확인 → FlushFileBuffers → 덮어쓰기 없는 핸들 rename
  → 해당 requestId의 ACK
```

`Network`가 I/O와 대기를, `Protocol`이 상태 검증과 안전 저장을 유지한다. 빈 클래스였던 `FileTransfer`에는 방향별 진행률 계산·출력만 맡겼다. 파일 핸들·상태 머신 책임을 대규모로 이동하지 않았다.

## 버퍼 구조와 백프레셔

| 항목 | 크기 / 규칙 |
|---|---|
| 송신 파일 읽기 배열 | 65536바이트, 한 작업에서 재사용 |
| 송신 프레임 prefix | 스택 13바이트(일반 메시지는 5바이트) |
| WSABUF | 최대 2개, 헤더와 원본 청크를 참조; 대기 완료까지 유효 |
| Network recv 배열 | 65536바이트, 수신 스레드에서 재사용 |
| Protocol 파싱 데이터 | **최대 1048581바이트 = 1MiB+헤더 5바이트**, 한 프레임 |
| 파싱 vector 예약 | 위 상한을 한 번 reserve, 완료 후 clear·재사용; 전체 입력이나 전체 파일을 붙이지 않음 |
| 진행률 | 방향별 계산기와 수신 최신 snapshot 한 개; 무제한 로그/이벤트 큐 없음 |
| Winsock send/receive 버퍼 | SO_SNDBUF/SO_RCVBUF 각각 64KiB 요청 |

양방향 동시 파일 데이터에 필요한 앱의 주요 버퍼는 약 **1.125MiB + 18바이트**다(읽기 배열+recv 배열+파서+prefix). 객체·파일명·stdio·allocator·스레드 스택·DLL·로그 출력 등의 오버헤드는 별도다. 위 수치는 프로세스 전체 메모리 한도가 아니며 파일 데이터의 명시적 버퍼 크기다. OS TCP 내부 구조·파일 시스템 캐시도 이 수치에 포함하지 않는다. socket 옵션은 버퍼 요청값이며 OS 전체 커널 메모리 사용량의 엄밀한 상한을 보장한다는 뜻은 아니다.

수신은 디스크 쓰기가 끝나야 다음 recv를 한다. 느린 디스크/수신자는 TCP 윈도와 고정 socket 버퍼를 통해 송신을 막는다. 송신은 한 청크의 동기 WSASend가 끝나야 다음 파일 데이터를 읽으므로 앱 큐에 파일 데이터가 쌓이지 않는다. 별도 생산자·소비자 큐나 busy waiting을 추가하지 않았다.

`WSASend`에 overlapped/completion 인자를 주지 않아 기존 send와 같은 blocking semantics를 사용한다. 부분 전송이 반환되면 헤더 및 본문의 남은 구간을 다시 계산해 재전송한다. 한 프레임의 송신 전체는 sendMutex로 보호하며 **소켓 I/O 중 protocolMutex를 잡지 않는다.** 실패/0바이트 송신은 기존 연결 종료 경로로 처리한다. 상세 동작은 [Microsoft WSASend 문서](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend)를 참고한다.

- 기존 SO_SNDTIMEO 2초, recv 폴링 250ms, 승인 60초, 수신 무진행/ACK 대기 15초를 유지한다.
- 충분히 진행 중인 느린 전송에는 전체 파일 전송시간 제한을 두지 않는다. 매 송신이 정체되면 socket 타임아웃, 수신 무진행이면 요청 타임아웃으로 종료/정리한다.
- WriteFile/FlushFileBuffers 자체는 동기 API다. 장치/드라이버가 I/O 완료를 무기한 반환하지 않는 상황까지 이번에 deadline/cancel 가능한 overlapped 디스크 I/O로 바꾸지는 않았다. 이 제한을 TCP 타임아웃으로 해결했다고 주장하지 않는다.

## 파일 I/O·오류·상태 머신

파일 크기와 누적량은 64비트이며, 수신 크기 비교는 `payload.size() > expectedSize - receivedSize`처럼 뺄셈 전 범위를 확인해 합산 오버플로를 피한다. 실제 4GiB 초과 전송과 최종 바이트 수·SHA-256을 확인한다.

안전 저장 규칙을 유지한다:

1. 사용자 승인 및 동일 ID·메타데이터 확인 후에만 저장 폴더를 연다.
2. 폴더 핸들 고정과 reparse point 거부, 기존 최종 파일 보존, CREATE_NEW로 소유한 .part만 생성한다.
3. 생성 전에 [GetDiskFreeSpaceExW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespaceexw)의 사용자 가용 공간을 64비트로 확인한다. 공간 부족·조회 실패는 안전하게 거부한다. 다른 프로세스의 소비/할당 제한 때문에 사전 검사는 예약이나 쓰기 성공 보장이 아니다.
4. WriteFile의 성공 부분 쓰기를 반복하며 실제 기록량만 누적한다. 실패·0바이트 쓰기·크기 불일치 시 요청 오류 및 소유한 임시 파일 정리로 간다.
5. 원본 읽기 부족/실패 시 FILE_ERROR를 통지하고 성공을 반환하지 않는다.
6. FILE_END 후 정확한 크기·flush·덮어쓰기 없는 rename이 모두 성공한 뒤에만 ACK를 만든다. 오류 시 기존 파일이나 소유하지 않은 .part는 지우지 않는다.

송신·수신 독립 상태 머신, requestId 홀짝 영역, 승인/거절, 잘못된 전환 거부, 오래된 응답 무시, 타임아웃·연결 종료 정리는 유지했다. **데이터 100% 송신은 성공이 아니다.** END 송신이 성공하고 해당 ID의 저장 완료 ACK가 확인돼야 SendFile이 true를 반환한다. ACK 유실로 송신 실패하더라도 이미 커밋된 수신 파일은 보존한다.

## 진행률 출력

예시 형식:

```text
Sending: file=large.bin requestId=2 bytes=4295032832/4295032832 progress=100.0% rate=... MiB/s elapsed=...s [data sent; ACK required]
Saving: file=large.bin requestId=2 bytes=4295032832/4295032832 progress=100.0% rate=... MiB/s elapsed=...s [data written; flush/commit required]
[Upload finished, waiting for ACK] large.bin
[Transfer completed] large.bin
```

- Sending은 TCP 송신 호출이 소비한 **파일 데이터 바이트**, Saving은 WriteFile이 기록한 **파일 데이터 바이트**다. 네트워크 전달 보증 또는 flush 완료를 앞서 주장하지 않는다.
- 파일명·requestId·전체/완료 바이트·진행률·최근 보고 구간의 MiB/s·경과 시간을 표시한다. 헤더 바이트와 승인 대기시간은 데이터 속도에 포함하지 않는다.
- 평상시 500ms 간격, 시작과 끝은 강제 snapshot이다. 매우 작은 파일도 최종 정보를 표시하고 0바이트는 0/0, 100%, 속도 0으로 표시한다.
- 불완전한 바이트 수의 퍼센트가 반올림돼 100%로 표시되지 않게 제한한다. double 변환 후 비율을 계산해 uint64_t 곱셈 오버플로를 피한다.
- 수신은 상태 잠금 안에서 최신 snapshot 하나만 회수하고, 출력은 잠금 밖에서 한다. 송신도 상태 잠금 밖에서 출력한다. 새 비동기 로그 큐는 없다. osyncstream으로 진행률 메시지를 묶으며 기존 다른 메시지의 전체 출력 순서를 보장하는 것은 아니다.
- 콘솔/리다이렉션 소비자가 출력 자체를 막으면 동기 stdout이 지연될 수 있다. 출력 주기를 제한했지만 막힌 출력 장치까지 비동기 무제한 큐로 우회하지 않는다.

## 자동 검증

기존 stage1=12, stage2=20, stage3=27, stage3_validation=16을 구성별 75개 그대로 실행한다. 0바이트·작은 파일·100MiB·연결 종료·ACK 지연/유실·이전 ID·정체된 송신 종료·단편/병합 프레임은 해당 스위트를 재사용한다.

stage4의 추가 19개 시나리오:

| 범주 | 항목 수 | 검증 |
|---|---:|---|
| 경계값 시뮬레이션 | 4 | 2^32-1, 2^32, 2^32+1, 1TiB 메타데이터 디코딩·승인 요청·거절 |
| 상한 거부 | 2 | 1TiB+1, UINT64_MAX를 파일/폴더 생성 없이 거부 |
| 짧은 대용량 선언 | 1 | 4GiB+1 선언에 1바이트만 보내고 END → ACK 없이 .part 정리 |
| 실제 대용량 TCP | 2 | 1GiB+64KiB, 4GiB+64KiB 전체 전송·ACK·SHA-256·최종 64비트 진행률 |
| 메모리 크기 비교 | 1 | 두 전송 크기의 실제 프로세스 private bytes 비교 |
| 동시 전송 | 1 | 양방향 각각 256MiB, ACK와 SHA-256 |
| 0바이트 진행률 | 1 | 데이터 100% 후 ACK를 보류하면 성공하지 않고 해제 후 성공 |
| 느린 수신자 | 1 | 8MiB/청크별 중계 20ms 지연, 2초 이상 진행·완료·메모리 제한 |
| 반복 전송 | 1 | 같은 프로세스·연결에서 12회, 워밍업 후 메모리·핸들 수 비교 |
| 테스트 전용 I/O 주입 | 5 | 읽기 실패, 쓰기 실패, 가용 공간 부족, flush 실패, 97바이트 소켓 송신/17바이트 파일 쓰기 분할 |

경계값 시뮬레이션을 해당 크기 전체 전송 성공으로 세지 않는다. 실제 대용량 원본은 NTFS 희소 파일로 준비하며 0/중간/4GiB 전후/끝에 비영 데이터 마커를 넣는다. 송신 앱은 희소 구간을 특별 취급하지 않고 **모든 논리 바이트를 읽고 TCP로 전송**한다. 수신 파일은 일반 파일이며 전송 전 원본 해시, 전송 후 원본 불변, 저장 결과 SHA-256을 비교한다. 테스트 완료 후 검증된 큰 원본·수신 파일만 테스트 작업 폴더 내부 경로를 확인해 삭제한다. 로그와 측정 JSON은 남긴다. 희소 파일 동작은 [Microsoft FSCTL_SET_SPARSE 문서](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_sparse)를 참고한다.

장애 주입은 별도 `FILETRANSFER_TESTING` 빌드에만 포함한다. 운영 빌드는 `FTA_TEST_*` 환경 변수를 읽지 않는다. 테스트 빌드에서 실제 앱·TCP·파일 핸들·기존 정리 경로를 사용하되 쓰기/flush 실패 반환과 가용 공간 부족을 합성한다. 실제 디스크를 채우거나 ACL/보안 정책을 바꾸지 않는다. 실제 WriteFile 부분 성공 반복과 소켓 프레임 분할은 정상 파일/실제 소켓으로 수행한다. OS가 자연적으로 부분 송신을 반환한 횟수를 측정했다는 뜻은 아니다.

실행 방법(프로젝트 루트, 순차 실행):

```powershell
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
foreach ($config in @('Release','Debug')) {
    & $msbuild FileTransferApp.slnx /p:Configuration=$config /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw 'Production build failed' }
    $faultRoot = Join-Path $PWD "tests/runs/stage4-fault-$config"
    & $msbuild FileTransferApp/FileTransferApp.vcxproj /p:Configuration=$config /p:Platform=x64 `
        "/p:ForceImportBeforeCppTargets=$PWD/tests/Stage4Faults.props" `
        "/p:OutDir=$faultRoot/bin/" "/p:IntDir=$faultRoot/obj/" /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw 'Test-only build failed' }
    foreach ($suite in @('stage1','stage2','stage3','stage3_validation')) {
        powershell -NoProfile -ExecutionPolicy RemoteSigned -File "tests/$suite.ps1" -Exe "x64/$config/FileTransferApp.exe"
        if ($LASTEXITCODE -ne 0) { throw "$config $suite failed" }
    }
    powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage4.ps1 `
        -Exe "x64/$config/FileTransferApp.exe" -FaultExe "$faultRoot/bin/FileTransferApp.exe"
    if ($LASTEXITCODE -ne 0) { throw "$config stage4 failed" }
}
```

프로세스 범위 RemoteSigned는 이전에 사용자가 허용한 범위만 사용했다. Windows 보안 정책·Smart App Control·방화벽은 변경하지 않았다. .gitignore도 유지하며 테스트 데이터·로그·빌드 산출물은 커밋하지 않는다.

## 메모리·성능 측정 방법 및 결과

환경: Windows 11 Pro 10.0.26200(build 26200), Intel Core Ultra 5 245K(14코어/14논리 프로세서), NTFS, Visual Studio Community 2026/v145/C++20/x64. 테스트 시작 시 가용 공간은 약 885GB였다. 실제 대용량은 두 구성 모두 1073807360바이트 및 4295032832바이트 전체를 전송했다. 1TiB 전체 전송은 하지 않았다.

### 최종 결과 (2026-10-14)

| 항목 | Release x64 | Debug x64 |
|---|---|---|
| 운영 빌드 | PASS, 오류·경고 없음 | PASS, 오류·경고 없음 |
| 별도 장애 주입 빌드 | PASS, 오류·경고 없음 | PASS, 오류·경고 없음 |
| 기존 stage1 / stage2 / stage3 / 추가 검증 | 12+20+27+16 = 75 PASS | 12+20+27+16 = 75 PASS |
| 신규 stage4 | 19 PASS | 19 PASS |
| 합계 | 94 PASS | 94 PASS |

**기존 150개 + 신규 38개 = 총 188개 PASS.** 경계값 시뮬레이션, 반복 횟수, 실제 대용량 전송을 중복 계수하지 않았다. 실제 두 PC 실환경은 미검증이며 실제 디스크를 채운 시험은 아니다. 초기 PowerShell 구문 오류와 helper 모드 스위치 덮어쓰기 문제를 수정했다. 스위트별 PASS 개수까지 확인해 건너뛴 스위트를 통과로 세지 않았고, 수정 후 최종 결과를 완성했다. 미해결 테스트 실패는 없다.

앱별 관측 peak, MiB 단위:

| 구성 | 실제 바이트 | 요청→완료 초 | 평균 MiB/s | 송신 private | 수신 private | 송신/수신 working set | 송신/수신 peak 핸들 | 송신 진행률 줄 수 |
|---|---:|---:|---:|---:|---:|---|---|---:|
| Release | 1073807360 | 1.088 | 941.6 | 2.22 | 2.13 | 6.19 / 6.25 | 101 / 99 | 3 |
| Release | 4295032832 | 4.726 | 866.8 | 2.22 | 2.13 | 6.21 / 6.25 | 101 / 99 | 11 |
| Debug | 1073807360 | 1.102 | 929.3 | 2.73 | 2.63 | 8.18 / 8.09 | 101 / 99 | 3 |
| Debug | 4295032832 | 4.858 | 843.1 | 2.72 | 2.63 | 8.20 / 8.09 | 101 / 99 | 11 |

느린 수신자(중계 지연, 8MiB):

| 구성 | 완료 초 | 평균 MiB/s | 송신 peak private MiB |
|---|---:|---:|---:|
| Release | 2.763 | 2.89 | 2.23 |
| Debug | 2.753 | 2.91 | 2.72 |

12회 반복 전송(처음 2회 워밍업 후 비교, 방향 0=송신 앱/1=수신 앱):

| 구성 | 방향 | private MiB 전→후 | 핸들 전→후 |
|---|---:|---|---|
| Release | 0 | 2.12 → 2.12 | 92 → 92 |
| Release | 1 | 2.13 → 2.13 | 91 → 91 |
| Debug | 0 | 2.62 → 2.62 | 92 → 92 |
| Debug | 1 | 2.63 → 2.63 | 91 → 91 |

1GiB에서 4GiB로 늘려도 관측 private bytes는 거의 일정했다. 송신 정체·느린 수신자·동시 송신·반복 전송에서 파일 전체에 비례하는 앱 메모리 증가는 관측하지 않았다. 짧은 실행의 측정이므로 모든 환경/장기 운영의 누수 부재를 증명하는 것은 아니다.

로컬 상세 기록(기존 .gitignore에 따라 Git 미포함):

- 최종 스위트 명세와 stdout 로그: `tests/runs/stage4-suite-57ad09bd7d9741619025997ddcf9f30d/results-final.json` 및 같은 폴더의 로그. 운영/장애 주입 exe SHA-256도 기록했다. `results.json`은 helper 오류를 포함한 최초 실행 기록이다.
- 최종 Release 신규 검증/측정: `tests/runs/cec8ae2c582f4243acb5f7c021b7355a/measurements.json`.
- 최종 Debug 신규 검증/측정: `tests/runs/573d3c4afe5748bca837de3c841b15b2/measurements.json`.
- 큰 테스트 파일은 SHA-256 비교 후 테스트 폴더 안의 지정 파일만 정리했다. 신규 오류 주입용 exe를 운영 배포에 사용하지 않는다.

실제 앱 프로세스를 20ms 간격으로 샘플링해 private bytes, working set, 핸들 수의 관측 최댓값을 기록한다. 중계/PowerShell/해시 계산 프로세스 메모리는 앱 메모리에 포함하지 않는다. 전송시간은 전송 전 SHA-256 계산을 제외한 파일 요청부터 완료 ACK 및 테스트의 50ms 안정화 대기까지다. 후속 해시 검증 시간은 제외한다. 평균 MiB/s는 파일 바이트/관측 시간이며 순수 소켓 대역폭 측정이 아니다. Direct TCP 대용량 측정은 중계 없는 앱 간 통신이다. 희소 원본·로컬 파일 캐시·같은 PC NTFS 저장장치의 영향을 받으므로 **실제 LAN 처리량으로 해석하지 않는다.** 20ms 관측으로 그 사이의 모든 순간 피크나 장기 누수 부재를 증명하지는 않는다.

검사 한계는 앱별 private bytes <256MiB, 1GiB와 4GiB의 peak 차이 <32MiB, 반복 전송 워밍업 후 증가 <8MiB 및 핸들 증가 ≤3개다. 실제 관측값도 별도로 기록한다. 이 한계는 테스트용 안정성 판정값이며 설계의 명시적 파일 버퍼 상한을 대신하지 않는다.

## 알려진 제한과 후속 설계 고려

- 두 PC 실환경·장시간 대용량 운영·실제 디스크 용량 소진·장치 장애·ACL 변경·OS가 자연스럽게 반환하는 I/O 실패는 별도 검증이 필요하다. 이번 장애 주입을 실제 장치 장애 재현이라고 주장하지 않는다.
- 1TiB는 설계/메타데이터 검사 상한이며 전체 1TiB 전송은 수행하지 않는다. 파일 시스템·쿼터·남은 공간이 실제 저장 가능 크기를 제한한다. FAT32의 큰 파일 저장 제약을 해소하는 기능도 아니다.
- 읽는 도중 원본이 같은 크기로 변경되면 내부 해시가 없어 내용 변경을 프로토콜에서 감지하지 못할 수 있다. 테스트는 전후 외부 SHA-256을 사용한다. 파일 인증/내부 해시/송신 원본 불변성 잠금은 후속 고려 사항이다.
- 수신 프로세스 강제 종료/전원 차단으로 남은 .part 자동 회수는 없다. 기존 .part를 임의 삭제하지 않는 규칙을 보존한다.
- 향후 요청별 취소는 requestId·방향·현재 상태를 검증하는 명시적 취소 이벤트와 대기자 통지, 소유한 핸들의 정리, 이미 커밋한 파일의 보존 규칙을 정의해야 한다. 기존 전체 연결 종료와 요청 취소를 혼동하지 않아야 한다.
- 이어받기는 새로운 요청과 기존 임시 파일의 소유권·오프셋·전체 크기·원본 동일성 및 해시 검증이 필요하다. 임의 경로의 기존 .part를 신뢰해 재사용하면 안 된다. offset/ACK 범위를 포함한 새 프로토콜 설계가 선행돼야 하며 이번 단계에서는 구현하지 않았다.
