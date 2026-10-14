# YouTube video: 3D first person view (2026-10-14)

Horizontal 4K/60 fps video (also 1080p), ~80 s: the map and the cab window in 2D, the 3D switch on the beat,
then rail, road, water and air, ending with the in-game newspaper "Alpha coming soon" and an end card.

Recording uses the frame-by-frame helper from the teaser (`src/cab_view_rec.cpp`, REC_SCRIPT/REC_PIPE, not for
cab-view). New script commands: `cab <vehicle>`, `3d`, `zoomin`, `close toolbar|status`.
Keep the status bar (move it off screen with `win status 0 2400 3840 48`): with it closed, no news window appears.
Runs are deterministic per script, but not between scripts that differ (closing the status bar shifted timings),
so every clip is cut from the run it was recorded in.

    Xvfb :98 -screen 0 3840x2160x24 &
    REC_SCRIPT=rec/mosty.txt REC_PIPE="ffmpeg -f rawvideo -pix_fmt bgra -s 3840x2160 -r 60 -i - -c:v libx264 -crf 12 mosty.mp4" \
      DISPLAY=:98 ./openttd -c rec4k.cfg -g kabina3d_mosty.sav -s null -m null -v sdl:no_threads
    python3 edit.py <recordings> <out>          # body.mp4, end.mp4, sfx.json ("preview" as 3rd arg: 720p)
    python3 music.py <out>/a.wav <OpenSFX src/wav> <out>/sfx.json
    python3 thumb.py <dir with frames> thumb.png

`rec4k.cfg` is `../teaser/rec.cfg` with `resolution = 3840,2160` (gui_scale 400). Scripts for the runs are in `rec/`.
Saves: /mnt/project-files/cab-view/mac_test/kabina3d_{mosty,kopce,autobus,lod,more,letadlo}.sav.
Music: own, numpy synth, 112 BPM, every cut on a bar line. Sounds: OpenSFX (github.com/OpenTTD/OpenSFX, src/wav).
