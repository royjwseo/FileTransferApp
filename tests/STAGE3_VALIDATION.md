# 3단계 실환경 검증 준비 및 안정성 보강

## 검증 범위와 현재 결론

**서로 다른 두 Windows PC의 LAN/Wi-Fi 검증은 미실행이다.** 이 작업 환경에서는 두 번째 PC에 접근할 수 없다. 아래 자동 테스트는 한 PC의 localhost에서 실제 FileTransferApp 프로세스 두 개 또는 raw TCP peer를 실행한 결과이며, 실환경 통과를 의미하지 않는다. 두 PC 검증은 아래 절차에 따라 별도로 수행하고 결과를 기록해야 한다.

기준 설계는 [3단계](STAGE3.md), [2단계](STAGE2.md), [1단계 기록](README.md)이다. 저장소에는 `STAGE1.md`가 없으며 1단계 기록은 README에 있다.

- `Network`: Winsock 연결/프레임 송신/수신 루프/조건 변수 대기. 기존 shutdown → 송신·수신 join → closesocket 순서를 보존했다.
- `Protocol`: requestId 및 양방향 독립 상태 머신, 프레임 파싱, 승인 검증, 소유한 Windows 핸들로 안전 저장·정리. 상태/메시지 형식/타임아웃/파일 상한은 변경하지 않았다.
- `FileTransfer`: 빈 클래스. 기존 책임을 대규모로 이동하지 않았다.

## 변경 사항과 발견 내용

1. 명령행 실행을 추가했다: `--host`, `--bind <IPv4>`, `--connect <IPv4>`, `--port <1..65535>`, `--version`, `--help`. 인자가 없으면 기존 메뉴/포트 9000을 유지한다. 호스트 기본 bind는 기존과 같은 0.0.0.0이다. 명령행 모드에서 stdin 첫 명령을 버리지 않도록 메뉴 모드에서만 개행을 제거한다.
2. `Protocol::WIRE_VERSION = 3`과 로컬 시작 배너/`--version`을 추가했다. wire format 자체는 그대로다. **버전 확인은 수동 사전 비교이며 자동 핸드셰이크나 피어 인증은 아니다.** 양쪽에 동일 Release exe를 복사하고 버전 출력과 exe SHA-256을 모두 비교한다. 구형 실행 파일이 `--version`에서 메뉴를 표시하면 사용을 중단한다.
3. **재현된 문제:** 수신 루프의 정상 연결 종료 안내가 stdout 버퍼에 남아 리다이렉트된 로그에서 즉시 보이지 않았다. 해당 안내를 명시적으로 flush하도록 수정했다. 데이터 정리는 이미 수행됐지만 상태 안내를 기다리는 검증이 실패했다. 수정 후 종료·정리 테스트를 재실행했다.
4. **코드 검토에 따른 보강:** ACK를 이미 파싱해 Completed인데 곧바로 FIN/종료 취소가 도착하면, 기존 `WaitForState`의 취소 우선 분기 때문에 확인된 성공을 false로 반환할 수 있었다. END의 송신 성공 후 도달하는 ACK 대기에서는 Completed를 먼저 확인한다. 수정 전 ACK+FIN 20회 실행에서는 이 경합이 재현되지 않았으므로 실제 재현 장애라고 주장하지 않는다. 신규 테스트는 20회 프로세스 실행으로 ACK+FIN 후 성공 보존을 확인한다. ACK가 없거나 잘못된 ID/상태인 경우에는 성공하지 않는다.
5. Debug 반복 테스트에서는 성공 표시와 파일명 사이에 다른 스레드의 연결 종료 안내가 끼어드는 로그를 확인했다. 실제 성공인데 연속 문자열 조건으로 실패하던 검사를, 단일 요청의 완료 표시 존재와 실패 표시 부재로 수정했다. 출력 순서 자체를 보장하는 전면적인 로깅 변경은 하지 않았다.
6. 테스트 helper에 선택적 명령행 인자를 추가하고, 테스트 전용 `FrameRelay.cs` 및 `stage3_validation.ps1`을 추가했다. 기존 검사 항목은 삭제하지 않았다. 자동 테스트는 생성한 프로세스와 테스트 폴더만 사용한다.

## 두 PC 실행 준비

PC A를 서버, PC B를 클라이언트로 사용한다. 예시 A=192.168.0.10, B=192.168.0.11, TCP 포트=9100이며 **실제 LAN IPv4로 바꿔야 한다.** IPv6/DNS 이름은 이번 CLI에서 지원하지 않는다. 같은 IP 대역에서도 무선 AP의 클라이언트 격리/회사 정책 등에 따라 서로 접근하지 못할 수 있으므로 관리자 정책을 따른다.

Visual Studio에서 Release x64를 빌드한 `x64/Release/FileTransferApp.exe`를 두 PC의 `C:\Tools\FileTransferApp\FileTransferApp.exe`에 동일하게 복사한다. 현재 솔루션 출력은 루트의 `x64/Release`이다. 과거 `FileTransferApp/x64` 경로의 오래된 exe와 혼동하지 않는다. 대상 PC에 실행에 필요한 VC++ x64 런타임이 없다면 조직의 정상 배포 절차로 준비한다. Windows가 실행을 차단하면 차단 내용을 기록하고 보안 정책을 해제하거나 우회하지 않는다.

양쪽 PC에서 PowerShell로 확인한다:

```powershell
ipconfig
& 'C:\Tools\FileTransferApp\FileTransferApp.exe' --version
Get-FileHash -Algorithm SHA256 -LiteralPath 'C:\Tools\FileTransferApp\FileTransferApp.exe'
```

버전 출력은 양쪽 모두 다음과 같아야 하며 exe 해시도 동일해야 한다. 프로토콜 버전만 같으면 동일 빌드라는 뜻은 아니므로 두 값을 함께 기록한다.

```text
FileTransferApp protocol=3 requestId64-parity maxFileBytes=104857600
```

각 PC에서 별도 작업 폴더를 만든다. **`received`는 exe 위치가 아닌 현재 작업 폴더 아래에 생성된다.** 받은 파일은 덮어쓰지 않으므로 새 검증은 새 폴더 또는 새 파일명을 사용한다.

PC A:

```powershell
New-Item -ItemType Directory -Force 'C:\FileTransferValidation\A' | Out-Null
Set-Location 'C:\FileTransferValidation\A'
& 'C:\Tools\FileTransferApp\FileTransferApp.exe' --host --bind 192.168.0.10 --port 9100
```

PC B:

```powershell
New-Item -ItemType Directory -Force 'C:\FileTransferValidation\B' | Out-Null
Set-Location 'C:\FileTransferValidation\B'
& 'C:\Tools\FileTransferApp\FileTransferApp.exe' --connect 192.168.0.10 --port 9100
```

`Commands:` 이후 양쪽에서 텍스트를 주고받는다. 서버는 한 번에 한 연결만 수락하며 별도의 파일 데이터 포트를 열지 않는다. 한 TCP 연결 안에서 파일을 양방향으로 전송한다. 연결이 끊기면 양쪽에서 `/quit` 후 위 명령으로 **새 프로세스/세션을 실행해 재접속**한다. 자동 재접속이나 기존 프로세스 안의 reconnect 명령은 추가하지 않았다. TCP 연결 자체가 성립하지 않으면 `--bind` 주소가 해당 PC의 실제 주소인지, 주소·포트·방화벽·정책을 확인한다.

## Windows 방화벽: 문서로만 제공, 자동 적용하지 않음

실행 호스트의 방화벽/네트워크 프로필/보안 정책은 변경하지 않았다. 서버 PC A에 선택한 exe, TCP 포트, 클라이언트 IP, 사용 중인 승인된 네트워크 프로필에 맞는 인바운드 허용이 필요하다. 한 연결의 양방향 전송에는 클라이언트의 별도 수신 포트 개방이 필요 없다. 조직에서 아웃바운드를 제한하면 관리자에게 해당 연결 허용을 요청한다.

아래는 **기존 네트워크 프로필이 Private이고 관리자에게 허용받은 경우에만 사용자가 직접 적용할 예시**다. 명령을 실행하기 위해 프로필을 바꾸거나 방화벽 전체를 끄지 않는다. 실제 exe 경로/IP/포트를 반영하고 먼저 `-WhatIf` 결과를 검토한다. 프로필이 Domain/Public이거나 회사 정책이 적용되면 관리자에게 맞는 규칙을 요청한다.

```powershell
# PC A의 관리자 PowerShell에서 검토용. 이 작업에서는 실행하지 않았다.
$ruleArgs = @{
    Name = 'FileTransferApp-Validation-9100'
    DisplayName = 'FileTransferApp validation TCP 9100'
    Direction = 'Inbound'
    Action = 'Allow'
    Profile = 'Private'
    Program = 'C:\Tools\FileTransferApp\FileTransferApp.exe'
    Protocol = 'TCP'
    LocalAddress = '192.168.0.10'
    LocalPort = 9100
    RemoteAddress = '192.168.0.11'
}
New-NetFirewallRule @ruleArgs -WhatIf
# 사용자가 승인·검토한 경우에만 적용:
# New-NetFirewallRule @ruleArgs
# 이 테스트용 규칙을 직접 만들었다면 검증 종료 후 해당 규칙만 제거:
# Remove-NetFirewallRule -Name 'FileTransferApp-Validation-9100'
```

옵션은 [Microsoft New-NetFirewallRule 문서](https://learn.microsoft.com/en-us/powershell/module/netsecurity/new-netfirewallrule)를 기준으로 했다. 주소 확인은 [Microsoft ipconfig 문서](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/ipconfig)를 참고한다. `Test-NetConnection -ComputerName 192.168.0.10 -Port 9100` 같은 TCP 진단은 단일 accept를 소비하므로, 진단에 사용했다면 서버를 다시 실행한 뒤 실제 앱을 연결해야 한다.

## 두 PC 파일·SHA-256 검증 절차 (미실행)

앱과 별도 PowerShell 창을 각 PC에 열어 전송 원본을 준비한다. 예를 들어 PC A의 작업 폴더에서:

```powershell
Set-Location 'C:\FileTransferValidation\A'
New-Item -ItemType Directory -Force source | Out-Null
[IO.File]::WriteAllBytes((Join-Path $PWD 'source\A-empty.bin'), [byte[]]@())
[IO.File]::WriteAllText((Join-Path $PWD 'source\A-small.txt'), 'from PC A')
$largePath = Join-Path $PWD 'source\A-large.bin'
$fileStream = [IO.File]::Create($largePath)
try {
    [byte[]]$dataBlock = New-Object byte[] 1MB
    $randomData = [Random]::new(20261014)
    for ($i = 0; $i -lt 100; $i++) {
        $randomData.NextBytes($dataBlock)
        $fileStream.Write($dataBlock, 0, $dataBlock.Length)
    }
} finally { $fileStream.Dispose() }
Get-ChildItem source -File | Get-FileHash -Algorithm SHA256 |
    Export-Csv -NoTypeInformation -Encoding UTF8 'source-before.csv'
```

PC B에서도 폴더를 `B`, 파일명 접두사를 `B-`로 바꿔 만든다. 상한은 **100MiB = 104857600바이트**다. 100MiB 초과 파일은 기존 사양에 따라 송신을 거부하며 GB 단위 파일은 지원하지 않는다.

정상 전송:

1. PC A 앱에서 `/file "C:\FileTransferValidation\A\source\A-small.txt"`를 입력하고 PC B가 요청을 확인한 뒤 `/accept`한다.
2. PC B의 `[File saved]`와 PC A의 `[Transfer completed] A-small.txt`를 확인한다. END 송신 안내만으로 합격 처리하지 않는다.
3. PC A 원본과 PC B 저장 결과의 SHA-256을 구해 **전송 전에 기록한 해시와도 비교**한다. 원본 불변, 수신 크기·해시 일치, `.part` 없음 모두 확인한다.
4. 같은 절차로 B→A, 양쪽 0바이트 파일, 양쪽 100MiB 파일을 보낸다. 저장 파일명이 이미 존재한다면 새 검증 폴더나 새 이름을 사용한다.

```powershell
# PC A, 전송 후 원본
Get-FileHash -Algorithm SHA256 -LiteralPath 'C:\FileTransferValidation\A\source\A-small.txt'
# PC B, 전송 후 저장 결과
Get-FileHash -Algorithm SHA256 -LiteralPath 'C:\FileTransferValidation\B\received\A-small.txt'
Get-ChildItem -LiteralPath 'C:\FileTransferValidation\B\received' -Filter '*.part'
```

양방향 동시 전송:

- 양쪽 앱에서 각각 미사용 이름의 `/file ...`을 입력한 다음 양쪽에서 `/accept`한다.
- 양쪽 모두 자신이 보낸 파일의 `[Transfer completed]`를 표시하고, 양쪽 저장 결과의 SHA-256이 각 전송 전 원본 해시와 일치하는지 확인한다.

중단과 재접속:

- 미사용 이름의 100MiB 송신에서 수신 측 `.part`가 존재하는 동안 송신 측에 `/quit`을 입력하거나, 테스트 송신 앱만 작업 관리자로 종료한다. 이미 전송이 완료됐다면 중단 조건을 만족하지 않으므로 합격 처리하지 않고 재시도하거나 자동 중계 테스트를 사용한다.
- 실제 회선 순간 끊김은 테스트용 유선 연결의 케이블 분리·복구 등으로 별도 확인한다. Windows 보안·방화벽·업무 네트워크 설정을 비활성화하지 않는다.
- 수신 측이 미완료 최종 파일을 만들지 않고 소유한 `.part`가 연결 종료 감지 또는 15초 수신 무진행 타임아웃 후 사라지는지 확인한다. 실제 회선 단절 감지는 즉시 이뤄지지 않을 수 있다.
- 송신 측 종료 시 살아 있는 수신 앱의 `.part` 정리와 **수신 측 강제 종료 시 잔류**를 구분한다. 수신 프로세스 강제 종료·전원 차단으로 남은 `.part`는 자동 회수하지 않는다. 이번 송신 프로세스 종료 테스트로 해당 기능이 추가된 것은 아니다.
- 실패 후 양쪽 앱을 종료하고 새 연결을 만든 뒤 미사용 이름의 작은 파일로 저장·ACK·SHA-256 일치를 재확인한다.

저장 실패:

- 기존 수신 데이터가 없는 **새 검증용 작업 폴더**에 `received`라는 일반 파일을 만들어 수신한다. 송신 측이 성공이라고 표시하지 않고, 원래 블로커 파일을 바꾸지 않으며 `.part`가 남지 않는지 확인한다.
- 양쪽 앱 종료 후 직접 만든 블로커만 제거하고 재접속해 새 전송을 확인한다. 실제 디스크 부족·ACL 거부·flush 실패는 별도 환경 검증이며 이 경로 충돌 테스트와 동일시하지 않는다.

ACK 지연·유실과 이전 requestId 응답:

- TCP에는 재송신이 있으므로 패킷 손실과 애플리케이션 ACK 프레임 비배송은 같지 않다. 보안 설정을 변경해 수동으로 재현하지 않는다.
- 아래 자동 테스트의 localhost 중계가 **실제 수신 앱이 저장 후 전송한 ACK**를 보류·폐기하고 이전 ACK를 재송신한다. 실제 LAN의 지연·패킷 손실은 이번에 검증하지 않았다.
- ACK가 도착하지 않으면 송신 측은 15초 후 실패하며, 수신 측의 커밋된 최종 파일은 보존된다. 송신 실패만으로 수신 파일이 없다고 판단하지 말고 결과를 확인한 다음 새 파일명으로 재시도한다.

두 PC 검증 기록표(미기록은 미검증):

| 기록/항목 | PC A / 송신 측 | PC B / 수신 측 | 판정 |
|---|---|---|---|
| OS / exe SHA-256 / --version | 미기록 | 미기록 | 미검증 |
| IPv4 / 포트 / 네트워크 | 미기록 | 미기록 | 미검증 |
| A→B / B→A | 미실행 | 미실행 | 미검증 |
| 양방향 동시 | 미실행 | 미실행 | 미검증 |
| 0바이트 / 100MiB | 미실행 | 미실행 | 미검증 |
| 연결 종료 / 송신 프로세스 종료 / 재접속 | 미실행 | 미실행 | 미검증 |
| 저장 실패 / 복구 | 미실행 | 미실행 | 미검증 |
| 각 파일의 전송 전·후·수신 SHA-256 | 미기록 | 미기록 | 미검증 |

## 자동 테스트와 결과

기존 스위트는 중복 구현하지 않고 그대로 실행한다. `stage1` 12개는 종료·소켓 정체·부분 파일 정리, `stage2` 20개는 승인·안전 저장·저장 실패·0바이트/1MiB 수신, `stage3` 27개는 상태·ID·타임아웃·1MiB 동시 전송을 검증한다.

추가 `stage3_validation`의 **16개 시나리오 그룹**:

| 번호 | 추가 검증 |
|---|---|
| 1 | ACK 직후 FIN에도 성공 보존, 실제 프로세스 20회 반복 |
| 2 | 잘못된/누락/중복 CLI 인자와 잘못된 주소 14개 조합 거부 |
| 3 | --help / --version 출력 및 정상 종료 |
| 4 | 명시한 loopback bind·동적 포트로 양방향 텍스트, 첫 stdin 명령 보존 |
| 5 | 실제 앱 A→B 0바이트 저장·ACK·전후 SHA-256 |
| 6 | 실제 앱 B→A 상한 100MiB 저장·ACK·전후 SHA-256 |
| 7 | 실제 저장 완료 ACK를 중계에서 지연, 해제 후에만 성공 |
| 8 | ACK 비배송·타임아웃, 저장 결과 보존, 늦은/이전 ACK 재송 무시, 다음 요청 성공 |
| 9–14 | 전송 중 연결 절단/송신 프로세스 Kill/송신 /quit 각각 수신 .part 정리 및 같은 수신 작업 폴더의 새 세션에서 전송 복구 |
| 15 | 실제 수신 저장 경로 실패가 실제 송신자에게 실패로 전달되고 기존 파일 보존 |
| 16 | 저장 실패 원인 제거 후 새 연결의 저장·ACK·SHA-256 성공 |

FrameRelay는 테스트 전용 localhost TCP 중계이며 운영 앱에 링크되지 않는다. 첫 CHUNK를 보류해 실제 수신 `.part`가 만들어진 이후 장애를 주입하므로, 전송이 이미 완료된 뒤 Kill/종료하는 테스트가 되지 않는다. 송신 프로세스 Kill 후에는 중계를 재개해 TCP EOF/reset이 수신자에게 전달되게 하며, 살아 있는 수신 앱이 부분 파일을 정리하는지 확인한다. 중계와 테스트 프로세스는 종료 시 모두 정리한다.

자동 테스트를 프로젝트 루트에서 순차 실행한다(다른 스위트와 동시 실행하지 않음). 이 세션에서 이전에 허용한 **프로세스 범위 RemoteSigned**만 사용했으며 시스템/사용자 실행 정책, Smart App Control, 방화벽은 변경하지 않았다.

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' FileTransferApp.slnx /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' FileTransferApp.slnx /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
foreach ($config in @('Release','Debug')) {
    foreach ($suite in @('stage1','stage2','stage3','stage3_validation')) {
        powershell -NoProfile -ExecutionPolicy RemoteSigned -File "tests/$suite.ps1" -Exe "x64/$config/FileTransferApp.exe"
        if ($LASTEXITCODE -ne 0) { throw "$config $suite failed" }
    }
}
```

### 최종 자동 검증 결과 (2026-10-14)

| 항목 | Release x64 | Debug x64 |
|---|---|---|
| 빌드 | PASS, 오류·경고 없음 | PASS, 오류·경고 없음 |
| 기존 stage1 | 12 PASS | 12 PASS |
| 기존 stage2 | 20 PASS | 20 PASS |
| 기존 stage3 | 27 PASS | 27 PASS |
| 추가 stage3_validation | 16 PASS | 16 PASS |
| 합계 | 75 PASS | 75 PASS |
| 서로 다른 두 PC의 LAN/Wi-Fi | 미실행 | 미실행 |

기존 118개와 추가 32개 시나리오 그룹, **총 150개 PASS**다. 추가 스위트의 ACK+FIN 20회 반복과 CLI 잘못된 입력 14개 조합을 별도 테스트 수로 부풀리지 않았다. 위 결과는 최종 앱 빌드의 결과이며, 로그 끼어듦 검사 수정 후 추가 스위트를 두 구성에서 재실행해 16개씩 통과했다. 미해결 테스트 실패는 없다.

로컬 전체 회귀 로그 및 최종 명세:

- `tests/runs/validation-suite-c32bcc94a35e43e886fdb4caa19ad94a/`
- `results.json`은 최초 전체 실행 기록(수정 전 Debug 검사 실패 포함), `results-final.json`은 해결 후 최종 8개 스위트 결과다.
- 최종 추가 스위트 상세 프로세스 로그: Release `tests/runs/94e358ca559c42b48743ff27aff8eb3a/`, Debug `tests/runs/0983205a5e9f4d68902ca9af2e5f487b/`.
- 재접속 테스트의 각 세션 로그는 서로 덮어쓰지 않도록 세션 ID로 구분했다. 테스트 전 SHA-256, 전송 후 원본 불변, 저장 결과 해시를 비교한다.
- 로그/생성 파일/exe는 기존 .gitignore에 따라 커밋하지 않는다.

검증에 사용한 실행 파일 SHA-256 (같은 빌드를 복사했는지 비교할 때 사용):

| 구성 | SHA-256 |
|---|---|
| Release | `D7B49DC8B6290FA8BC58EF2E2D082396F348716CBC0CCF806BFFA04C18862C91` |
| Debug | `6FCE8FA079D5B7E4360A60392F35D5A269FB18401BE058AF4B4F34E5848434C3` |

재빌드하면 바이너리 해시는 달라질 수 있으므로 새로 배포하는 실행 파일의 해시를 다시 구해 두 PC 사이에서 비교한다.

## 알려진 제한

- 실제 LAN/Wi-Fi, PC별 저장장치·보안 정책·장시간 운영·실제 패킷 손실은 미검증이다. 중계 시뮬레이션을 실제 네트워크 품질 측정으로 해석하지 않는다.
- 저장 실패는 기존 파일/폴더 충돌을 재현했다. 디스크 용량 소진, WriteFile/FlushFileBuffers 실패, 정리 API 실패, ACL 변경은 강제 주입하지 않았다.
- 수신 프로세스 강제 종료/전원 차단에서 파일 핸들의 정상 정리 경로가 보장되지 않으며, 남은 .part의 자동 복구/삭제·이어받기 기능은 없다. 기존 .part를 임의로 지우지 않는 안전 규칙을 유지한다.
- 로그/종료/후속 정상 전송 검증을 수행했지만 장시간 핸들 수 측정이나 누수 분석 도구로 리소스 누수 부재를 증명한 것은 아니다.
- 파일명은 기존의 안전한 ASCII 제한, 파일당 상한 100MiB, 승인 대기 60초, 수신 무진행/ACK 대기 15초, 소켓 송신 정체 2초를 유지한다. 느린 실LAN에서의 적정성은 별도 측정 대상이다.
- SHA-256은 테스트·수동 검증의 외부 비교다. 프로토콜 내부 해시, 암호화, 인증, 자동 버전 협상은 추가하지 않았다.
