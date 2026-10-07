# Releasing extras

How an `extras-vN` release on GitHub is made. The `extras` branch is
upstream `aqtion` plus the feature branches merged into it with
`--no-ff`; a feature starts as its own branch (off `origin/aqtion`, or
off the branch it builds on) and is merged into `extras` when done.

## Versioning

- **A new feature is a new version.** Anything users would notice -
  a command, a menu, a key - is `extras-v(N+1)`, never a retag of the
  last one. The tag is what a user's `version` cvar reports and what
  a bug report quotes; a moved tag makes two different clients say the
  same thing.
- **A re-release of the same version** is only for a broken build of
  the same content: a packaging slip, a missing file, caught within
  hours. See the end of this file.

## Steps

1. Everything merged into `extras`, built, tried, and **pushed**; the
   tree clean. Note the head's short sha (`git rev-parse --short HEAD`).

2. Tag it and push the tag:

       git tag extras-vN
       git push origin extras-vN

3. Rebuild both clients **after** tagging. The version string comes
   from `version.py` at meson configure time, so touch `meson.build`
   to make it run again, or the binaries keep saying `~zextras-v(N-1)+5`:

       touch meson.build
       ninja -C build
       ./build-windows.sh

   Check both read `~zextras-vN` with no `+M`:

       strings build/q2pro | grep -m1 'r[0-9]*~'
       strings build-windows/q2pro.exe | grep -m1 'r[0-9]*~'

4. Package. Two assets, named by the short sha, each the client
   renamed `q2prox`, its game library renamed `gamex86_64x`, the fonts
   and the jmod marker; everything stripped:

       q2pro-extras-<sha>-linux-x86_64.tar.gz
           q2prox                          build/q2pro, strip
           action/gamex86_64x.so           build/gamex86_64.so, strip
           action/fonts/                   NotoSans-Regular.ttf, NotoSans-Bold.ttf, OFL.txt
           action/pics/jmod_spawn.png
       q2pro-extras-<sha>-windows-x86_64.zip
           q2prox.exe                      build-windows/q2pro.exe, x86_64-w64-mingw32-strip
           action/gamex86_64x.dll          build-windows/gamex86_64.dll, x86_64-w64-mingw32-strip
           action/fonts/, action/pics/     as above

   Stage each in an empty directory and `tar czf` / `zip -r` from
   inside it, so the archives unpack straight into the AQtion folder.

5. Notes. Take the previous release's body (`gh release view
   extras-v(N-1) --json body -q .body`), put a `## New in vN` section on
   top of the `## New in` sections, one `###` per feature area and a
   bullet per thing a user can do or will notice, and update the
   `Built from` paragraph's sha. The intro paragraph lists the headline
   features; extend it when vN adds one. The install, menu and cvar
   reference below the `New in` sections is kept current, not
   duplicated.

6. Create the release on the pushed tag. Give **no `--target`**: with
   the tag already on GitHub a short sha is refused (`422
   target_commitish is invalid`).

       gh release create extras-vN --title "Extras vN" -F notes.md \
           q2pro-extras-<sha>-linux-x86_64.tar.gz \
           q2pro-extras-<sha>-windows-x86_64.zip

7. Check it: `gh release view extras-vN` lists both assets, and
   `gh release download extras-vN -D /tmp/x` unpacks to the layout above.

## Re-releasing the same version

Only for a broken build of the same content (see Versioning).

    gh release delete extras-vN --cleanup-tag --yes   # release and tag, remote and local
    git tag extras-vN <sha>
    git push origin extras-vN

then steps 3 to 7 again. Do not `git fetch --prune` with a tags refspec
to tidy up: it deletes local tags the remote never had.
