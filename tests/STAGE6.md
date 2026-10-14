# 6단계: UTF-8/Unicode 파일명

기준은 `feature/stage5-recovery`의 `a87911d`다. 별도 `feature/stage6-unicode`에서 작업하며 기존 브랜치·커밋과 main=`b87ff47`을 보존한다. 작업 결과는 로컬 커밋만 만들며 main 병합과 원격 push를 하지 않는다. 5단계 원본 빌드의 최종 검증이 SAC 차단으로 미완료였다는 [기존 기록](STAGE5.md)은 그대로 유지한다. 이번 결과는 새 protocol=6 빌드에 대한 결과이며 과거 실행 차단을 소급해서 통과로 변경하지 않는다.

## 기존 구조와 변경 범위

기존 Protocol은 영숫자/점/밑줄/하이픈만 허용했고 파일명은 최대 ASCII 100바이트였다. Network의 narrow filesystem::path 및 filename().string(), 저장 경로의 narrow 이름 결합은 Windows 지역 코드페이지에 의존했다. 이 경계를 명시적인 UTF-8↔UTF-16 변환으로 대체했다.

- `include/FileName.h`: 엄격한 변환·공통 파일명 검증 선언과 길이 상한.
- `src/FileTransfer.cpp`: MultiByteToWideChar/WideCharToMultiByte 변환 및 검증 구현, 메타데이터 바이트 상한 확장, Windows 저장 경로를 UTF-16으로 결합.
- `src/Network.cpp`: `/file`, `/resume`, `/status` 경로를 UTF-8에서 UTF-16으로 변환하여 원본을 열고, 실제 basename을 UTF-8로 메타데이터에 넣는다.
- `src/Protocol.cpp`: 공통 검증을 사용한다. 수신 메타데이터의 이름은 디스크 변경 전에 검증한다.
- `src/main.cpp`: 실제 콘솔일 때만 입력/출력 코드페이지를 UTF-8로 설정하며 종료 시 복원한다. 리디렉션 스트림은 UTF-8 계약을 사용한다.
- 프로젝트 파일에 FileName.h를 등록했다. 테스트 TestApp의 stdin 쓰기와 stdout/stderr 읽기도 UTF-8로 명시했다. 버전 회귀 기대값은 6으로 변경했다.
- PowerShell 5.1의 RunStage5 최종 집계에서 Hashtable의 `pass`를 Measure-Object 속성으로 직접 읽지 못하는 문제를 발견해 숫자 배열을 먼저 추출하도록 한 줄 수정했다. 테스트 동작이나 실패 판정을 완화하지 않았다.

Network의 소켓/스레드 소유, Protocol의 상태/승인/requestId, FileTransfer의 진행률/스트리밍 저장 역할은 유지했다. 5단계 controlMutex, 취소/재접속/세대 검증, DPAPI manifest/file identity, SHA-256 및 저장 완료 ACK 순서를 재작성하지 않았다. GUI·LAN 검색은 추가하지 않았다.

## 전송 인코딩과 호환성

파일명 필드는 **BOM 없는 엄격한 UTF-8 바이트**다. Windows 파일 접근은 UTF-16과 기존 W API를 사용한다. UTF-8 입력은 MB_ERR_INVALID_CHARS, UTF-16 출력 변환은 WC_ERR_INVALID_CHARS를 사용하여 대체 문자로 조용히 변환하지 않는다. unpaired surrogate, overlong UTF-8, 잘린 멀티바이트와 U+10FFFF 범위 밖 입력은 거부한다.

5단계 프레임/메시지 번호/requestId/metadata layout은 변경하지 않았다. REQUEST/BEGIN의 이름만 UTF-8로 정의하며 UTF-8 크기는 기존 고정 메타데이터 137바이트 이후의 실제 바이트 수다. protocol 표시를 **6**으로 올렸다. protocol=5는 Unicode/공백 이름을 거부하므로 양쪽 모두 6 사용을 권장한다. 자동 버전 협상은 없다. 기존 ASCII 이름의 바이트·메타데이터·hash·transferId는 유지한다.

transferId는 기존 `FTA5\0` namespace, uint64 크기, digest 및 **정확한 파일명 바이트**를 계속 사용한다. 버전 표시가 6이라고 namespace를 바꾸지 않는다. Unicode 이름 역시 SHA-256/소유 manifest에 결합되어 다른 이름의 파일에 잘못 이어붙이지 않는다. `/status`, `/resume`에도 같은 UTF-8 경로 변환이 적용된다.

콘솔 수동 사용:

```text
/file "C:\사진 모음\여행 사진.jpg"
/resume <transferId> "C:\사진 모음\여행 사진.jpg"
/status <transferId> "C:\사진 모음\여행 사진.jpg"
```

리디렉션하는 호출자는 stdin을 UTF-8로 보내고 stdout/stderr를 UTF-8로 읽어야 한다. PowerShell/.NET 테스트 helper도 이 규칙을 따른다. 로컬 sourcePath에는 절대 경로가 가능하지만 **원격 파일명은 basename 한 요소만** 허용한다.

## 파일명 보안·길이·정규화 정책

한글·라틴 문자·숫자·내부 공백·일반 문장부호·CJK·이모지·결합 문자를 허용한다. 다음은 거부한다:

- 빈 이름, `.`으로 시작하는 이름, 선행/후행 ASCII 공백, 후행 점.
- `/`, `\`, `:`, `<`, `>`, `"`, `|`, `?`, `*`. 절대경로, 드라이브/UNC/디바이스 경로, 경로 탐색 및 alternate data stream이 이 검증에서 차단된다.
- NUL, C0/C1 제어 문자, Unicode 줄/문단 구분자, BOM(U+FEFF), bidi embedding/override/isolate 제어 문자. 숨겨진 표시 제어로 로그가 혼동되는 경우를 줄인다. 모든 시각적 동형 문자까지 차단하는 정책은 아니다.
- 대소문자와 확장자에 관계없는 CON/PRN/AUX/NUL/COM1–9/LPT1–9, Windows에서 예약된 COM¹/²/³ 및 LPT¹/²/³. CONIN$/CONOUT$/CLOCK$도 보수적으로 거부한다. 첫 점 앞 base의 trailing space를 제거해 `con .txt` 우회도 막는다.
- **UTF-8 400바이트 또는 UTF-16 코드 단위 100개 초과.** 보조 평면 문자(이모지 등)는 UTF-16에서 2개로 센다. 100개 이름의 승인 경계는 실제 파일 생성 시험과 구분한다.

정규화는 **하지 않는다**. NFC/NFD/NFKC로 원본 이름을 바꾸거나 trim/치환하지 않는다. NFC `café.txt`와 NFD `café.txt`는 서로 다른 UTF-8/transferId를 가지며 지원하는 파일 시스템에서는 별개로 저장한다. 대소문자 및 이름 충돌은 파일 시스템 동등성에 따라 기존 CREATE_NEW/독점 핸들/no-replace rename이 안전하게 거부한다. 자동 번호 붙이기나 기존 파일 덮어쓰기는 하지 않는다.

기존 `.part`와 `.part.fta`/게시용 `.new` 접미사는 유지한다. 임시/manifest는 owned handle로만 정리하며 알 수 없는 기존 파일을 삭제하지 않는다. 정상 저장은 전체 hash 검증→flush→no-replace rename→ACK 순서를 유지한다.

전체 경로가 Windows/런타임 경로 길이 제한을 초과하면 안전하게 실패할 수 있다. 이 단계에서 OS의 long-path 정책을 변경하거나 `\\?\` namespace를 도입하지 않았다. 최대 component 길이가 허용되어도 모든 깊은 작업 디렉터리에서 생성 가능함을 뜻하지 않는다.

공식 근거: [Windows 파일명/예약 이름](https://learn.microsoft.com/en-us/windows/desktop/fileio/naming-a-file), [엄격한 UTF-8 변환](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar).

## 테스트와 실행 환경

Windows x64/MSVC C++20/Windows SDK/로컬 NTFS. 테스트는 한 PC의 실제 앱 프로세스와 localhost TCP, 일부 raw TCP peer를 사용한다. 한글 PDF/JPG 등은 해당 이름의 바이트 fixture이며 실제 문서 렌더링/이미지 디코딩을 검사하는 테스트가 아니다. 파일 내용은 SHA-256으로 비교한다. 두 PC LAN 및 수동 콘솔 IME/폰트 표시 시험은 미실행이다.

`tests/stage6_unicode.ps1` 구성당 43개:

| 범위 | 개수 | 내용 |
| --- | ---: | --- |
| 실제 양방향 파일 전송/소유권/완료 조회 | 13 | 한글 PDF/JPG, 영문·한글 혼합, 공백/문장부호, accent/CJK/이모지, NFC/NFD 보존, ASCII, 같은 이름 다른 내용 및 unowned part 충돌, Unicode /status 및 완료 /resume |
| raw TCP 거부 | 27 | 잘못된 UTF-8 5종, NUL, 절대/상대/UNC 경로, 예약 이름·윗첨자, ADS, 제어/표시 문자, 101 UTF-16 unit |
| 승인 단계 길이 경계 | 2 | 한글 100 UTF-16 unit와 이모지 50개. 디스크 생성/실전송이 아님 |
| 실제 대용량 | 1 | Unicode 이름 1GiB+64KiB 스트리밍·저장 ACK·SHA-256 |

표준 Debug/Release x64 및 FILETRANSFER_TESTING 전용 Debug/Release 빌드가 성공했다. 새 코드로 다음 결과를 확보했다:

| 항목 | Debug | Release |
| --- | --- | --- |
| Unicode 43개 | 통과 | 통과 |
| 기존 1~4단계 94개 | 통과 | 통과 |
| 5단계 27개 | 통과 | 통과 |
| 합계 | 164개 | 164개 |

총 **328개**(기존 188 + 5단계 54 + Unicode 86)이며 같은 테스트의 반복 실행을 추가 집계하지 않는다. 실제 프로세스/TCP 결과와 승인 단계/경계 시뮬레이션을 위 범위대로 구분한다. Unicode 이름 정확성은 문화권 비교가 아닌 ordinal UTF-16 비교로 확인한다.

Unicode 최초 실행 로그/fixture: `tests/runs/ea2ad31f6e9544daa2abdb14816e83d4/`(Debug), `8e9771116d5442f38e10f2c0eb604919/`(Release). ordinal 비교 보강 후 최종 Unicode 집계/전체 출력은 `tests/runs/stage6-summary-19d183cd9d7b4780b625bbb6e8fb03e7/`의 구성별 JSON/log에 기록했다.

Release suite는 `tests/runs/suite-5c949bf4d1a24a4d86832a82c741d482/results.json`의 9개 스크립트 exitCode=0, 통과 수 합계 121을 확인했다. 끝의 합계 출력에서만 위 Hashtable 오류가 발생했으며 동일 Hashtable 입력의 집계 식을 별도로 검증해 수정했다. 이미 통과한 Release 테스트를 그 이유만으로 반복하지 않았다. Debug는 수정된 runner에서 exit 0 / Total PASS 121이며 `tests/runs/suite-0337048dbe884ef5b7b430d5365c3d10/results.json`에 기록했다.

이번 6단계 표준/오류 주입 빌드의 테스트에서는 SAC 차단이 발생하지 않았다. 보안 설정 변경·실행 파일 변형을 통한 우회는 하지 않았다. **5단계 protocol=5 원본 빌드가 과거 SAC에 차단되어 최종 검증 미완료였다는 사실은 유지**한다. 이번 전체 회귀 성공은 새 protocol=6 소스/바이너리의 결과이며 과거 차단 판정이나 미실행 기록을 소급 변경하지 않는다.

표준 바이너리 SHA-256:

- Debug: `073D5B19DA399368D07DFECCFA288172E6493860903AFD54BF22091479F26819`
- Release: `1E7BC9D37BDB7247D5F1A2E6A336471DA7FD80CC5E336D71E24C0F4C0BAFD6A1`

재실행은 프로젝트 루트에서 기존 STAGE5의 표준 빌드 명령과 오류 주입 props를 사용한다. 6단계의 오류 주입 출력은 `tests/runs/stage6-fault/{Configuration}/bin/`이다. 큰 파일 시험을 위해 10GiB 이상의 여유 공간을 권장한다. suite끼리는 고정 9000 포트 때문에 병렬 실행하지 않는다.

```powershell
foreach ($configuration in @('Release','Debug')) {
    powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/stage6_unicode.ps1 -Exe "FileTransferApp/x64/$configuration/FileTransferApp.exe"
    if ($LASTEXITCODE -ne 0) { throw "Unicode test failed or blocked: $configuration" }
    powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/RunStage5.ps1 -Exe "FileTransferApp/x64/$configuration/FileTransferApp.exe" -FaultExe "tests/runs/stage6-fault/$configuration/bin/FileTransferApp.exe"
    if ($LASTEXITCODE -ne 0) { throw "Regression failed or blocked: $configuration" }
}
```

RemoteSigned는 기존 사용자 승인에 따른 테스트 PowerShell 프로세스 범위다. 시스템/사용자 정책, SAC, AppLocker, 방화벽을 바꾸지 않는다. 차단 시 실제 exe 경로·hash와 Code Integrity 이벤트를 기록하고 통과로 세지 않는다. 실행 파일 이동/이름 변경/보안 우회는 하지 않는다. 로그·큰 파일·바이너리·DPAPI 데이터는 기존 `.gitignore`로 제외하고 소스/테스트/문서만 로컬 커밋한다.
