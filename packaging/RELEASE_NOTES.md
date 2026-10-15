# FileTransferApp v1.0.0 배포 후보

Windows x64용 Win32 GUI 파일·텍스트 전송 앱입니다.

- 같은 LAN의 앱 발견 및 상대의 명시적 연결 승인
- Unicode 메시지·파일, 파일별 승인/거절, 송수신 취소
- SHA-256 검증·저장 완료 ACK, 이어받기·완료 조회
- 수신 폴더 저장, 작은 창의 세로 스크롤 갱신 보완

## 다운로드와 실행

`FileTransferApp-v1.0.0-win-x64.zip`과 `SHA256SUMS.txt`를 받으세요.
ZIP을 모두 압축 해제하고 `FileTransferAppGui.exe`를 실행합니다.
Visual Studio/Git은 필요하지 않습니다. 최신 Microsoft Visual C++ v14 x64 Redistributable이 필요합니다.
사용 순서와 저장 위치는 ZIP의 `README.txt`를 확인하세요.

## 제한

EXE는 코드 서명되지 않았습니다. SmartScreen/SAC/회사 정책에 따라 차단될 수 있습니다.
체크섬은 코드 서명을 대신하지 않으며 보안 정책을 끄거나 우회하지 마세요.
실제 두 PC 간 LAN·파일 전송은 이번 배포 준비에서 검증하지 않았습니다.
실제 휠/드래그의 시각적 잔상, 100/125/150% DPI, IME/drop은 수동 검증이 필요합니다.
프로토콜과 기존 전송·승인 정책은 변경하지 않았습니다.
별도 오픈 소스 라이선스를 부여하지 않았으며 `LICENSE.txt`의 권리 보유 안내를 확인하세요.

자동 검증은 워크플로의 `validation-logs` 결과를 확인하세요. 테스트가 실행되지 않거나 실패하면 배포하지 않습니다.
이 문서는 후보 Release 초안이며 공개 게시를 의미하지 않습니다.
