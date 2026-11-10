#!/bin/bash
# Builds standalone HTML and PDF versions of the guides:
#   docs/html/user-guide.html, docs/html/obs-guide.html  (images and CSS inside)
#   docs/pdf/user-guide.pdf,   docs/pdf/obs-guide.pdf
# Each HTML file has its images and CSS inside, so it works on its own.
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(sed -n "s/^ *version : '\([^']*\)'.*/\1/p" meson.build | head -1)
footer="Bluetooth Phone Recorder $version. https://github.com/frebergguru/bluetooth-phone-recorder"
work="$PWD/scratchpad/exports"
rm -rf "$work" docs/html docs/pdf
mkdir -p "$work" docs/html docs/pdf

# PDFs are made with WeasyPrint (pip install weasyprint, ideally in a venv).
# Point BPR_WEASYPRINT_PYTHON at a Python that has it, if not the system one.
py="${BPR_WEASYPRINT_PYTHON:-python3}"
have_weasy=0
"$py" -c "import weasyprint" 2>/dev/null && have_weasy=1

for g in user-guide obs-guide; do
    python3 -I scripts/md2html.py "docs/$g.md" "docs/html/$g.html" --link-ext .html --footer "$footer"
    if [ $have_weasy = 1 ]; then
        python3 -I scripts/md2html.py "docs/$g.md" "$work/$g.html" --link-ext .pdf --footer "$footer"
        "$py" scripts/html2pdf.py "$work/$g.html" "docs/pdf/$g.pdf"
    fi
done
ls -la docs/html docs/pdf
[ $have_weasy = 1 ] || echo "WeasyPrint not found: only HTML was made (set BPR_WEASYPRINT_PYTHON)." >&2
