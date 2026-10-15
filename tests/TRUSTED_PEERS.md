# 등록 PC 자동 연결

## 사용 순서

1. 양쪽 PC에서 이 수정본 GUI를 실행한다. 기존 v1.0.0 ZIP에는 이 기능이 없다.
2. A의 LAN 목록에서 B를 선택한다. 등록 키 칸을 비운 채 **선택 PC 등록**을 누르면
   임의의 64자리 소문자 16진수 키가 생성되고 A에 B의 IPv4/키가 저장된다.
3. 생성된 키를 상대 PC 사용자에게 신뢰할 수 있는 별도 경로로 전달한다.
   공개 게시판, 화면 공유, 발견된 미확인 상대와의 대화로 전달하지 않는다.
4. B의 LAN 목록에서 A를 선택하고 같은 키를 붙여 넣은 뒤 **선택 PC 등록**을 누른다.
5. 한쪽에서 **선택 기기 연결 요청**을 누른다. 양쪽 키가 맞으면 상대 승인 없이 연결한다.
   등록된 IPv4에 대한 **IP 직접 연결 요청**도 같은 인증을 사용한다.
6. 파일·대화는 기존과 같이 양방향으로 사용한다. 수신 파일은 설정된 폴더로 자동 저장한다.
7. 연결을 해제하고 상대를 목록에서 선택한 뒤 **선택 PC 해제**로 등록을 지운다.
   양쪽에서 해제하면 이후 연결은 기존 수동 승인 방식이다.

키가 다른 PC의 등록을 덮어쓰지 않도록 PC 쌍마다 별도 키를 생성한다.
현재 등록은 IPv4별로 키를 선택한다. DHCP 등으로 IP가 바뀌면 예전 IP 등록을 해제하고
양쪽에서 새 IP로 재등록해야 한다. 동일 IP를 사용하는 여러 PC에는 적용할 수 없다.
IP만 같다고 자동 승인하지는 않는다. 해당 등록 키를 실제로 증명해야 한다.
등록은 연결 자동 승인에 대한 허용이므로 해당 상대가 파일도 자동으로 보낼 수 있다.

## 구현

- `TrustedPeers.h`: BCrypt 난수 256비트 키, HMAC-SHA256 방향별 인증, DPAPI 저장.
  파일은 `GuiSettings::SettingsFile()`에 `.trusted`를 붙인 경로다.
  기본 경로는 `%LOCALAPPDATA%/FileTransferApp/download-folder.txt.trusted`다.
  Windows 사용자 계정 범위 DPAPI로 보호하고 임시 파일→원자적 교체로 저장한다.
  잘못된 형식, 중복 IP, 64개 초과, 복호화 실패는 자동 허용 권한을 부여하지 않는다.
- `LanService`: 기기 이름/실행마다 바뀌는 발견 ID는 인증 수단으로 사용하지 않는다.
  등록 IP에 대한 요청만 새 인증 greeting을 사용한다. 등록/해제와 파일 저장은
  Controller 작업 스레드에서 수행하며 네트워크 소켓은 기존 nonblocking 방식을 유지한다.
- `Gui.cpp`: 등록 키 입력, 선택 PC 등록/해제 및 안내를 추가한다. 새 행 높이를
  스크롤 범위와 자식 좌표에 함께 반영한다. 기존 영역별 갱신 방식을 유지한다.
  연결/승인 대기 중에는 등록 편집을 비활성화한다. 등록 취소는 진행 중 연결을 끊는
  기능이 아니며, 현재 연결을 끝내려면 **연결 해제**를 사용한다.

## 인증 및 호환성

등록된 요청자는 `FTA-C2|instanceId|clientNonce|name`을 보낸다.
수신자는 IP에 대응하는 키를 찾아 새 serverNonce를 생성하고
`FTA-H2|receiverId|serverNonce|serverProof`를 반환한다.
transcript는 C2 greeting 전체 + `|receiverId|serverNonce`다.
serverProof는 `HMAC(key, "server|" + transcript)`다.
요청자는 수신자 증명을 먼저 검증하고 `FTA-P2|clientProof`를 보낸다.
clientProof는 `HMAC(key, "client|" + transcript)`다.
수신자 검증 후 `FTA-A2|receiverId|clientNonce|acceptedProof`를 보내고 기존 socket을
Network에 넘긴다. acceptedProof는 `HMAC(key, "accepted|" + transcript)`다.
새 난수와 방향별 접두사는 인증 응답 재사용/반사를 막는다. 키 자체는 전송하지 않는다.

잘못된 키, 미등록 C2, 잘못된 형식은 연결을 닫고 오류를 기록한다.
키가 등록된 송신자는 인증 실패 시 자동으로 구형 수동 프로토콜로 내려가지 않는다.
미등록 송신자/구버전은 기존 C1 수동 승인을 유지한다. 등록 IP에서 C1이 와도
수동 승인 없이 연결하지 않는다. 구버전과 수동 연결하려면 해당 IP 등록을 해제한다.
UDP 발견 v1과 파일 프로토콜 v6, BEGIN/CHUNK/END·이어받기·해시·완료 ACK는 바꾸지 않았다.
이 인증은 최초 연결의 공유 키 확인이다. 전송 내용을 암호화하거나 각 파일 프레임을
인증하는 TLS 기능을 추가한 것은 아니므로 신뢰할 수 있는 LAN에서 사용한다.

## 검증

`tests/trusted_peers.ps1`은 격리된 GUI 두 개와 별도 raw TCP peer를 사용한다.
등록 fixture는 루프백 멀티캐스트를 사용해 두 프로세스에 모두 전달한다.
실제 키 대신 run마다 생성한 시험 키를 사용한다. UTF-8 BOM은 Windows PowerShell 5.1
한글 문자열 해석을 위해 유지한다.

신규 14개: 잘못된 키, 난수 생성, 암호화 저장, 자동 연결, 양방향 대화,
파일 SHA-256/ACK, 재시작, 키 불일치, C1 신원 사칭 시 수동 승인 유지,
독립 HMAC 검증, 이전 증명 replay 거부, 등록 해제, 미등록 수동 승인, 손상 저장소.

2026-10-15 최초 Debug 증분 빌드에서 신규 14/14 PASS, Debug/Release x64 빌드 성공.
전체 runner에 구성별 14개를 추가해 목표 602개로 늘렸다.
그 후 Debug 전체 runner의 Rebuild도 성공했으나, 해당 산출물의 GUI와 CLI 모두
Windows Application Control에 차단되어 **0/301 PASS, 301개 실행 차단**으로 종료됐다.
첫 증분 빌드에서 통과한 신규 14개를 최신 Rebuild 산출물의 통과 수로 합산하지 않는다.
Release GUI는 미실행이고 전체 Release 301개도 이번 작업에서는 미검증이다.
전체 602개 회귀가 통과한 것은 아니며 현재 Debug 파일도 실행 가능하다고 보장할 수 없다.
CodeIntegrity 로그에는 3118/3077 등의 보안 차단 이벤트가 기록돼 있다.
차단 후 같은 실행 파일을 재시도하거나 이름 변경/재빌드로 실행 차단을 우회하지 않았다.

최신 Debug GUI SHA-256:
`372F781E2ACA93225B1FC1ABC9039B4CAFFAD1C349BF832AAC8FF3B37F1B04BA`.
최신 Debug CLI SHA-256:
`D0A1EEF7D058B40695CAB58FE52426D61F6F2EAEE22AE89ED5FBAC97FAAD92A1`.
전체 실행 로그와 suite별 CodeIntegrity 자료:
`tests/runs/release-validation-30d635d903e34620b2edd0d3ee87d6ad/`.

개발 중 최초 신규 suite는 BOM 없는 한글 스크립트 해석 때문에 중단됐고,
그 다음에는 SO_REUSEADDR UDP 소켓에 대한 unicast 시험 패킷이 한 GUI에만 전달되어
중단됐다. UTF-8 BOM 및 루프백 멀티캐스트로 시험 도구를 수정한 실행에서 14개 통과했다.
실패 로그/실행 디렉터리도 보존하며 실패 횟수를 통과 수에 합산하지 않는다.
Release GUI는 직전 빌드에서 Application Control 차단이 확인되어 이번에 실행하지 않았다.
정책 변경이나 우회는 하지 않았다. 이전 커밋의 검사 수를 이번 통과 수로 합산하지 않는다.

로그: `tests/runs/trusted-peers-Debug.log`, `trusted-build-{Debug,Release}.log`,
`trusted-regression-Debug.log`. 각 실행의 상세 자료는 로그의 run directory에 보관한다.

## 남은 수동 확인

- 실제 두 PC에 키를 전달·등록하고 한쪽 요청만으로 자동 연결 및 파일 전송.
- 양쪽 IP 변경/오래된 등록 해제, 다른 Windows 계정에서 설정 복사 시 자동 허용 거부.
- 구버전 PC는 등록 해제 후 수동 승인으로 연결.
- DPI 100/125/150%, 빠른 스크롤·리사이즈의 시각적 잔상, 실제 IME 입력.
- 정상 허용된 환경에서 Release GUI 및 전체 Release 회귀.
