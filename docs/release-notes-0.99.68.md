**A stop no longer waits on the recorders when nothing is recording, and the log says who asked for it.** STOP, the Start/Stop key, the scope's POWER key, and a stop from the web remote or a plugin used to end both recorders even when neither was running, and each of those steps waited for the signal thread to finish the block it was on - a block a busy plugin can stretch to hundreds of milliseconds. The 0.99.65 crash report this comes from showed a 740 ms stop four seconds into a session with nothing recording, eight seconds before the program died by a fast-fail that no report has yet explained. Now an idle recorder is left alone, and every stop writes one line in the diagnostic log naming what asked for it, what it ended and how many milliseconds ending it took, so the next such report says why the receiver stopped and not only what it cost.

**With the browser remote on, the window no longer waits for the signal thread's block every frame it hands audio to the browser.** Each frame that had new audio copied it out of a short rolling window that sat behind the same lock the signal thread holds for a whole block, so the window stopped drawing until that block was done. It turned up while testing the stop change: with a block stretched to a second and a half, as a busy plugin can, a single frame waited a second and a half inside that copy and nothing else. The window now has a lock of its own, which the signal thread holds only while it writes a block's samples into it, so the browser gets exactly the same audio and the window no longer queues behind the block.

Still not fixed, and known: the two fast-fail deaths reported from the field (0.99.64 on an RSP1 after 48 minutes, 0.99.65 on an RTL-SDR after 12 seconds) are not explained - since 0.99.66 the sentinel names the faulting module for the next one; the console window still opens beside FoxSDR on Windows; a recording whose disk disappears part way ends as an empty file with nothing on screen saying so; an MP3 patch speaker whose folder is removed says nothing; a log file held open by another program at start-up is silently switched off.

---

**SHA-256 of `foxsdr-setup-0.99.68.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.68-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.68-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.68-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.68-linux-arm64.tar.gz`:**
`TBD`
