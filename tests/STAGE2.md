# 2단계 결과: 승인 후 파일 생성 및 임시 파일 소유권

기존 C++20 / v145 설정을 유지했습니다. requestId 상태 머신은 이번 변경에 포함하지 않습니다.

## 구현

- `Protocol::BeginFile`: 승인, 현재 전송 상태, 파일명, 크기, 승인된 메타데이터 일치를 먼저 검사합니다. 검증 실패 시 `received` 폴더도 만들지 않습니다.
- 파일명은 기존 ASCII 제한을 유지하며 Windows 예약 이름과 끝의 점도 거부합니다.
- 저장 폴더가 재분석 지점이면 거부하고, 폴더 핸들을 보유해 전송 중 폴더의 이름 변경·삭제를 방지합니다.
- `.part`는 `CreateFileW(CREATE_NEW)`로 생성합니다. 기존 파일·링크를 덮어쓰지 않으며 생성에 성공한 핸들만 소유합니다.
- `AbortFile`: 소유한 핸들에 삭제 처리를 설정하고 닫습니다. 경로로 파일을 삭제하지 않으므로 다른 파일이나 기존 `.part`를 정리 대상으로 삼지 않습니다. 삭제 API 실패 시 오류를 출력하고 임의 경로 삭제로 대체하지 않습니다.
- `EndFile`: 크기 검사와 `FlushFileBuffers` 후 파일 핸들로 최종 이름을 변경합니다. 덮어쓰기를 금지하므로 완료 직전 생긴 같은 이름의 파일도 보존합니다. 이 작업이 성공한 뒤에만 ACK를 생성합니다.
- `Protocol::FailFile`과 `TakeFileError`: 파일 실패 사유를 Network에 전달합니다.
- `Network::ReceiveLoop`: Protocol 잠금을 해제한 뒤 실패 통지를 전송하고 기존 종료 경로를 따릅니다. 기존 소켓 잠금·shutdown·join 구조는 유지합니다.

`Protocol.h/.cpp`, `Network.cpp`, 테스트 파일을 변경했습니다.
`FileTransfer`로의 책임 이동과 전체 상태 머신은 후속 단계에서 진행합니다.

## 실패 통지

기존 `FILE_REJECT(8)`의 빈 payload와 거절 동작은 유지합니다.
새 `FILE_ERROR(9)`의 payload는 1~256바이트의 출력 가능한 ASCII 사유입니다.

잘못된 파일 메시지나 저장 실패는 소유한 임시 파일을 정리한 뒤 FILE_ERROR를 보내고 연결을 종료합니다.
상대방은 `[Peer file error]`로 사유를 출력하고 연결 종료·승인 대기 취소를 수행합니다.
오류 메시지에 다시 오류 메시지를 응답하지 않습니다. TCP 연결 자체가 실패하면 통지는 전달되지 않을 수 있습니다.
requestId가 없으므로 실패 뒤 같은 연결을 계속 사용하거나 특정 전송에 오류를 적용하지 않습니다.
양방향 전송 중 한쪽 수신이 실패하면 해당 연결의 양쪽 전송이 중단됩니다.
실패 원인까지 전달하려면 양쪽 모두 이번 실행 파일을 사용해야 합니다.

`SendFile()`의 ACK 대기와 성공 반환 시점은 아직 기존 동작입니다.
파일 저장 성공을 기다려 함수 결과에 반영하는 작업은 requestId를 도입하는 후속 단계에 남겨 두었습니다.

## 빌드 및 테스트

Visual Studio Community 2026:

| 항목 | 결과 |
|---|---|
| Debug x64 빌드 | 성공, 오류·경고 없음 |
| Release x64 빌드 | 성공, 오류·경고 없음 |
| Release 2단계 자동 테스트 | 20개 PASS |
| Release 1단계 회귀 테스트 | 12개 PASS |
| Debug 실행 테스트 | Windows Smart App Control 서명 정책에 의해 차단 |

Debug 실행 차단은 샌드박스 밖에서도 재현됐습니다. Windows CodeIntegrity 로그의
3033/3077/3118 이벤트에서 서명 정책 차단을 확인했으며, 보안 정책은 변경하지 않았습니다.
이 단계에서는 Debug 런타임 테스트를 통과했다고 주장하지 않습니다.

2단계 테스트 항목:

1. 승인 없이 FILE_BEGIN → 파일·폴더 미생성
2. 승인 대기 중 FILE_BEGIN → 파일·폴더 미생성
3. 거절 후 FILE_BEGIN → 파일·폴더 미생성
4. 승인된 파일명과 다른 FILE_BEGIN 거부
5. 승인된 크기와 다른 FILE_BEGIN 거부
6. 짧은 메타데이터 거부
7. 크기 상한 초과 거부
8. 경로 이동을 포함한 파일명 거부
9. Windows 예약 파일명 거부
10. 끝의 점이 있는 파일명 거부
11. 기존 최종 파일 내용 보존
12. 기존 `.part` 내용 보존
13. 저장 폴더 위치가 파일일 때 저장 실패 통지 및 기존 내용 보존
14. 수신 도중 중복 FILE_BEGIN → 소유한 `.part` 삭제
15. 승인된 크기 초과 청크 → 소유한 `.part` 삭제
16. 부족한 크기로 FILE_END → 소유한 `.part` 삭제
17. 수신 시작 후 최종 파일명 충돌 → 기존 최종 파일 보존, `.part` 삭제
18. 빈 파일 저장·ACK·SHA-256 일치 및 종료 후 최종 파일 보존
19. 1MiB 파일을 64KiB 청크로 전송·저장·ACK·SHA-256 일치
20. 송신자가 FILE_ERROR 사유를 표시하고 승인 대기 종료

파일 수신 도중 연결 종료 시 `.part` 삭제, 정상 양방향 통신·승인·거절·파일 저장·SHA-256,
송신 정체·타임아웃·종료, 프레임 분할·병합은 1단계 회귀 테스트에서 재검증했습니다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/stage2.ps1 -Exe FileTransferApp/x64/Release/FileTransferApp.exe
powershell -NoProfile -ExecutionPolicy Bypass -File tests/stage1.ps1 -Exe FileTransferApp/x64/Release/FileTransferApp.exe
```

로그:

- 새 테스트: `tests/runs/7df8e6f4033a4a66b6f72051e22777f2/`
- 회귀 테스트: `tests/runs/f73e47fbb78a4ee19eb6fefd11e453bd/`

실제 두 노트북 간 LAN/Wi-Fi, 디스크 용량 소진에 따른 WriteFile/flush 실패,
삭제 권한이 변경된 경우의 정리 실패, 강제 프로세스 종료는 이번에 주입 테스트하지 않았습니다.
프로세스 강제 종료로 남은 기존 `.part`는 자동 삭제하지 않습니다.
SHA-256은 테스트에서 저장 결과를 비교하며 프로토콜 내부 해시 검증은 아직 없습니다.
