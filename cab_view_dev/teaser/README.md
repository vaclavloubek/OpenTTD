# Teaser video (Shorts / Reels)

Recording helper only, not meant for cab-view: `src/cab_view_rec.cpp` runs the game on a virtual clock
(one redraw interval per drawn frame) when `REC_SCRIPT` is set, moves/clicks the mouse by frame number and
pipes every frame to `REC_PIPE`. Smooth 60 fps however slow the 3D drawing is.

    Xvfb :99 -screen 0 2160x3840x24 &
    REC_SCRIPT=final.txt REC_PIPE="ffmpeg -f rawvideo -pix_fmt bgra -s 2160x3840 -r 60 -i - -vf trim=start_frame=400,setpts=PTS-STARTPTS -c:v libx264 -crf 8 -pix_fmt yuv444p raw4k.mp4" \
      DISPLAY=:99 ./openttd -c rec.cfg -g kabina3d_mosty.sav -s null -m null -v sdl:no_threads
    python3 post.py <dir with raw4k.mp4 and q.png> teaser.mp4

`q.png` is a frame of a run with `60 news ?` (the "?" glyph in the game font). Train is vehicle 7 in
`kabina3d_mosty.sav`; the run is deterministic.
