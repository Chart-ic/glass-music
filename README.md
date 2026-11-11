# Glass Music Overlay (글래스모피즘 뮤직 오버레이 v1.1.0)

KDE Plasma Wayland 환경에서 재생 중인 음악(MPRIS2)을 시스템에서 실시간으로 가로채어 화면 최상단에 미려한 **글래스모피즘(Glassmorphism)** 캡슐 HUD로 띄워주는 **순수 네이티브 C(Wayland Layer-Shell + KWin Blur)** 프로그램입니다.

Figma 디자인(`Frame 2.svg`) 및 제공해주신 스크린샷과 동일한 레이아웃, 타이포그래피, 앨범 아트(64×64, rx=8), 글래스 하이라이트 및 실제 KWin 하드웨어 블러 효과를 구현하였습니다.

---

## 추가된 주요 기능 (New Features)

1. **등장 및 퇴장 애니메이션 (Smooth Spring & Slide Animation)**
   - HUD가 나타날 때 상단 베젤에서 부드러운 Ease-out 스프링 바운스(Bounce) 효과와 함께 슬라이드 다운됩니다.
   - 곡 정지 또는 자동 숨김 시 상단으로 부드럽게 페이드 아웃되며 슬라이드 업 퇴장합니다.

2. **유동적 크기 변경 & 마퀴 라벨 효과 (Dynamic Pill Resizing & Marquee Label)**
   - 곡명과 아티스트 이름 길이에 맞춰 캡슐 HUD의 가로 너비가 60fps로 부드럽게 실시간 모핑(Interpolation)됩니다.
   - 곡 제목이나 아티스트명이 특정 길이(`max_text_width: 240px`)를 넘어가면 자동으로 **마퀴 라벨(Marquee Scrolling)** 효과가 작동합니다.
   - 좌우 엣지에 부드러운 알파 디졸브(Fade-out) 그라데이션이 적용되어 글자가 잘리지 않고 자연스럽게 롤링됩니다.

3. **"Not Playing" 유휴 상태 모드 (Not Playing State)**
   - 음악이 멈추거나 재생 중이 아닐 때, 유저 첨부 스크린샷과 동일하게 좌측 프로스티드 반투명 큐브 아이콘과 우측의 깔끔한 단일 라인 **"Not Playing"** 캡슐 형태로 즉시 변경됩니다.
   - 트레이 옵션의 `Always Show` 설정으로 음악이 꺼져 있을 때도 시계/위젯처럼 항상 띄워둘 수 있습니다.

4. **시스템 트레이 및 네이티브 폰트 설정 (System Tray & Font Chooser)**
   - KDE 작업표시줄 시스템 트레이(StatusNotifierItem)에 상주하여 언제든 설정을 변경할 수 있습니다.
   - 트레이 아이콘을 클릭하거나 우클릭하면 **Font & Display Settings** 창이 열리며, 곡 제목(Title Font), 아티스트(Artist Font), Not Playing 상태 폰트를 네이티브 `GtkFontChooserDialog`로 직접 선택할 수 있습니다.
   - 설정한 폰트와 여백, 마퀴 속도 등은 `~/.config/glass_music/config.ini`에 자동 영구 저장되며 실시간으로 HUD에 반영됩니다.

5. **클릭 패스스루 (Wayland Input Region Masking)**
   - 화면 상단에서 캡슐 외부의 투명 여백 영역은 마우스 클릭이 그대로 아래 창(브라우저 탭, Figma, Discord 등)으로 통과되도록 동적 입력 영역(`wl_surface_set_input_region`)이 적용되어 작업 흐름을 방해하지 않습니다.

---

## 빌드 및 실행 (Build & Run)

### 빌드 (Build)
```bash
cd /home/chartic/glass_music
make
```
컴파일 완료 시 `bin/glass-music` 바이너리가 생성됩니다.

### 실행 방법 (Usage)

#### 1. 일반 실행 (시스템 음악 실시간 감지 모드)
```bash
./bin/glass-music
```
Spotify, 크롬, 파이어폭스(유튜브/유튜브 뮤직), Discord 등에서 음악을 틀면 상단에 실시간으로 글래스 HUD가 등장합니다.

#### 2. 테스트 / 데모 모드 (첨부된 Frame 2.svg 곡으로 즉시 확인)
```bash
./bin/glass-music --test
```
`Frame 2.svg`에 포함된 **HIMEHINA - キスキツネ** 곡 정보와 앨범 아트로 오버레이를 화면에 즉시 띄웁니다.

#### 3. 마퀴 라벨 데모 모드 (Marquee Scrolling Demo)
```bash
./bin/glass-music --marquee-test
```
긴 제목의 곡으로 가로 크기가 늘어난 뒤 마퀴 롤링 애니메이션이 동작하는 모습을 바로 확인하실 수 있습니다.

#### 4. 유휴 상태 데모 모드 (Not Playing State Demo)
```bash
./bin/glass-music --idle-test
```
첨부해주신 스크린샷과 일치하는 "Not Playing" 캡슐 모드를 즉시 확인하실 수 있습니다.

---

## 컨트롤 안내 (HUD Controls)

- **HUD 좌클릭**: 재생 / 일시정지 (Play / Pause) 토글
- **HUD 우클릭**: 다음 곡 (Next Track)
- **HUD 휠 클릭 (가운데 클릭)**: 이전 곡 (Previous Track)
- **HUD 마우스 휠 스크롤**: 볼륨 업 / 다운 (±5%)
- **트레이 아이콘 클릭 / 우클릭**: 폰트 및 디스플레이 설정창 열기

---

## 명령줄 옵션 (CLI Options)

| 옵션 | 설명 | 기본값 |
| :--- | :--- | :--- |
| `-m, --margin <px>` | 화면 상단으로부터의 여백(픽셀) | `25` |
| `-a, --autohide <sec>` | 일시정지 후 자동 숨김 대기 시간(초, 0 입력 시 비활성화) | `5` |
| `-s, --always-show` | 일시정지 상태에서도 'Not Playing' HUD 항상 표시 | `false` |
| `-t, --test` | 샘플 곡(HIMEHINA - キスキツネ) 데모 모드 | `false` |
| `--marquee-test` | 긴 제목으로 마퀴 라벨 효과 데모 모드 | `false` |
| `--idle-test` | 'Not Playing' 유휴 상태 데모 모드 | `false` |
| `-h, --help` | 도움말 표시 | - |
| `-v, --version` | 버전 정보 표시 | `1.1.0` |

---

## 설정 파일 (Configuration)

설정은 `~/.config/glass_music/config.ini`에 저장됩니다:

```ini
[Display]
title_font = Pretendard, Noto Sans CJK KR, Noto Sans, Sans-Serif Bold 12.5
artist_font = Pretendard, Inter, Noto Sans, Sans-Serif Bold 10
not_playing_font = Pretendard, Noto Sans CJK KR, Noto Sans, Sans-Serif Bold 13
margin_top = 25
autohide_delay_sec = 5
always_show = false
max_text_width = 240
marquee_speed = 35.0
default_art = assets/default_art.png
```

---

## 시스템 자동 시작 설정 (Autostart)

로그인 시 자동으로 실행되도록 설정하려면:

### 방법 1: 데스크톱 자동 시작 (KDE Autostart)
```bash
cp glass-music.desktop ~/.config/autostart/
```

### 방법 2: Systemd 사용자 서비스
```bash
mkdir -p ~/.config/systemd/user
cp glass-music.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now glass-music.service
```

# glass-music
