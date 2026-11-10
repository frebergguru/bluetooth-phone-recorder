# Documentation

- **[User guide](user-guide.md):** setting up the adapter, pairing, calls,
  the mixer, recording, preferences, shortcuts and troubleshooting.
- **[OBS guide](obs-guide.md):** using Phone Recorder's audio in OBS Studio and
  starting OBS recordings from Phone Recorder.
- **[Building](../BUILD.md):** build, install, release and clean from scratch.
- **[Accessibility](../ACCESSIBILITY.md):** accessibility statement in English
  and Norwegian.

Also available as standalone files, with all pictures inside:

| Guide | PDF (A4) | HTML |
|-------|----------|------|
| User guide | [user-guide.pdf](pdf/user-guide.pdf) | [user-guide.html](html/user-guide.html) |
| OBS guide | [obs-guide.pdf](pdf/obs-guide.pdf) | [obs-guide.html](html/obs-guide.html) |

## På norsk

- **[Brukerveiledning](nb/user-guide.md)** ([PDF](pdf/nb/user-guide.pdf), [HTML](html/nb/user-guide.html))
- **[OBS-veiledning](nb/obs-guide.md)** ([PDF](pdf/nb/obs-guide.pdf), [HTML](html/nb/obs-guide.html))

Skjermbildene i `images/nb/` viser appen på norsk.

The screenshots in `images/` are made automatically from the real app with
demo data (no phone, no Bluetooth adapter and no sound devices are used):

```sh
scripts/make-doc-screenshots.sh     # app pictures
scripts/make-obs-screenshots.sh     # OBS pictures (needs OBS Studio)
```

Both run off-screen in a sealed virtual session and leave your desktop alone.

The PDF and HTML files are made from the Markdown guides with:

```sh
scripts/make-doc-exports.sh
```

PDFs need WeasyPrint (`python3 -m venv ~/.venv/weasy && ~/.venv/weasy/bin/pip install weasyprint`,
then run with `BPR_WEASYPRINT_PYTHON=~/.venv/weasy/bin/python`). Without it, only
the HTML files are made.
