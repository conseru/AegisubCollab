# Native collaboration prototype

This branch adds **Subtitle → Collaborate…** (Ctrl+Shift+J) directly to Aegisub.
It is source code for a prototype, not a verified Windows release yet.

## Use after building

1. Both people run the same build and connect to the same Hamachi network.
2. The host opens the ASS file, opens Collaborate, enters their name, their own
   Hamachi IPv4 address and a shared room password, then clicks **Host room**.
3. The guest enters their name, the host's Hamachi IPv4 address and that password,
   then clicks **Join room**. Joining saves a separate ASS backup first and opens
   the host's subtitles in the current window.
4. Close the collaboration window and edit normally. Open it again to see all
   connected user names, follow another collaborator's playhead, review the
   latest join/disconnect notice, or disconnect.
5. If the host has a video open, the joiner can automatically download and open
   that exact room video when their currently loaded video does not match the
   host filename and file size. The option is enabled by default in Collaborate.

The host listens on TCP port 49215, bound specifically to the entered address.
Allow that app/port through Windows Firewall for the Hamachi connection when
needed. This protocol relies on Hamachi for encryption and is intended for a
trusted VPN; it does not provide its own TLS or a public Internet service.
The room ends when the host closes Aegisub or disconnects.

Edits are checked after a short pause (250 ms), with a 10-second fallback scan.
Only changed subtitle documents are transmitted. Heartbeats run separately.
Subtitles, timing, inserted/copied lines, explicit deletions, styles and script
coordinate settings are shared. Each line receives a persistent identity. Lines created during a room also store
their collaboration author, and locally changed lines store the last editor.
Aegisub shows Author, Last edit, and Live columns without rewriting the Actor field.

Both people should install the same fonts. The host's currently opened video
can now be transferred directly to a joiner over the existing Hamachi room
connection and opened automatically. Downloads are saved under the Aegisub
user-data directory in `collaboration-media`. Standalone audio files,
playback position, attachments, and other scripts' private extradata are not
transferred. Shared PlayRes/LayoutRes and style settings preserve the subtitle
coordinate system; they do not rescale one person's video into a different
video.

Video transfers are chunked so subtitle updates can continue on the same TCP
connection, and are capped at 32 GB. The receiver checks the announced and
written byte count before opening the file. This first media-transfer version
does not yet provide resume support or a cryptographic checksum; interrupted
partial downloads are removed and both peers should remain on a trusted
Hamachi network.

## Data preservation and current limits

- Distinct lines stay distinct even when they contain identical text. Rows are
  matched by persistent identities, never by row number or text alone.
- Unrelated concurrent edits merge. Conflicting edits to the same field,
  timing, style, or coordinate settings pause by disconnecting the affected
  editor and retain that editor's local document. There is no conflict-resolution
  dialog yet: save both versions before resolving and rejoining.
- Moving/sorting existing lines during a session is currently rejected safely.
  Sort before hosting. Undoing a previously synced deletion may also conflict
  with the deletion tombstone. Local Undo can undo received edits; this prototype
  does not yet implement per-author collaborative undo.
- Joining intentionally opens the room's document; it does not combine two
  independently started projects. The before-join backup is under the Aegisub
  user data directory in `collaboration-backups/before-join-<id>.ass`.
- Opening another subtitle file disconnects the session. Aegisub's usual Save
  and autosave still apply; a room is not a cloud backup.
- Limits: eight guests, 20,000 lines, 2,000 styles and an 8 MB document. The host
  retains 32 revisions; a client older than that must reconnect after saving
  its local work. Negative numeric fields and unsupported ASS field content
  are rejected rather than silently rewritten.

## Build and verification

Use the repository's normal Meson build instructions. wxWidgets networking is
now required in addition to its existing components. No Lua collaboration
script, Python server, or batch launcher is required by users.

The existing **Meson CI** workflow also runs on `collab/**` branches. Its Windows
job builds and uploads the portable ZIP. It can also be run manually against
this branch. No Windows artifact has been produced or tested in this workspace.

Standalone merge checks:

```sh
c++ -std=c++20 -Wall -Wextra -Werror src/collaboration/document.cpp \
  tests/collaboration_core.cpp -o collaboration-core
./collaboration-core
```

Standalone loopback networking checks (Linux with wxWidgets development files):

```sh
c++ -std=c++20 $(wx-config --cxxflags base,net) -DwxUSE_GUI=0 \
  src/collaboration/document.cpp tests/collaboration_transport.cpp \
  $(wx-config --libs base,net) -o collaboration-transport
./collaboration-transport
```

Before sharing a release, run the full app build and exercise two Windows PCs:
join/backup; simultaneous edits on different lines; same-field conflict;
copy/paste above and below the original; deletion versus edit; changed styles
and resolution; disconnect/reconnect; host shutdown; save/reopen; and Undo.
Check subtitle grid selection and video rendering during remote edits.


## Collaboration v2 UI

The Collaborate dialog now has two tabs. **Collaborate** keeps the room controls,
shows every connected user, provides a playhead-follow selector, and keeps a
small recent-event line. Join and disconnect events also appear temporarily in
Aegisub's status bar rather than interrupting work with a popup.

**YTSub Reference** contains an in-app reference for the YTSubConverter ASS
features and YouTube-specific override tags used by this project. It includes
mobile/PC caveats and links to the upstream font chart and YTSubConverter.

The subtitle grid adds **Author**, **Last edit**, and **Live** columns. Author is
set for lines created during a collaboration room. Last edit updates whenever a
collaborator changes that line. Live lists users whose active line is that row.
Presence and playhead updates are ephemeral room state and are not written into
subtitle text.


## v3 collaboration coordination

The next collaboration protocol adds editor/viewer roles, automatic guest
reconnection, room chat and line notes with @mentions, a conflict decision
dialog, a guarded "Undo my last synced edit" action, per-user colors, separate
viewing/typing presence, and colored remote playhead markers on the video
slider. Viewers receive room changes but cannot submit subtitle edits.
