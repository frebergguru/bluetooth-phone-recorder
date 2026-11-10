#!/usr/bin/env python3
"""Convert the project's Markdown guides to one self-contained HTML file.

Handles the Markdown used in docs/: headings (with GitHub-style anchors),
paragraphs, bullet and numbered lists (one level of nesting), tables, fenced
code, block images, links, bold, italic, inline code and rules. Images are
embedded as data URIs and the CSS is inline, so the file works on its own.

usage: md2html.py <input.md> <output.html> [--link-ext .html|.pdf] [--title T]
"""
import argparse, base64, html, mimetypes, os, re, sys

CSS = """
:root { --fg:#1d1d1f; --muted:#4a4a4f; --bg:#ffffff; --line:#d0d0d7; --code:#f3f3f6;
        --accent:#1a5fb4; --head:#eef3fb; }
@media screen and (prefers-color-scheme: dark) {
  :root { --fg:#ececf0; --muted:#b8b8c0; --bg:#1e1e22; --line:#3d3d45; --code:#2a2a30;
          --accent:#78aeed; --head:#26303d; }
  img.shot { border-color: var(--line); }
}
html { background: var(--bg); }
body { color: var(--fg); background: var(--bg); font: 16px/1.6 Cantarell, "Noto Sans",
       "DejaVu Sans", sans-serif; max-width: 52rem; margin: 0 auto; padding: 2rem 1.2rem 4rem; }
h1 { font-size: 2rem; line-height: 1.25; margin: 0 0 1rem; }
h2 { font-size: 1.45rem; margin: 2.4rem 0 .8rem; padding-bottom: .3rem;
     border-bottom: 1px solid var(--line); }
h3 { font-size: 1.15rem; margin: 1.8rem 0 .6rem; }
p, ul, ol, table, pre, figure { margin: 0 0 1rem; }
ul, ol { padding-left: 1.6rem; }
li { margin: .25rem 0; }
a { color: var(--accent); }
code { font: .92em/1.4 "DejaVu Sans Mono", "Noto Sans Mono", monospace; background: var(--code);
       padding: .1em .3em; border-radius: 4px; }
pre { background: var(--code); padding: .8rem 1rem; border-radius: 8px; overflow-x: auto; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; width: 100%; font-size: .95rem; }
th, td { border: 1px solid var(--line); padding: .45rem .6rem; text-align: left; vertical-align: top; }
th { background: var(--head); }
hr { border: 0; border-top: 1px solid var(--line); margin: 2rem 0; }
figure { text-align: center; }
img { max-width: 100%; height: auto; }
img.shot { border: 1px solid #c8c8cf; border-radius: 8px; }
.doc-footer { margin-top: 3rem; font-size: .85rem; color: var(--muted); }
@media print {
  body { max-width: none; padding: 0; font-size: 11pt; }
  h2 { break-after: avoid; } h3 { break-after: avoid; }
  figure, table, pre { break-inside: avoid; }
  a { color: inherit; text-decoration: underline; }
}
@page { size: A4; margin: 16mm 15mm 18mm;
  @bottom-center { content: counter(page); font-size: 9pt; color: #6b6b72; } }
@page :first { @bottom-center { content: none; } }
@media print { figure img { max-height: 19cm; width: auto; } }
"""


def slug(text):
    """GitHub-style heading anchor."""
    t = re.sub(r"<[^>]+>", "", text).lower()
    t = re.sub(r"[^\w\- ]", "", t, flags=re.UNICODE)
    return t.replace(" ", "-")


def inline(text, link_ext, base_dir):
    """Inline Markdown to HTML (text is raw Markdown)."""
    out, i = [], 0
    # protect inline code first
    parts = re.split(r"(`[^`]+`)", text)
    for part in parts:
        if part.startswith("`") and part.endswith("`") and len(part) > 1:
            out.append("<code>%s</code>" % html.escape(part[1:-1]))
            continue
        s = html.escape(part, quote=False)
        # links [text](url)
        def link(m):
            label, url = m.group(1), m.group(2)
            url = html.unescape(url)
            if not re.match(r"^[a-z]+:", url) and not url.startswith("#"):
                path, _, frag = url.partition("#")
                if path.endswith(".md"):
                    name = os.path.basename(path)
                    if name in ("user-guide.md", "obs-guide.md"):
                        if link_ext == ".pdf":
                            # a PDF travels alone: point at the published copy (a
                            # relative link would become a local file path)
                            path = ("https://github.com/frebergguru/bluetooth-phone-recorder"
                                    "/blob/master/docs/pdf/" + LANG_SUB + name[:-3] + ".pdf")
                        else:
                            path = name[:-3] + link_ext
                    else:
                        # files outside the exported guides: link to GitHub
                        rel = os.path.normpath(os.path.join(base_dir, path))
                        path = "https://github.com/frebergguru/bluetooth-phone-recorder/blob/master/" + rel
                url = path + ("#" + frag if frag else "")
            return '<a href="%s">%s</a>' % (html.escape(url), label)
        s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, s)
        s = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", s)
        s = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])", r"<em>\1</em>", s)
        out.append(s)
    return "".join(out)


def data_uri(path):
    mime = mimetypes.guess_type(path)[0] or "application/octet-stream"
    with open(path, "rb") as f:
        return "data:%s;base64,%s" % (mime, base64.b64encode(f.read()).decode())


EMBED = True
LANG_SUB = ""  # "nb/" for the Norwegian guides


def convert(md, src_dir, link_ext):
    lines = md.split("\n")
    out, i, title = [], 0, None
    rel_dir = os.path.relpath(src_dir, os.path.join(os.path.dirname(__file__), ".."))

    def para_flush(buf):
        if buf:
            out.append("<p>%s</p>" % inline(" ".join(x.strip() for x in buf), link_ext, rel_dir))
        buf.clear()

    para = []
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        # fenced code
        if stripped.startswith("```"):
            para_flush(para)
            i += 1
            code = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            i += 1
            out.append("<pre><code>%s</code></pre>" % html.escape("\n".join(code)))
            continue
        # headings
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            para_flush(para)
            level, text = len(m.group(1)), m.group(2).strip()
            if level == 1 and title is None:
                title = re.sub(r"[*`]", "", text)
            out.append('<h%d id="%s">%s</h%d>' % (level, slug(text), inline(text, link_ext, rel_dir), level))
            i += 1
            continue
        # rule
        if re.match(r"^-{3,}$", stripped):
            para_flush(para)
            out.append("<hr>")
            i += 1
            continue
        # block image
        m = re.match(r"^!\[([^\]]*)\]\(([^)]+)\)$", stripped)
        if m:
            para_flush(para)
            alt, src = m.group(1), m.group(2)
            path = os.path.join(src_dir, src)
            cls = "" if src.endswith(".svg") else ' class="shot"'
            img = data_uri(path) if EMBED else "file://" + os.path.abspath(path)
            out.append('<figure><img%s src="%s" alt="%s"></figure>' % (cls, img, html.escape(alt)))
            i += 1
            continue
        # table
        if stripped.startswith("|") and i + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|$", lines[i + 1].strip()):
            para_flush(para)
            def cells(row):
                return [c.strip() for c in row.strip().strip("|").split("|")]
            head = cells(line)
            i += 2
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(cells(lines[i]))
                i += 1
            t = ["<table><thead><tr>"] + ["<th>%s</th>" % inline(c, link_ext, rel_dir) for c in head]
            t.append("</tr></thead><tbody>")
            for r in rows:
                t.append("<tr>" + "".join("<td>%s</td>" % inline(c, link_ext, rel_dir) for c in r) + "</tr>")
            t.append("</tbody></table>")
            out.append("".join(t))
            continue
        # lists (one nesting level, items may continue on indented lines)
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", line)
        if m:
            para_flush(para)
            ordered = m.group(2)[0].isdigit()
            tag = "ol" if ordered else "ul"
            items = []
            while i < len(lines):
                m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", lines[i])
                if m and len(m.group(1)) == 0:
                    items.append({"text": [m.group(3)], "sub": [], "subtag": None, "code": []})
                    i += 1
                elif m and items:
                    items[-1]["sub"].append(m.group(3))
                    items[-1]["subtag"] = "ol" if m.group(2)[0].isdigit() else "ul"
                    i += 1
                elif lines[i].strip().startswith("```") and items:
                    i += 1
                    code = []
                    while i < len(lines) and not lines[i].strip().startswith("```"):
                        code.append(lines[i].strip("\n")[2:] if lines[i].startswith("  ") else lines[i])
                        i += 1
                    i += 1
                    items[-1]["code"].append("\n".join(code))
                elif lines[i].startswith("  ") and lines[i].strip() and items:
                    if items[-1]["sub"]:
                        items[-1]["sub"][-1] += " " + lines[i].strip()
                    else:
                        items[-1]["text"].append(lines[i].strip())
                    i += 1
                elif not lines[i].strip() and i + 1 < len(lines) and (
                        re.match(r"^\s*([-*]|\d+\.)\s+", lines[i + 1]) or lines[i + 1].startswith("  ")):
                    i += 1  # blank line inside a list
                else:
                    break
            h = ["<%s>" % tag]
            for it in items:
                body = inline(" ".join(it["text"]), link_ext, rel_dir)
                for c in it["code"]:
                    body += "<pre><code>%s</code></pre>" % html.escape(c)
                if it["sub"]:
                    body += "<%s>%s</%s>" % (it["subtag"], "".join(
                        "<li>%s</li>" % inline(s, link_ext, rel_dir) for s in it["sub"]), it["subtag"])
                h.append("<li>%s</li>" % body)
            h.append("</%s>" % tag)
            out.append("".join(h))
            continue
        if not stripped:
            para_flush(para)
            i += 1
            continue
        para.append(line)
        i += 1
    para_flush(para)
    return title or "Document", "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--link-ext", default=".html")
    ap.add_argument("--footer", default="")
    ap.add_argument("--no-embed", action="store_true",
                    help="load images from their files (for printing to PDF only)")
    a = ap.parse_args()
    global EMBED, LANG_SUB
    EMBED = not a.no_embed
    # guides in docs/<lang>/ are that language; English ones live in docs/
    parent = os.path.basename(os.path.dirname(os.path.abspath(a.src)))
    lang = parent if parent != "docs" else "en"
    LANG_SUB = "" if lang == "en" else lang + "/"
    md = open(a.src, encoding="utf-8").read()
    title, body = convert(md, os.path.dirname(os.path.abspath(a.src)), a.link_ext)
    footer = '<p class="doc-footer">%s</p>' % html.escape(a.footer) if a.footer else ""
    doc = ('<!doctype html>\n<html lang="%s">\n<head>\n<meta charset="utf-8">\n' % lang +
           '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
           '<title>%s</title>\n<style>%s</style>\n</head>\n<body>\n<main>\n%s\n</main>\n%s\n</body>\n</html>\n'
           % (html.escape(title), CSS, body, footer))
    with open(a.dst, "w", encoding="utf-8") as f:
        f.write(doc)


if __name__ == "__main__":
    main()
