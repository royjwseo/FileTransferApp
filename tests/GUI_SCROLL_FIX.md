# Win32 GUI 스크롤 화면 갱신 수정

## 기준과 분석의 한계

기준: `feature/stage8-lan-discovery`, `f9c2d9f33151e5a2501f4feba174566e8279c2bb`.
origin fetch 후 원격 stage8과 로컬 HEAD 일치 확인. 수정 브랜치: `feature/fix-win32-gui-scroll`.

**사용자가 보고한 지속적인 잔상의 실제 원인을 재현으로 확정하지 못했다.**
아래의 코드상 확인 사항과 잔상 관련 가설을 구분한다. 원본을 별도 디렉터리에 빌드하여
동일한 휠 스크롤 후 강제 전체 갱신 전후 클라이언트 픽셀을 비교했다.
원본/수정본, legacy/LAN 모드 모두 정지 후 차이는 0이었다.
따라서 원본이 실패하고 수정본이 통과하는 시각적 재현 증거는 없으며,
이번 변경을 모든 환경에서 잔상이 해결되었다는 증명으로 해석하면 안 된다.

## 호출 관계와 확인된 코드 동작

`RunGui` → `WndProc(WM_CREATE)` → `Window::Create` → `Layout`.
`WM_VSCROLL`, `WM_MOUSEWHEEL`, `WM_SIZE`, `WM_DPICHANGED` 모두 같은 `Layout` 사용.

- `ScrollWindow`/`ScrollWindowEx` 사용 없음. 픽셀 스크롤과 자식 이동의 중복은 없다.
- 기존 좌표는 `Scale(contentY) - scrollY`. LAN 하단 영역은 contentY에 225 DIP 추가.
  스크롤 값과 SCROLLINFO는 물리 픽셀이다. 기존 오프셋 중복/누락은 발견하지 못했다.
- 기존 `MoveWindow(..., TRUE)`는 컨트롤별로 즉시 갱신한다. 그룹박스와 입력/버튼/라벨은
  부모가 같은 겹치는 형제 창이다. 이동 도중 일부는 이전 위치, 일부는 새 위치에서 그려진다.
  **이 중간 상태와 형제 배경 갱신 순서는 잔상의 가능 경로이지, 이번 환경에서 확정한 원인이 아니다.**
  MoveWindow 자체도 이전 위치를 무효화하므로 단순히 '이전 영역 무효화가 전혀 없다'고 판단하지 않는다.
- `SetScrollInfo` 전에 읽은 client width로 레이아웃을 계산했다. 스크롤바 표시/숨김이
  너비를 바꾸는 시점과 계산 시점이 불일치한다. 수정본은 표시 상태 반영 후 다시 읽는다.
- `SB_TOP`, `SB_BOTTOM`, `SB_THUMBPOSITION` 처리 누락 확인. 기존 thumb tracking은
  HIWORD의 16비트 값 대신 `nTrackPos`를 사용하며 이 방식은 유지했다.
- 최소화의 0 크기 `WM_SIZE`에도 레이아웃을 수행했다. 이제 최소화는 건너뛴다.
- DPI 변경은 폰트/좌표만 배율 변경하고 scrollY는 이전 물리 픽셀을 유지했다.
  이제 이전 DPI에 대한 상대 스크롤 위치도 함께 환산한다.
- Create의 LAN Disconnect 별도 SetWindowPos는 Layout과 동일 좌표를 다시 적용하는 중복 경로였다.
  삭제하여 모든 위치를 Layout에서 관리한다.
- 사용자 정의 `WM_PAINT`/`WM_ERASEBKGND` 없음. `DefWindowProc`와
  클래스 배경 브러시 `COLOR_BTNFACE+1`로 부모 배경을 지운다.
- 기존 `UpdateWindow`는 RunGui의 최초 표시에서만 호출한다. 별도 InvalidateRect/RedrawWindow 경로 없었다.
- 부모 `WS_CLIPCHILDREN`, 자식 `WS_CLIPSIBLINGS` 없음. 기존 Z-order를 유지한다.
  그룹박스 안의 컨트롤도 실제로는 형제이므로 스타일만 추가하면 넓은 그룹박스 사각형의
  클리핑이 배경 갱신에 영향을 줄 수 있다. 스타일 변경은 하지 않았다.

## 수정한 함수와 화면 갱신 방식

`Window::Layout`:

1. SCROLLINFO의 범위 `Scale(795 + top) - 1`, page=client height를 설정한다.
   SetScrollInfo가 정한 최종 위치를 다시 읽으므로 끝 위치는 `max(0, nMax - nPage + 1)`이다.
2. 스크롤바 상태 반영 후 client rect를 다시 읽는다.
3. 공통 place 함수에서 이전 rect를 부모 좌표로 변환하고 새 rect를 계산한다.
   위치/크기가 같으면 이동과 무효화를 생략한다.
4. `SetWindowPos(SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW)`로 이동한다.
   이동 도중의 즉시 그리기를 막고 보이는 컨트롤의 이전/새 rect 합집합 HRGN을 모은다.
5. 모든 이동 후 그 영역에 한 번 `RedrawWindow(RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN)`.
   부모의 노출 배경과 겹치는 그룹박스/형제 컨트롤을 함께 무효화한다. RDW_FRAME은 입력창의 WS_EX_CLIENTEDGE 같은 nonclient 테두리 갱신도 포함한다.
   동기 전체 강제 갱신 없이 정상 메시지 루프가 빠른 연속 입력의 paint를 합친다.
   그룹박스도 이동하므로 스크롤 시 합집합은 뷰포트 대부분일 수 있다.
   클라이언트 밖의 영역은 Windows가 잘라낸다. 무조건 전체 창 재구성/재생성은 하지 않는다.

`WndProc`: 누락된 스크롤 명령 처리, 최소화 레이아웃 생략, DPI scrollY 환산.
`Window::Create`: 중복 Disconnect 위치 지정 제거.

더블 버퍼링은 적용하지 않았다. 이번 수정은 표준 컨트롤의 이동/무효화 순서를 정리한다.
배경 깜빡임의 실측 증거가 없어 렌더링 시스템이나 스타일을 추가하지 않았다.
네트워크, LAN 발견, 승인 정책, 파일 전송, 프로토콜, 보안 정책은 변경하지 않았다.

## 재현 및 자동 검사

수동 재현: GUI 실행 → 창 높이를 최소 근처로 축소 → 휠을 빠르게 왕복 → 스크롤바를
직접 드래그 → 최상단/최하단 왕복 → 스크롤하면서 크기 조정 → 가림/복원/최소화/복원.
라벨, 그룹박스 테두리, 버튼의 이전 위치에 픽셀이 남는지 관찰한다.

신규 검사:

```powershell
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/gui_scroll.ps1 -Exe FileTransferApp/x64/Debug/FileTransferAppGui.exe
powershell -NoProfile -ExecutionPolicy RemoteSigned -File tests/gui_scroll.ps1 -Exe FileTransferApp/x64/Release/FileTransferAppGui.exe
```

`GuiDriver.cs`의 기존 실제 Win32 GUI 자동화를 사용하고 `ScrollProbe.cs`로
휠/스크롤 메시지, SCROLLINFO, 자식 좌표, 최소화/복원을 검사한다.
legacy와 기본 LAN 모드를 모두 검사한다. 실제 마우스 드래그/운영체제의 thumb tracking 입력은 미검증이다.
작은/큰 창과 스크롤 혼합, 끝 위치, 입력창 사용을 검사한다.
바닥/중간/복원 후 GetDC+BitBlt로 현재 픽셀과 강제 전체 RedrawWindow 후 픽셀을 비교한다.
전체 이미지 차이 0을 요구하며 PNG는 tests/runs/scroll-*에 저장한다.
이는 정지 상태의 화면 비교다. 스크롤 도중의 순간적인 잔상이나 모든 표시 환경을 보증하지 않는다.

## 빌드와 회귀 결과

검증일: 2026-10-15 (Asia/Seoul). Windows x64, VS 18 Community MSBuild 18.10.1, 대화형 데스크톱.
Debug/Release x64 빌드 성공, 빌드 로그의 오류/경고 없음. 신규 스크롤 검사는 구성별 32개, 합계 64개 PASS.
바닥/실제 중간 휠 위치/복원 후 픽셀 차이 모두 0. 최종 코드의 Debug GUI 회귀도 재실행 통과.
로그 위치: `tests/runs/scroll-fix/` (Git ignore).
기존 448개는 구성별 CLI 121 + Unicode 43 + legacy GUI 35 + LAN GUI 25 = 224, 두 구성 합계다.
기존 재검증 절차는 `STAGE8.md`를 따른다. GUI suite는 순차 실행했다.

| 검사 | Debug | Release |
|---|---:|---:|
| 기존 CLI stage1~5 | 121 PASS | 121 PASS |
| 기존 Unicode | 43 PASS | 43 PASS |
| 기존 GUI 기본/고급/다운로드 | 10+8+17 PASS | 10+8+17 PASS |
| 기존 LAN GUI | 25 PASS | 25 PASS |
| 신규 스크롤 | 32 PASS | 32 PASS |

기존 **448개 PASS**, 신규 **64개 PASS**, 합계 **512개 PASS**. 실패 없음.
RDW_FRAME 보완 후 Debug GUI 60개를 재실행했고, Release GUI는 보완 후 빌드로 실행했다.
CLI/네트워크 소스는 변경하지 않았다. 초기 sandbox 빌드는 기존 추적 파일 권한 오류로
중단되어 사용자 권한으로 다시 빌드했다. SAC/Application Control 차단이나 정책 변경은 없었다.

최종 로그: `cli-{Debug,Release}.log`, `stage6_unicode-{Debug,Release}.log`,
`stage7_gui-{Debug,Release}.log`, `stage7_advanced-{Debug,Release}.log`,
`stage7_1_download-{Debug,Release}.log`, `stage8_lan-{Debug,Release}.log`,
`scroll-{Debug,Release}.log`, `build-{Debug,Release}.log`, `pixels.log`.

최종 스크롤 이미지: `tests/runs/scroll-2e800c10936a4b248f73bcdc7b1aeaba` (Debug),
`tests/runs/scroll-c5fea90101924bafa7558434b8f23e68` (Release).
전후 비교 원본 빌드와 이미지: `tests/runs/scroll-fix/before`, `pixels-{before,after}-{True,False}`.

최종 GUI 실행 파일 SHA-256:
- Debug: `AAE4E09BAF992FAA6DFB5B882C4C7D8CAD27A3F8F7B0BDF11DB67C752D3B8D80`
- Release: `666C63907D3B180578A430676A714AD0F643E9FE1D6E63DCD6BBDC95B078259D`

## 남은 수동 검증

- 보고된 지속 잔상을 실제 사용자 환경에서 재현하고 동일 조건으로 수정본과 비교.
- 빠른 실제 휠 입력/스크롤바 드래그 중의 순간 잔상과 깜빡임 육안 확인.
- 스크롤 중 실제 창 크기 드래그 및 다른 창으로 가린 뒤 노출.
- 100%, 125%, 150% 실제 DPI 설정과 모니터 간 이동, 글꼴/레이아웃/스크롤 위치 확인.
  합성 WM_DPICHANGED를 보내 실제 DPI 검증을 대신하지 않았다.
- 최소화/복원 후 시각 상태 및 모든 버튼의 포인터 클릭/키보드 탐색.
- 실제 두 PC LAN 연결/파일 전송. 로컬 회귀는 실제 두 PC 결과가 아니다.

WM_PAINT 발생, 좌표 정상, 정지 후 픽셀 비교 통과만으로 잔상 완전 해결을 단정하지 않는다.
SAC/보안 정책 변경, 우회, main 병합, 강제 push는 수행하지 않는다.

## Win32 API 근거

- [MoveWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-movewindow): TRUE일 때 이동 직후 UpdateWindow로 즉시 WM_PAINT 처리.
- [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos): SWP_NOREDRAW 사용 후 노출 영역 등을 애플리케이션이 갱신해야 한다.
- [RedrawWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-redrawwindow): RDW_ERASE/FRAME/ALLCHILDREN의 역할과 비동기 invalidation 구분.
