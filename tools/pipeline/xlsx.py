"""Minimal .xlsx reader (standard library only): sheet names and cell values.

Reads the shared-string table and a worksheet's <c> cells; numbers come back as float,
text as str, empty cells are absent. Enough for the statistical workbooks the pipeline
reads (World Bank Fiscal Space database, UN DESA migrant stock, Transparency
International CPI); no formulas, styles or dates.
"""

import re
import zipfile
import xml.etree.ElementTree as ET

NS = {"m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
      "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships"}


def _col_index(ref):
    letters = re.match(r"[A-Z]+", ref).group(0)
    n = 0
    for ch in letters:
        n = n * 26 + (ord(ch) - 64)
    return n - 1


class Workbook:
    def __init__(self, path):
        self.z = zipfile.ZipFile(path)
        self.shared = []
        if "xl/sharedStrings.xml" in self.z.namelist():
            root = ET.fromstring(self.z.read("xl/sharedStrings.xml"))
            for si in root.findall("m:si", NS):
                self.shared.append("".join(t.text or "" for t in si.iter("{%s}t" % NS["m"])))
        wb = ET.fromstring(self.z.read("xl/workbook.xml"))
        rels = ET.fromstring(self.z.read("xl/_rels/workbook.xml.rels"))
        target = {r.get("Id"): r.get("Target") for r in rels}
        self.sheets = {}
        for s in wb.find("m:sheets", NS):
            t = target[s.get("{%s}id" % NS["r"])]
            self.sheets[s.get("name")] = "xl/" + t.lstrip("/").replace("xl/", "", 1) if not t.startswith("xl/") else t

    def names(self):
        return list(self.sheets)

    def rows(self, sheet):
        """Rows of `sheet` as lists (None where a cell is empty)."""
        root = ET.fromstring(self.z.read(self.sheets[sheet]))
        out = []
        for row in root.iter("{%s}row" % NS["m"]):
            cells = {}
            for c in row.findall("m:c", NS):
                v = c.find("m:v", NS)
                t = c.get("t")
                if t == "inlineStr":
                    is_ = c.find("m:is", NS)
                    val = "".join(x.text or "" for x in is_.iter("{%s}t" % NS["m"])) if is_ is not None else None
                elif v is None:
                    continue
                elif t == "s":
                    val = self.shared[int(v.text)]
                elif t in ("str", "e"):
                    val = v.text
                elif t == "b":
                    val = v.text == "1"
                else:
                    try:
                        val = float(v.text)
                    except (TypeError, ValueError):
                        val = v.text
                cells[_col_index(c.get("r"))] = val
            width = max(cells) + 1 if cells else 0
            out.append([cells.get(i) for i in range(width)])
        return out
