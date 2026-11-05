"""Society, finance, extension and logistics tables of the full world (build_world.py).

Every value comes from a published dataset (fetch.py fetch_world_extras -> data/raw/world_extra):

  society.csv
    youth_share          population 15-29 / population 15-64 (UN WPP via WDI: SP.POP.1519/2024/2529
                         .MA/.FE.5Y, SP.POP.TOTL.MA.ZS, SP.POP.1564.TO.ZS)
    youth_unemp_rel      youth (15-24) / total unemployment rate (ILO modelled estimates via WDI)
    internet_pct         internet users, % of population (ITU via WDI IT.NET.USER.ZS); the loader
                         derives social-media users from it (DataReportal ratio)
    corruption_cpi       Transparency International CPI 2019; nations TI does not score: fitted on
                         WGI control of corruption (linear, on the scored nations)
    emigrant_stock_pct   emigrants abroad, % of the home population, mid-2019 (UN DESA International
                         Migrant Stock 2020, interpolated 2015-2020)
  finance.csv
    private_credit_pct   domestic credit to the private sector, % GDP (WDI FS.AST.PRVT.GD.ZS; else
                         World Bank Fiscal Space Database pscy)
    npl_pct              non-performing loans, % of gross loans (WDI FB.AST.NPER.ZS)
    capital_ratio_pct    regulatory capital / risk-weighted assets (World Bank GFDD.SI.05)
    lending_spread_pp    lending minus deposit rate (WDI FR.INR.LNDP)
    avg_maturity_years   average maturity of government debt (Fiscal Space avglife)
    foreign_debt_share   share of government debt held abroad (Fiscal Space fordebtsh)
    fx_debt_share        foreign-currency share of government debt (Fiscal Space fxsovsh)
    tariff_pct           applied tariff, trade-weighted mean, all products (WITS via WDI)
    union_density        trade-union members / employees (ILOSTAT ILR_TUMT_NOC_RT)
    bargaining_coverage  employees covered by collective agreements (ILOSTAT ILR_CBCT_NOC_RT)
  nations_ext.csv
    rd_pct               gross R&D expenditure, % GDP (UNESCO UIS via WDI GB.XPD.RSDV.GD.ZS)
    reserves_months      total reserves in months of imports (WDI FI.RES.TOTL.MO)
    terrain              mountainous share of land, from Nunn & Puga (2012) highly rugged share:
                         0.013 + 0.0134 x rugged_pc, fitted on the 2020 slice's Fearon & Laitin
                         values (R^2 0.83, n 20)
    language             official language with the most speakers (Unicode CLDR territoryInfo),
                         as an ISO 639 three-letter code (CLDR aliases)
  logistics.csv
    port_teu_m           container port traffic, million TEU (WDI IS.SHP.GOOD.TU)

Fills of nations.csv (before income-group imputation):
    gov_debt_pct, gov_net_lending_pct   general government gross debt and fiscal balance, % GDP
                         (Fiscal Space ggdy, fby: IMF WEO-based; preferred to WDI's central-government
                         series, which cover a third of the world)
    hospital_beds_per_1000              WHO GHO WHS6_102 where WDI has none
Values are the latest available in 2015-2019 (2010-2019 for slow-moving structural series). A
nation without data in a column leaves it empty; the loader then keeps its documented default.
"""

import csv
import io
import json
import pathlib
import statistics
import zipfile

import xlsx

YEAR = 2019


def _latest(series, back=4):
    for y in range(YEAR, YEAR - back - 1, -1):
        if y in series:
            return series[y]
    return None


def load_wdi_extra(raw: pathlib.Path):
    """-> {code: {iso3: {year: value}}} for the WORLD_WDI indicators."""
    from fetch import WORLD_WDI
    out = {}
    for code in WORLD_WDI:
        series = {}
        payload = json.loads((raw / "world_extra" / "wdi" / f"{code}.json").read_text())
        for row in payload[1] or []:
            if row["value"] is not None:
                series.setdefault(row["countryiso3code"], {})[int(row["date"])] = float(row["value"])
        out[code] = series
    return out


def load_fiscal_space(raw: pathlib.Path):
    """-> {sheet: {iso3: {year: value}}} for the Fiscal Space sheets used here."""
    wb = xlsx.Workbook(raw / "world_extra" / "fiscal_space.xlsx")
    out = {}
    for sheet in ("ggdy", "fby", "avglife", "fordebtsh", "fxsovsh", "pscy"):
        rows = wb.rows(sheet)
        head = rows[0]
        years = {i: int(h) for i, h in enumerate(head) if isinstance(h, str) and h.isdigit()}
        data = {}
        for r in rows[1:]:
            if not r or not isinstance(r[0], str) or len(r[0]) != 3:
                continue
            s = {years[i]: float(v) for i, v in enumerate(r) if i in years and isinstance(v, float)}
            if s:
                data[r[0]] = s
        out[sheet] = data
    return out


def load_ilo(path: pathlib.Path):
    vals = {}
    with path.open(encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            try:
                y, v = int(r["time"][:4]), float(r["obs_value"])
            except (ValueError, KeyError):
                continue
            vals.setdefault(r["ref_area"], {})[y] = v
    return vals


def load_who_beds(path: pathlib.Path):
    vals = {}
    for r in json.loads(path.read_text())["value"]:
        if r.get("SpatialDimType") == "COUNTRY" and r.get("NumericValue") is not None and r.get("TimeDim"):
            vals.setdefault(r["SpatialDim"], {})[int(r["TimeDim"])] = float(r["NumericValue"]) / 10.0  # per 10,000 -> per 1,000
    return vals


def load_gfdd(path: pathlib.Path):
    vals = {}
    for r in json.loads(path.read_text())["source"]["data"]:
        if r["value"] is None:
            continue
        v = {x["concept"]: x for x in r["variable"]}
        vals.setdefault(v["Country"]["id"], {})[int(v["Time"]["value"])] = float(r["value"])
    return vals


def load_wgi_cc(path: pathlib.Path):
    vals = {}
    for r in sorted(json.loads(path.read_text())[1] or [], key=lambda r: r["date"]):
        if r["value"] is not None:
            vals[r["countryiso3code"]] = float(r["value"])
    return vals


def load_cpi(path: pathlib.Path):
    z = zipfile.ZipFile(path)
    name = next(n for n in z.namelist() if n.endswith("CPI2019.xlsx") and not n.startswith("__MACOSX"))
    wb = xlsx.Workbook(io.BytesIO(z.read(name)))
    rows = wb.rows(wb.names()[0])
    head_row = next(i for i, r in enumerate(rows) if r and "ISO3" in r)
    head = rows[head_row]
    ci, si = head.index("ISO3"), next(i for i, h in enumerate(head) if isinstance(h, str) and h.startswith("CPI score 2019"))
    return {r[ci]: float(r[si]) for r in rows[head_row + 1:] if len(r) > si and isinstance(r[ci], str) and isinstance(r[si], float)}


def load_rugged(path: pathlib.Path):
    z = zipfile.ZipFile(path)
    rows = list(csv.DictReader(io.TextIOWrapper(z.open("rugged_data.csv"), encoding="latin-1")))
    by_iso = {r["isocode"]: r for r in rows}
    m49 = {int(r["isonum"]): r["isocode"] for r in rows if r["isonum"].isdigit()}
    return by_iso, m49


def load_emigrants(path: pathlib.Path, m49_to_iso):
    """Emigrants from each origin living anywhere, mid-2019 (2015 + 0.8 x (2020 - 2015))."""
    wb = xlsx.Workbook(path)
    rows = wb.rows("Table 1")
    head_i = next(i for i, r in enumerate(rows) if r and "Location code of origin" in r)
    head = rows[head_i]
    dest_c, orig_c = head.index("Location code of destination"), head.index("Location code of origin")
    y15 = head.index(2015.0)
    y20 = head.index(2020.0)  # the first 2015 / 2020 columns: both sexes
    out = {}
    for r in rows[head_i + 1:]:
        if len(r) <= y20 or r[dest_c] != 900.0 or not isinstance(r[orig_c], float):  # destination WORLD
            continue
        iso = m49_to_iso.get(int(r[orig_c]))
        a, b = r[y15], r[y20]
        if iso and isinstance(a, float) and isinstance(b, float):
            out[iso] = a + 0.8 * (b - a)
    return out


def load_languages(raw: pathlib.Path):
    """iso2 (upper) -> ISO 639 three-letter code of the official language with most speakers."""
    info = json.loads((raw / "world_extra" / "cldr_territory_info.json").read_text())["supplemental"]["territoryInfo"]
    alias = json.loads((raw / "world_extra" / "cldr_aliases.json").read_text())["supplemental"]["metadata"]["alias"]["languageAlias"]
    three = {}
    for k, v in alias.items():
        if len(k) == 3 and v.get("_reason") == "overlong" and "_replacement" in v:
            three.setdefault(v["_replacement"], k)
    out = {}
    for iso2, t in info.items():
        langs = t.get("languagePopulation") or {}
        official = {k: v for k, v in langs.items() if v.get("_officialStatus") in ("official", "de_facto_official")}
        pool = official or langs
        if not pool:
            continue
        best = max(pool, key=lambda k: float(pool[k].get("_populationPercent", 0)))
        base = best.split("_")[0]
        out[iso2] = three.get(base, base if len(base) == 3 else base)
    return out


def _fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return my - b * mx, b


def build(raw: pathlib.Path, nations: dict, countries: dict):
    """-> (tables, fills, report): tables = {file: (header, rows)}; fills = {field: {iso: value}}."""
    w = load_wdi_extra(raw)
    fs = load_fiscal_space(raw)
    gfdd = load_gfdd(raw / "world_extra" / "gfdd_si05.json")
    ilo_ud = load_ilo(raw / "world_extra" / "ilo_union_density.csv")
    ilo_cb = load_ilo(raw / "world_extra" / "ilo_bargaining_coverage.csv")
    beds = load_who_beds(raw / "world_extra" / "who_beds.json")
    cpi = load_cpi(raw / "world_extra" / "ti_cpi2019.zip")
    cc = load_wgi_cc(raw / "world_extra" / "wgi_cc.json")
    rugged, m49 = load_rugged(raw / "world_extra" / "rugged_data.zip")
    emig = load_emigrants(raw / "world_extra" / "undesa_ims2020.xlsx", m49)
    langs = load_languages(raw)
    order = sorted(nations)
    g = lambda code, iso, back=4: _latest(w[code].get(iso, {}), back)
    report = {}

    # CPI where TI does not score: a linear fit on WGI control of corruption.
    pairs = [(cc[i], cpi[i]) for i in cpi if i in cc]
    a, b = _fit([p[0] for p in pairs], [p[1] for p in pairs])
    report["cpi_fit"] = f"CPI = {a:.1f} + {b:.1f} x WGI CC.EST (n={len(pairs)})"

    soc = []
    for iso in order:
        male = g("SP.POP.TOTL.MA.ZS", iso)
        ages_m = [g(c, iso) for c in ("SP.POP.1519.MA.5Y", "SP.POP.2024.MA.5Y", "SP.POP.2529.MA.5Y")]
        ages_f = [g(c, iso) for c in ("SP.POP.1519.FE.5Y", "SP.POP.2024.FE.5Y", "SP.POP.2529.FE.5Y")]
        wa = g("SP.POP.1564.TO.ZS", iso)
        youth = None
        if male is not None and wa and None not in ages_m and None not in ages_f:
            youth = (male / 100 * sum(ages_m) + (1 - male / 100) * sum(ages_f)) / wa
        yu, u = g("SL.UEM.1524.ZS", iso), g("SL.UEM.TOTL.ZS", iso)
        rel = yu / u if yu is not None and u and u > 0.3 else None
        corruption = cpi.get(iso)
        if corruption is None and iso in cc:
            corruption = max(5.0, min(95.0, a + b * cc[iso]))
        pop = nations[iso]["population"]
        em = 100.0 * emig[iso] / pop if iso in emig and pop else None
        soc.append([iso, youth, rel, g("IT.NET.USER.ZS", iso), corruption, em])

    fin = []
    for iso in order:
        fsv = lambda sheet: _latest(fs[sheet].get(iso, {}))
        credit = g("FS.AST.PRVT.GD.ZS", iso, 9)
        if credit is None:
            credit = fsv("pscy")
        share = lambda v: v / 100.0 if v is not None else None
        fin.append([iso, credit, g("FB.AST.NPER.ZS", iso), _latest(gfdd.get(iso, {})), g("FR.INR.LNDP", iso, 9), fsv("avglife"),
                    share(fsv("fordebtsh")), share(fsv("fxsovsh")), g("TM.TAX.MRCH.WM.AR.ZS", iso),
                    share(_latest(ilo_ud.get(iso, {}), 9)), share(_latest(ilo_cb.get(iso, {}), 9))])

    ext = []
    for iso in order:
        terrain = None
        if iso in rugged and rugged[iso]["rugged_pc"]:
            terrain = max(0.0, min(0.95, 0.013 + 0.0134 * float(rugged[iso]["rugged_pc"])))
        iso2 = (countries.get(iso, {}).get("iso2") or "").upper()
        ext.append([iso, g("GB.XPD.RSDV.GD.ZS", iso, 9), g("FI.RES.TOTL.MO", iso), terrain, langs.get(iso2, "")])

    logi = []
    for iso in order:
        teu = g("IS.SHP.GOOD.TU", iso, 9)
        if teu is not None:
            logi.append([iso, teu / 1e6])

    fills = {"gov_debt_pct": {}, "gov_net_lending_pct": {}, "hospital_beds_per_1000": {}}
    for iso in order:
        d, bal = _latest(fs["ggdy"].get(iso, {})), _latest(fs["fby"].get(iso, {}))
        if d is not None:
            fills["gov_debt_pct"][iso] = d
        if bal is not None:
            fills["gov_net_lending_pct"][iso] = bal
        bb = _latest(beds.get(iso, {}), 9)
        if bb is not None:
            fills["hospital_beds_per_1000"][iso] = bb

    tables = {
        "society.csv": (["iso3", "youth_share", "youth_unemp_rel", "internet_pct", "corruption_cpi", "emigrant_stock_pct"], soc),
        "finance.csv": (["iso3", "private_credit_pct", "npl_pct", "capital_ratio_pct", "lending_spread_pp", "avg_maturity_years",
                         "foreign_debt_share", "fx_debt_share", "tariff_pct", "union_density", "bargaining_coverage"], fin),
        "nations_ext.csv": (["iso3", "rd_pct", "reserves_months", "terrain", "language"], ext),
        "logistics.csv": (["iso3", "port_teu_m"], logi),
    }
    for name, (head, rows) in tables.items():
        cover = {h: sum(1 for r in rows if r[i] not in (None, "")) for i, h in enumerate(head) if i > 0}
        report[name] = cover
    report["fills"] = {k: len(v) for k, v in fills.items()}
    return tables, fills, report
