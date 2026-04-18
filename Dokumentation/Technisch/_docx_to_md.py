"""
Extrahiert Text + Tabellenstruktur aus einer .docx-Datei und gibt Markdown aus.
Einmalig verwendet, um bTn_Wecker_Funktionsreferenz.docx als .md-Startbasis zu erzeugen.
"""
import sys
import zipfile
import xml.etree.ElementTree as ET

NS = {"w": "http://schemas.openxmlformats.org/wordprocessingml/2006/main"}

def txt(elem):
    return "".join(t.text or "" for t in elem.iter(f"{{{NS['w']}}}t"))

def style(p):
    s = p.find(".//w:pStyle", NS)
    return s.get(f"{{{NS['w']}}}val") if s is not None else ""

def heading_level(p):
    s = style(p)
    if s.startswith("Heading"):
        try:
            return int(s.replace("Heading", ""))
        except ValueError:
            return 0
    return 0

def is_bold(p):
    r = p.find(".//w:r/w:rPr/w:b", NS)
    return r is not None

def para_to_md(p):
    text = txt(p).strip()
    if not text:
        return ""
    lvl = heading_level(p)
    if lvl > 0:
        return f"{'#' * (lvl + 1)} {text}"
    if is_bold(p) and len(text) < 100 and not text.endswith("."):
        return f"**{text}**"
    return text

def cell_md(tc):
    parts = [txt(p).strip() for p in tc.findall("w:p", NS)]
    parts = [p for p in parts if p]
    joined = "<br>".join(parts)
    joined = joined.replace("\r\n", "<br>").replace("\n", "<br>").replace("\r", "<br>")
    return joined.replace("|", "\\|")

def table_to_md(tbl):
    rows = tbl.findall("w:tr", NS)
    if not rows:
        return ""
    out = []
    for i, tr in enumerate(rows):
        cells = [cell_md(tc) for tc in tr.findall("w:tc", NS)]
        out.append("| " + " | ".join(cells) + " |")
        if i == 0:
            out.append("|" + "|".join(["---"] * len(cells)) + "|")
    return "\n".join(out)

def main(docx_path, md_path):
    with zipfile.ZipFile(docx_path) as z:
        xml = z.read("word/document.xml")
    root = ET.fromstring(xml)
    body = root.find("w:body", NS)

    md_lines = []
    prev_blank = True
    for child in body:
        tag = child.tag.split("}")[-1]
        if tag == "p":
            line = para_to_md(child)
            if line:
                md_lines.append(line)
                md_lines.append("")
                prev_blank = True
            elif not prev_blank:
                md_lines.append("")
                prev_blank = True
        elif tag == "tbl":
            t = table_to_md(child)
            if t:
                md_lines.append(t)
                md_lines.append("")
                prev_blank = True

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(md_lines).rstrip() + "\n")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
