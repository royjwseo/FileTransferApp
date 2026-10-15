# v1.0.0 Windows 배포 준비

## 기준과 현재 상태

기준 main: `5166607`; 브랜치 `feature/release-v1.0.0`.
`main`은 stage8 `f9c2d9f`와 스크롤 수정 `b38f3a3`을 포함한다.
작업 시작 시 작업 트리 clean. GitHub `royjwseo/FileTransferApp`은 **Private**,
Release 0개, Actions workflow 0개였다. API 읽기·Git push 인증 사용 가능.
저장소 공개 범위·기존 태그·main은 변경하지 않는다. 실제 두 PC 검사는 범위에서 제외한다.

현재 상태: 배포 구조 구현 및 로컬 ZIP 검증 진행. **Debug 회귀가 Application Control로 차단되었고 Release 첫 회귀에도 실패가 있어 공개 배포 준비 완료로 판정하지 않는다.**
기존 512 PASS 기록을 이번 후보의 성공으로 소급 사용하지 않는다.
태그 및 Release는 생성하지 않는다. 원격 Actions 실행도 이번 작업의 '구성과 로컬 검증만' 범위 밖이므로 미실행이다.

## 변경 범위

- `.github/workflows/release.yml`: Windows VS2026 runner, Debug/Release 빌드·512개 검사,
  GUI 데스크톱 확인, 검사 성공 후 ZIP/체크섬 생성, 반복 패키징·추출 실행 검사.
  일반 브랜치 push에는 실행되지 않는다. 태그/수동 실행만 허용한다.
  read-only 검증 job과 contents:write 초안 업로드 job을 분리한다.
  기존 태그가 현재 commit과 일치해야 하며 기존 Release는 덮어쓰지 않는다.
  workflow는 공개 게시를 수행하지 않고 draft만 생성한다.
- `tools/ReleaseCommon.ps1`: MSBuild 탐색, 공통 버전 읽기.
- `tools/test-release.ps1`: 기존 suite를 순차 실행하고 exit code와 정확한 PASS 수를 확인.
  실패/차단 후 독립 suite는 계속 검사하되 마지막에 실패로 종료해 패키징을 막는다.
- `tools/package-release.ps1`: Release GUI 버전·Debug flag·PE x64/GUI·소스 최신 여부 검사,
  명시적 3파일 allowlist, 별도 staging, ZIP과 SHA256SUMS 생성.
  항목 정렬과 ZIP 날짜 고정으로 동일 입력에서 동일 ZIP을 만든다.
  빌드 EXE까지 bit-for-bit 재현되는 빌드라고 주장하지 않는다.
  기존 파일과 같으면 유지, 다르면 오류. staging과 실패 후보는 검토용으로 보존한다.
- `tools/test-package.ps1`: ZIP 항목·체크섬·추출 EXE 동일성·버전 및 임시 위치 실행 검사.
- `packaging/README.txt`, `LICENSE.txt`, `RELEASE_NOTES.md`: 사용자 안내, 권리 보유, Release 초안.
- `FileTransferApp/include/AppVersion.h`: 제품 버전 단일 기준. wire protocol 버전과 별개.
- `FileTransferApp/resources/App.rc`: GUI/CLI의 Windows VERSIONINFO.
- 두 vcxproj: 리소스 등록, Debug 리소스 flag, Release PDB 경로를 파일명만 기록.
- Gui.cpp: 창 제목에 제품 버전 표시만 추가. main.cpp: --version에 제품 버전 추가.
  네트워크·프로토콜·상태 머신·승인 정책·스크롤 동작은 변경하지 않는다.
- README: 사용자 배포 후보·패키징 절차 추가. dist/는 Git 제외.

## 빌드·의존성 조사

VS18 Community, MSVC 14.51.36231 (dumpbin 14.51.36260), v145, C++20, x64.
런타임은 기존 동적 `/MD` Release 설정 유지. `/MT`로 변경하지 않았다.
외부 패키지·서드파티 GUI 라이브러리 없음. Win32/Windows SDK 라이브러리만 사용한다.
Windows DLL·VC++ 런타임 DLL은 ZIP에 재배포하지 않는다.

직접 import 중 VC++ 런타임:
`MSVCP140.dll`, `MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`.
UCRT `api-ms-win-crt-*` 및 KERNEL32/USER32/GDI32/COMDLG32/SHELL32/ole32/bcrypt/
CRYPT32/WS2_32/COMCTL32/IPHLPAPI는 Windows 런타임/시스템 의존성이다.
공식 최신 [v14 x64 Redistributable](https://aka.ms/vc14/vc_redist.x64.exe)을 설치한다.
빌드 도구와 같거나 새로운 런타임이 필요하다.
[Microsoft 런타임 안내](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

후보 대상: Windows 10 1703 이상 / Windows 11 x64.
현재 정적 import인 [SetProcessDpiAwarenessContext](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setprocessdpiawarenesscontext)의 최소 client는 Windows 10 1703이다.
이 최소 API 요건이 모든 구버전 Windows에서의 실행 보증은 아니다. OS별 실기 검증은 미완료다.
로컬 실행 환경은 **Windows 11 Pro 10.0.26200** (CIM 확인).
Get-ComputerInfo의 WindowsProductName은 Windows 10으로 반환되어 실제 제품 판정에는 사용하지 않았다.

Release RSDS PDB 경로는 `FileTransferAppGui.pdb` 파일명만 남는다.
PDB는 ZIP에 포함하지 않는다. 선택적 FILETRANSFER_* 환경 변수 이름은 제품 기능으로 유지한다.
개발 도구/프로젝트 디렉터리가 앱 실행에 필요하지 않은지 추출 실행으로 확인한다.

## Windows 보안

Release EXE Authenticode: **NotSigned**. Defender 서비스·백신·실시간 보호는 활성 상태이며 정책을 바꾸지 않았다.
추출 GUI 실행 검사는 로컬 실행 가능성만 확인하며 다운로드의 Mark-of-the-Web나 모든 PC 보안 정책을 재현하지 않는다.
SmartScreen 평판·SAC/조직 코드 무결성 정책에 의해 서명 없는 앱이 차단될 수 있다.
[Microsoft SAC 서명 안내](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/code-signing-for-smart-app-control).
코드 서명 인증서를 구매·신청·생성하지 않았다. 체크섬은 게시자 인증이나 코드 서명을 대신하지 않는다.

이번 Debug stage4 fault EXE 차단:

- 파일: `tests/runs/release-validation-42c006741d9d4749b9f5f057706a9737/fault/Debug/bin/FileTransferApp.exe`
- SHA-256: `5C4E766351128DA8537DB52B73406F99FD5AFB7767B2D0CDC881ABDB2D480CC4`
- CodeIntegrity 이벤트 **3077**, **3033**: Enterprise signing level / code integrity policy 위반.
- Policy ID: `{0283ac0f-fff1-49ae-ada1-8a933130cad6}`.
- 이벤트 원문: 해당 run의 `application-control.json`.

Debug fault EXE는 변형·재시도하지 않는다. 독립 검사 및 원래 요청된 Release 구성 검사는 별도로 수행한다.
차단을 해결하려고 서명 정책을 끄거나 실행 파일 내용·경로를 변형하는 우회는 하지 않는다.

## 검증 결과

검증일: 2026-10-15 (Asia/Seoul). Debug/Release x64 빌드 성공, 빌드 오류·경고 없음.

| 회귀 항목 | Debug | Release |
|---|---:|---:|
| CLI stage1~5 | 116/121 (5 차단) | 121/121 (stage4 단독 재실행 포함) |
| Unicode | 43/43 | 43/43 |
| GUI 기본/고급/다운로드 | 35/35 | 35/35 |
| LAN GUI | 25/25 | 25/25 (단독 재실행 포함) |
| GUI 스크롤 | 32/32 | 32/32 |
| 합계 | 251/256 | 256/256 |

**기준 항목별 507/512 PASS, 5개 차단·미완료. 전체 검증 성공 아님.**
Release 첫 실행은 241 PASS로 stage4/LAN suite가 실패했고,
동일 바이너리·기존 테스트를 변경하지 않은 단독 재실행에서 각각 19/19, 25/25 PASS였다.
이후 통과로 첫 실패를 지우거나 원인이 해결됐다고 판단하지 않는다.
현재 자동화의 한 번 실행으로 전체 검증 성공 조건은 확인되지 않았다.
프로덕션 코드·기존 테스트는 실패를 우회하도록 수정하지 않았다.

로그:
- Debug: `tests/runs/release-validation-42c006741d9d4749b9f5f057706a9737`.
- Debug 초기 CLI 집계: `tests/runs/suite-065252b905b44c3c9dccf87809bc4184`.
- Release: `tests/runs/release-validation-79b68f7857f74115a218381bce2a364e`.
- Release 첫 결과: `results.json`, 단독 재실행: `stage4-Release-retry.log`, `stage8_lan-Release-retry.log`.
- 기준 항목별 최종 집계: 위 Release run의 `final-summary.json` (`overallComplete=false`).

### 검토용 ZIP

`dist/FileTransferApp-v1.0.0-win-x64.zip` (125340 bytes).

```text
FileTransferApp-v1.0.0-win-x64/
  FileTransferAppGui.exe
  LICENSE.txt
  README.txt
```

ZIP SHA-256: `05e4ca164970feaaa08efb0b8790bdc0dbe5bd1715e4b8becadfd6051832a2f6`.
EXE SHA-256: `6D1D6CA061A8B04442B4C2C27D20F95CD0891293E9B12B5FF9F85A823C5F54E4`.
체크섬 파일은 `dist/SHA256SUMS.txt`.
동일 입력 두 번 패키징의 해시 일치, 정확한 3파일 allowlist, 압축 해제,
소스 Release와 추출 EXE의 해시·제품 버전 일치 검사 통과.
FILETRANSFER_GUI_SETTINGS/FILETRANSFER_LAN_PORT를 제거하고 별도 임시 폴더에서
추출 GUI 생성·WM_NULL 응답·정상 종료 확인. 런타임이 이미 설치된 개발 PC에서의 검사다.
최종 추출 위치: `%TEMP%/FileTransferApp-package-a12919cfa27e4b609515c86c3839ae99`.
기존 다른 내용인 ZIP의 덮어쓰기 거부·원본 해시 보존도 확인했다.
검토용 ZIP은 전체 회귀 통과 전에 도구 검증용으로 생성했으며 배포 승인된 최종본이 아니다.
이전 검토 ZIP도 `dist/preliminary-*`에 보존했다. 이 산출물과 staging은 Git에서 제외한다.

PowerShell 구문 및 vcxproj XML 검사 통과. README 로컬 문서 링크 검사 통과.
Release EXE의 ASCII/UTF-16 문자열 검사에서 개발자 절대 경로·tests/runs 경로를 발견하지 못했다.
원격 Actions는 활성화되어 있지만 workflow를 실행하지 않았으므로 runner에서의 GUI/네트워크 동작은 미검증이다.
Release 첫 실행의 stage4는 `.part` Test-Path에서 Access is denied, LAN suite는 동시 요청의 승인 화면 대기 타임아웃으로 실패했다. Release fault EXE의 실행 차단으로 판단하지 않으며 첫 실패 로그를 보존한다. 원인 확정이나 해결을 주장하지 않는다. 동일 바이너리 단독 재검증 결과를 별도로 기록한다.
기준 테스트 수는 구성별 256, 두 구성 합계 512다.
Debug의 초기 aggregate runner는 stage4에서 89 PASS 후 중단되었다.
리소스의 Debug flag 정의를 명시한 후 정상 Debug CLI/GUI를 갱신했으며, 정상 Debug CLI 최종 출력의 IsDebug=true와 stage1 12개도 재확인했다. 이 중복 검사는 507개에 추가 합산하지 않는다.
Debug fault EXE는 그대로 보존하고 나머지 suite를 별도 실행했다.

## 로컬 재검증

```powershell
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tools/test-release.ps1
if ($LASTEXITCODE -ne 0) { throw 'Validation failed/blocked; do not release' }
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tools/package-release.ps1
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tools/test-package.ps1
```

PowerShell RemoteSigned는 해당 테스트 프로세스 실행 옵션이며 영구 실행 정책·SAC·방화벽 설정을 변경하지 않는다.
GUI 검사는 대화형 데스크톱에서 다른 GUI suite와 겹치지 않게 실행한다.
Actions에서 데스크톱 또는 실제 dialog/pixel 검사에 문제가 생기면 성공으로 대체하지 않는다.
호스티드 runner의 사용 가능 여부·Private 저장소의 Actions 한도/과금 설정·GUI 표시 상태는 원격 실행 전 별도 확인이 필요하다.
원격 workflow 성공을 로컬 성공으로 가정하지 않는다.

## 승인 후 실제 게시 절차

현재 작업은 배포 브랜치 push까지만 한다. main 병합·태그·draft/public Release 생성은 하지 않는다.

1. 보안 차단 및 누락 검증을 정식 승인된 환경에서 해결하고 전체 검사 결과를 확인한다.
   필요하면 정상 코드 서명 절차를 별도로 결정한다. 실제 두 PC 검사는 이번 작업의 범위 밖이다.
2. 사용자가 배포 내용과 권리 정책, 접근 범위를 검토한다.
   현재 Private 저장소이므로 일반 외부 사용자가 익명으로 다운로드할 수 없다.
   Public 변경 또는 별도 공개 배포 저장소는 별도 승인 사항이며 여기서는 변경하지 않는다.
3. 배포 브랜치의 main 반영 여부·태그 대상 commit을 별도 승인한다.
4. 승인된 commit에 **새로운** `v1.0.0` 태그를 만들고 push한다. 기존 태그가 있으면 덮어쓰지 않는다.
5. 태그 workflow가 검증·패키징 후 **draft Release**에 ZIP과 SHA256SUMS를 올린다.
   workflow_dispatch에서 create_draft=false면 검증·artifact만 만들며,
   true여도 현재 commit의 기존 버전 태그가 있어야 한다. workflow 자체는 태그를 만들지 않는다.
   새 workflow의 수동 실행은 기본 브랜치에 workflow가 등록되어야 사용할 수 있다.
6. ZIP 내용·체크섬·테스트 로그와 사용자 안내를 확인한 뒤, 사용자의 최종 게시 승인 후
   GitHub Releases UI에서 초안의 Publish release를 누른다.
   자동화는 기존 Release의 asset이나 태그를 덮어쓰거나 공개 상태로 바꾸지 않는다.

남은 수동 확인: 100/125/150% 실제 DPI, 빠른 실제 휠/스크롤바 드래그·가림/복원 잔상,
IME/drop, 깨끗한 Windows/VC 런타임 설치 환경, SmartScreen/SAC 다운로드 실행.
실제 두 PC 간 LAN·파일 전송은 본 작업에서 수행하지 않는다.
