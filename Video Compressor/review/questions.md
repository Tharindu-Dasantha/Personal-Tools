# Performance and security review: questions

These are the decisions to make before the fixes in [fixes.md](fixes.md) can go in. The numbers in brackets point to [findings.md](findings.md).
Each question has a recommended answer. Fixes 1, 2, 6, 8, 9, 10 and 13 need no answers and can go in as they are.

## Q1. Two videos with the same name: flag the clash, or give them separate names? (#3)

`clip.mov` and `clip.mp4` both write `compressed/clip.mp4`, so the second one can be trashed against the first one's copy.

- **A (recommended):** keep today's names. The second video shows **Name clash**, no Trash is offered for it, and you rename one of the two and run again.
- **B:** give twins their full names, as images do (`compressed/clip.mov.mp4` and `compressed/clip.mp4.mp4`). Both compress and both can be trashed. Earlier outputs of twins are no longer recognised and get encoded again. This changes output names, which SPEC.md defines.

Which one?

## Q2. Image size cap: what limit? (#5)

The proposal: refuse images above **200 megapixels** (about 800 MB of RAM in `pngquant`), and cap ImageMagick's disk cache at **8 GiB**. A refused image shows as **Unreadable**, with "Too large: W×H" in its tooltip.

- Is 200 MP right for your photos? Panoramas and stitched scans can go above 100 MP.
- Is "Unreadable" an acceptable label, or do you want a separate **Too large** pill? That would take a few more lines in `render_row()`.

## Q3. Decode the end of each copy before trashing its original? (#4)

Today the check reads headers only. Decoding the last 2 seconds would catch a copy that is cut short or corrupt at the end, and adds about 1 s per video before the original moves to Trash.
**Recommendation:** skip it. ffmpeg must exit 0 and the rename is atomic, so the gap is small, and the Trash makes everything restorable. Say if you want it anyway.

## Q4. Change the build so the installed app is a release build? (#7)

The installed `~/.local/bin/video-compressor` is a Debug build (`-O0`, no hardening flags). The proposal:
- `CMakeLists.txt` defaults to `RelWithDebInfo` and adds `-fstack-protector-strong` everywhere, plus `_FORTIFY_SOURCE=3` outside Debug.
- You install from a new `build/release/` folder.
- `build/` stays the Debug test build, so the commands in HANDOFF.md still work.

HANDOFF.md would get one extra line with the install command. OK?

## Q5. How many files are in your biggest folders? (#11)

The list builds every row up front. A few thousand rows are fine once fixes 8 and 10 are in. If you often open folders with **10,000+ images**, the list should move to `GtkListView`, which is a large rewrite. Otherwise, leave it.

## Q6. How do you want the fixes committed?

- One commit per fix (recommended: each one can be reverted on its own), or one commit for the whole review?
- HANDOFF.md says you choose the commit date. Which date should these commits use?
- HANDOFF.md still says Phase 10 is "Not committed yet", but git shows it committed as `8921588`. Should I update HANDOFF.md along with the fixes?
