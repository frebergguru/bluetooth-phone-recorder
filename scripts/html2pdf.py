#!/usr/bin/env python3
"""Render an HTML file to PDF with WeasyPrint (no browser involved).
usage: html2pdf.py <input.html> <output.pdf>"""
import os, sys
from weasyprint import HTML

src, dst = sys.argv[1], sys.argv[2]
doc = HTML(filename=src, base_url=os.path.dirname(os.path.abspath(src))).render()
doc.write_pdf(dst)
print("%s: %d pages" % (dst, len(doc.pages)))
