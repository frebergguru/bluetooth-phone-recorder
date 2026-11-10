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

# English guides live in docs/, translations in docs/<lang>/ (same names)
for lang in "" nb; do
    src="docs${lang:+/$lang}"
    [ -f "$src/user-guide.md" ] || continue
    mkdir -p "docs/html${lang:+/$lang}" "docs/pdf${lang:+/$lang}" "$work${lang:+/$lang}"
    for g in user-guide obs-guide; do
        python3 -I scripts/md2html.py "$src/$g.md" "docs/html${lang:+/$lang}/$g.html" --link-ext .html --footer "$footer"
        if [ $have_weasy = 1 ]; then
            # the print copy sits next to the source so relative image paths resolve
            python3 -I scripts/md2html.py "$src/$g.md" "$work${lang:+/$lang}/$g.html" --link-ext .pdf --footer "$footer"
            "$py" scripts/html2pdf.py "$work${lang:+/$lang}/$g.html" "docs/pdf${lang:+/$lang}/$g.pdf"
        fi
    done
done
ls -laR docs/html docs/pdf | grep -E "\.(pdf|html)$"
[ $have_weasy = 1 ] || echo "WeasyPrint not found: only HTML was made (set BPR_WEASYPRINT_PYTHON)." >&2
