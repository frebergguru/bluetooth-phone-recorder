# Documentation

- **[User guide](user-guide.md):** setting up the adapter, pairing, calls,
  the mixer, recording, preferences, shortcuts and troubleshooting.
- **[OBS guide](obs-guide.md):** using Phone Recorder's audio in OBS Studio and
  starting OBS recordings from Phone Recorder.
- **[Building](../BUILD.md):** build, install, release and clean from scratch.
- **[Accessibility](../ACCESSIBILITY.md):** accessibility statement in English
  and Norwegian.

The screenshots in `images/` are made automatically from the real app with
demo data (no phone, no Bluetooth adapter and no sound devices are used):

```sh
scripts/make-doc-screenshots.sh     # app pictures
scripts/make-obs-screenshots.sh     # OBS pictures (needs OBS Studio)
```

Both run off-screen in a sealed virtual session and leave your desktop alone.
