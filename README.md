# glass-music
내가쓸라고만듦
## 빌드 및 실행 방법 
### 서비스 설치
1. 서비스 파일을 `~/.config/systemd/user/`로 옮기세요.
2. 서비스 파일을 텍스트 에디터로 열어 *처리된 부분을 자신의 사용자 이름으로 변경하세요.
   ```
   ExecStart=/home/*/glass_music/bin/glass-music
   -> ExecStart=/home/사용자 이름/glass_music/bin/glass-music
   ```
3. 터미널을 열어 `systemctl --user daemon-reload`를 입력하세요.
4. 다음 명령어를 입력하세요.
```
systemctl --user enable glass-music
systemctl --user restart glass-music
```
### 프로그램 설치
1. 아래의 명령어만 입력하세요
``` bash
make install
systemctl --user restart glass-music
```

짠~ 겁나게 쉽죠?
