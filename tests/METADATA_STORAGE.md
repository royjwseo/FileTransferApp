# 전송 정보 별도 보관

수신한 `.fta` 관리 정보는 더 이상 다운로드 폴더에 새로 생성하지 않는다.
기본 위치는 `%LOCALAPPDATA%\FileTransferApp\transfer-metadata`다.
GUI와 CLI 모두 같은 저장 정책을 사용하며, 파일 전송 프로토콜은 바꾸지 않았다.
수신 중 실제 데이터인 `<파일명>.part`는 최종 파일과 같은 폴더에 유지한다.
SHA-256 검증과 flush 후 원자적으로 최종 이름으로 바꾸는 기존 처리를 유지하기 위해서다.
완료되면 다운로드 폴더에는 최종 파일만 남는다.

## 보존 및 복구

- `.fta` 내용은 기존과 같은 DPAPI 보호 transferId·파일 객체 신원·디렉터리 신원이다.
- 메타데이터 파일명은 수신 폴더의 volume/file identity와 transferId를 SHA-256으로
  해시한 64자리 값으로 구성한다. Windows 확장 경로도 적용한다.
  다른 다운로드 폴더의 같은 전송 ID가 섞이지 않으며, 경로 별칭과 대소문자에 의존하지 않는다.
- 메타데이터 디렉터리도 reparse point를 거부하고 핸들로 고정한다.
  생성은 CREATE_NEW, flush, no-replace owned-handle rename 순서다.
- 전송 완료 후 메타데이터는 보존한다. ACK 유실 시 완료 조회와 재시작 복구에 필요하다.
  임의 삭제하면 파일 자체는 남지만 해당 전송의 복구·완료 확인이 불가능해질 수 있다.
- 기존 다운로드 폴더의 `<파일명>.part.fta`는 전역 스캔하거나 일괄 이동하지 않는다.
  해당 파일의 이어받기/완료 조회 때 중앙 기록이 없으면 기존 기록을 읽는다.
  DPAPI·transferId·파일/폴더 소유권·필요한 해시 검사가 성공한 뒤 보호된 내용을
  새 위치에 복사하여 flush/publish한 다음 기존 owned metadata를 삭제한다.
  다른 드라이브에서 받은 파일도 이동 가능하도록 파일 rename 대신 복사를 사용한다.
- 중앙 기록이 손상됐거나 접근이 거부된 경우 기존 기록으로 우회하지 않는다.
  검증 실패 시 기존 데이터·정보를 삭제하거나 자동 덮어쓰지 않는다.
- 이동 중 실패/종료 시 기존 정보 또는 새 기록이 남도록 새 기록 게시를 먼저 한다.
  중단 시 남은 `.new`나 이중 기록은 임의 삭제하지 않는다.

새 다운로드가 아닌, 예전에 받은 파일 옆의 `.fta`는 해당 전송을 복구/조회하기 전까지
그 위치에 남을 수 있다. 앱이 종료된 상태에서 정보를 정리한다는 이유로 삭제하지 않는다.
앱 데이터 폴더는 캐시가 아니므로 일반 임시 파일 정리 대상으로 취급하면 안 된다.

## 변경 및 검사

- `FileTransfer.cpp`, `TransferStorage.h`: 중앙 저장, 핸들 고정, 검증 후 legacy 마이그레이션.
- `MetadataProbe.cs`: 테스트 전용 폴더/파일 identity로 정확한 기록 위치 확인.
- `GuiDriver.cs`, `stage1.ps1`: 시험 프로세스의 `FILETRANSFER_METADATA_ROOT`를 run 내부의
  절대 경로로 지정해 실제 사용자 정보를 건드리지 않는다.
- 기존 stage1/stage3_validation/stage5_resume/stage7_1_download의 기록 위치 검사와
  손상·다른 폴더 이동 공격 fixture를 새 구조에 맞춘다. 기존 안전성 검사는 유지한다.
- `metadata_storage.ps1`: 중앙 완료 조회, 구형 완료 기록 이동, 구형 부분 전송 이어받기,
  손상 중앙 기록의 legacy fallback 거부, 손상 legacy 기록 이동 거부 5개.
- 전체 runner/CI 목표는 612개(기존 602 + 새 검사 5개 × 두 구성)다.

2026-10-15 Debug/Release x64 빌드 성공(오류·경고 없음).
첫 회귀에서 긴 시험 디렉터리 이름과 97자리 메타데이터 파일명이 결합해
게시용 `.new` 경로가 Win32 경로 제한에 걸리는 문제를 발견했다.
파일명을 64자리 해시로 줄이고 확장 경로를 적용한 뒤 재빌드했다.
수정 후 최종 빌드의 관련 회귀 결과는 다음과 같다(전체 612개 실행 결과가 아님).

| 관련 검사 | Debug | Release |
|---|---:|---:|
| 신규 저장·legacy 이동 | 5/5 PASS | CLI 실행 차단 |
| 기본·validation·취소·이어받기·Unicode | 89/89 PASS | CLI 차단으로 미실행 |
| 다운로드 폴더·다른 폴더 이동 공격·복구 | 17/17 PASS | 16/17 PASS, 마지막 CLI 연동 검사 차단 |
| 자동 수신 raw wire | 15/15 PASS | 15/15 PASS |
| 대화·자동 수신 UX | 16/16 PASS | 3/16 PASS, 실제 키보드 foreground 확보 실패로 중단 |
| 등록 PC 인증 | 14/14 PASS | 14/14 PASS |
| 기존 GUI 스크롤 | 32/32 PASS | 32/32 PASS |
| 합계 | **188/188 PASS** | **80/188 PASS, 일부 실행 차단·미완료** |

최종 관련 검사 376개 중 268개 PASS다. 나머지 108개는 보안 차단으로 중단/미실행하거나
키보드 포커스 확보 실패로 미완료다. 관련 검사 외 전체 runner의 236개는 이번 변경에서
실행하지 않았으므로 전체 회귀 통과를 주장하지 않는다.
Release GUI는 실행됐으며 Release CLI의 Application Control 차단과 구분한다.
차단된 CLI를 재실행하지 않고 독립 GUI 검사를 이어서 실행했다.
정책 변경·이름 변경·차단 우회는 하지 않았다.

최종 Release CLI SHA-256:
`26D9F5A4C6F2B7359542AC6C72D4A7278552136866192DC8DC10C5AF1A00BB80`.
초기 결과는 `metadata-initial-results.json`, `metadata-initial-regression.log`에 보존한다.
초기 Release 대화 UX 검사는 키보드 foreground 확보 실패로 10/16에서 중단됐다.
검사 실패를 성공으로 대체하거나 다른 창에 키 입력을 주입하지 않았다.

로그: `tests/runs/metadata-build-{Debug,Release}.log`, `metadata-storage-Debug.log`,
`metadata-regression.log`, `metadata-release-gui-regression.log`, `metadata-final-results.json`,
`metadata-{suite}-{Debug,Release}.log`, `metadata-CodeIntegrity-Release-*.json`.

실제 2PC에서 전송 후 다운로드 폴더에 최종 파일만 남는지, Windows 계정별 앱 데이터
보관과 재시작 후 이어받기를 확인해야 한다. 다른 드라이브에서의 legacy 이동은
구현상 지원하지만 실제 다중 드라이브 환경 검증은 별도로 필요하다.
