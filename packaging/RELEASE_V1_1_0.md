# v1.1.0 배포 기록

- 기준: `feature/chat-auto-receive`, 기능 변경 기준 커밋 `01bb85c933287fb09c61b7d94b50645ed3bb2e26`.
- 변경: AppVersion 1.1.0, README·사용자 안내·Release Notes, v1.1.0 태그의 자동 회귀 실행 제외.
- 범위: 기존 1:1 양방향 파일/텍스트, 대화 창·자동 수신·공유 키 PC 등록·별도 메타데이터 저장.
- N:1 기능, main, 기존 v1.0.0 태그/Release/asset은 변경하지 않는다.
- 2026-10-15 Release x64 GUI 최소 빌드 성공. 추가 테스트·2PC·ZIP 실행 검사는 사용자 요청으로 생략.
- 빌드 로그: `tests/runs/release-v1.1.0-build.log` (로컬 산출물, Git 제외).
- 패키징: 기존 `tools/package-release.ps1`, 출력 `dist/v1.1.0`.
- ZIP: `FileTransferApp-v1.1.0-win-x64.zip`.
- 포함 파일: `FileTransferAppGui.exe`, `README.txt`, `LICENSE.txt`.
- SHA-256: `1c7c6d5f19a518cd4c62c734b0b457a9937ca84ca412d9aea305c4a9c20fbb11`.
- 체크섬 asset: `SHA256SUMS.txt`.
- 게시 위치: https://github.com/royjwseo/FileTransferApp/releases/tag/v1.1.0
- 일반 Release로 게시하며 Draft/Pre-release가 아니다. 기존 버전 asset을 덮어쓰지 않는다.

패키징 스크립트의 입력 파일 존재·PE 버전·타임스탬프 확인과 체크섬 생성은 패키지 생성 과정이다.
앱을 실행하거나 전체 회귀를 검증하지 않았으며 기존 실패/차단 기록을 변경하지 않는다.
환경별 보안 차단·실제 2PC·DPI/IME/drop 등 미검증 사항은 `packaging/RELEASE_NOTES.md`에 기재한다.
