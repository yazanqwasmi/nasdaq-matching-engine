# Regenerate with: vhs docs/demo.tape   (from the repo root, after
# building: cmake --build build-rel -j)
Output docs/demo.gif
Set FontSize 13
Set Width 1000
Set Height 620
Set Theme "Catppuccin Mocha"
Set TypingSpeed 30ms

# marketsim starts on a delay so the viewer joins the feed from sequence 1
# (no mid-join gap warning in the recording).
Hide
Type "./build-rel/exchanged --port 26480 --mcast 239.192.0.20 --mcast-port 26020 >/dev/null 2>&1 &"
Enter
Type "(sleep 3; ./build-rel/marketsim --port 26480 --agents 6 --seconds 40 >/dev/null 2>&1) &"
Enter
Sleep 1s
Type "clear"
Enter
Show

Type "./build-rel/itchview --group 239.192.0.20 --port 26020"
Enter
Sleep 14s
Ctrl+C
Sleep 1s

Hide
Type "kill %1 %2"
Enter
