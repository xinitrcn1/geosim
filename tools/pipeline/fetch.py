#!/usr/bin/env python3
"""Download raw datasets for the 2020 and 2008 scenarios (initial state + back-test targets) into data/raw/ (not committed).

Standard library only. Re-running skips files that already exist unless
--force is given. Every source is listed in SOURCES with its licence, which
build_assets.py copies into assets/SOURCES.txt.

Usage: python3 tools/pipeline/fetch.py [--raw data/raw] [--force] [--map-only]
"""

import argparse
import json
import pathlib
import sys
import time
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from countries import SLICE, ISO2  # noqa: E402

USER_AGENT = "geosim-data-pipeline/0.1"

# World Bank WDI indicators (2019 initial state).
WDI_INDICATORS = {
    "SP.POP.TOTL": "population",
    "SP.POP.0014.TO.ZS": "pop_0_14_pct",
    "SP.POP.65UP.TO.ZS": "pop_65up_pct",
    "SL.TLF.TOTL.IN": "labor_force",
    "SL.UEM.TOTL.ZS": "unemployment_pct",
    "NY.GDP.MKTP.CD": "gdp_usd",
    "NY.GDP.MKTP.PP.CD": "gdp_ppp",
    "NV.AGR.TOTL.ZS": "va_agr_pct",
    "NV.IND.TOTL.ZS": "va_ind_pct",
    "NV.IND.MANF.ZS": "va_man_pct",
    "NV.SRV.TOTL.ZS": "va_srv_pct",
    "NY.GDP.PETR.RT.ZS": "oil_rents_pct",
    "NY.GDP.NGAS.RT.ZS": "gas_rents_pct",
    "NY.GDP.COAL.RT.ZS": "coal_rents_pct",
    "SL.AGR.EMPL.ZS": "emp_agr_pct",
    "SL.IND.EMPL.ZS": "emp_ind_pct",
    "SL.SRV.EMPL.ZS": "emp_srv_pct",
    "SL.EMP.VULN.ZS": "vulnerable_emp_pct",
    "NE.CON.PRVT.ZS": "hh_cons_pct",
    "NE.CON.GOVT.ZS": "gov_cons_pct",
    "NE.GDI.TOTL.ZS": "investment_pct",
    "NE.EXP.GNFS.ZS": "exports_pct",
    "NE.IMP.GNFS.ZS": "imports_pct",
    "SI.POV.GINI": "gini",
    "SH.MED.BEDS.ZS": "hospital_beds_per_1000",
    "SE.TER.CUAT.BA.ZS": "tertiary_attainment_pct",
    "SP.URB.TOTL.IN.ZS": "urban_pct",
    "NY.GDP.MKTP.KD.ZG": "real_gdp_growth_pct",
    "FP.CPI.TOTL.ZG": "cpi_inflation_pct",
    "GC.REV.XGRT.GD.ZS": "gov_revenue_pct",
    "GC.XPN.TOTL.GD.ZS": "gov_expense_pct",
    "GC.NLD.TOTL.GD.ZS": "gov_net_lending_pct",
    "GC.DOD.TOTL.GD.ZS": "gov_debt_pct",
    "SL.TLF.CACT.ZS": "lfp_rate_pct",
    "SP.DYN.CBRT.IN": "birth_rate_per_1000",
    "SP.DYN.CDRT.IN": "death_rate_per_1000",
    "BX.TRF.PWKR.DT.GD.ZS": "remittances_in_pct",
    "BM.TRF.PWKR.CD.DT": "remittances_out_usd",
    "AG.LND.TOTL.K2": "land_area_km2",
    "MS.MIL.XPND.GD.ZS": "defense_pct",  # SIPRI military expenditure (% of GDP)
    "ST.INT.RCPT.XP.ZS": "tourism_receipts_pct_exports",
    "ST.INT.ARVL": "tourist_arrivals",
    "ST.INT.DPRT": "tourist_departures",
}

SOURCES = [
    ("World Bank WDI", "https://data.worldbank.org", "CC BY 4.0"),
    ("World Bank Worldwide Governance Indicators (VA.EST, GE.EST)", "https://info.worldbank.org/governance/wgi", "CC BY 4.0"),
    ("Oxford COVID-19 Government Response Tracker (OxCGRT)", "https://github.com/OxCGRT/covid-policy-dataset", "CC BY 4.0"),
    ("BIS central bank policy rates (WS_CBPOL)", "https://data.bis.org", "BIS terms: free reuse with attribution"),
    ("geoBoundaries gbOpen ADM1 (simplified)", "https://www.geoboundaries.org", "CC BY 4.0 / ODbL / public domain per country (see metadata)"),
    ("Natural Earth 10m populated places", "https://www.naturalearthdata.com", "Public domain"),
    ("Brent crude daily spot price (EIA series RBRTE, via datahub.io datasets/oil-prices)", "https://github.com/datasets/oil-prices", "Public domain (EIA) / PDDL"),
    ("The Economist excess-deaths model (estimated excess deaths by country)", "https://github.com/TheEconomist/covid-19-the-economist-global-excess-deaths-model", "CC BY 4.0"),
    ("Our World in Data energy dataset (Energy Institute Statistical Review of World Energy; EIA)", "https://github.com/owid/energy-data", "CC BY 4.0"),
]

# World map (tools/pipeline/build_map.py -> assets/map/world.gsmap).
MAP_SOURCES = [
    ("NOAA ETOPO1 Global Relief Model, ice surface (Amante & Eakins 2009), 5 arc-minute subsample via the NOAA CoastWatch ERDDAP (etopo180)",
     "https://coastwatch.pfeg.noaa.gov/erddap/griddap/etopo180.html", "Public domain (US Government work)"),
    ("Koppen-Geiger climate classification 1991-2020, 0.1 degree (Beck et al. 2023, Scientific Data 10:724)",
     "https://doi.org/10.6084/m9.figshare.21789074", "CC BY 4.0"),
    ("Natural Earth 10m admin-0 countries (territories without geoBoundaries ADM1) and 50m lakes",
     "https://www.naturalearthdata.com", "Public domain"),
    ("WorldClim 2.1 monthly precipitation 1970-2000, 10 arc-minutes (Fick & Hijmans 2017); per-region aggregates only "
     "(regions_geo.csv, build_region_geo.py)", "https://www.worldclim.org/data/worldclim21.html",
     "WorldClim terms: free for academic and non-commercial use; redistribution of the raw data needs permission "
     "(only derived regional statistics ship)"),
]
SOURCES += MAP_SOURCES

ETOPO_URL = ("https://coastwatch.pfeg.noaa.gov/erddap/griddap/etopo180.esriAscii?"
             "altitude%5B(-90):5:(90)%5D%5B(-180):5:(180)%5D")
KOPPEN_URL = "https://ndownloader.figshare.com/files/61012822"  # koppen_geiger_tif.zip (130 MB)
WORLDCLIM_PREC_URL = "https://geodata.ucdavis.edu/climate/worldclim/2_1/base/wc2.1_10m_prec.zip"  # 12 GeoTIFFs, ~40 MB
NE_URL = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"


def fetch_map(raw: pathlib.Path, force: bool) -> None:
    """Raw inputs of the world map (build_map.py) and regional geography (build_region_geo.py), ~230 MB in data/raw/map/."""
    print("World map: ETOPO1 relief, Koppen-Geiger climate, Natural Earth territories and lakes")
    d = raw / "map"
    d.mkdir(parents=True, exist_ok=True)
    get(ETOPO_URL, d / "etopo1_5min.asc", force)
    get(KOPPEN_URL, d / "koppen_geiger_tif.zip", force)
    get(NE_URL + "ne_10m_admin_0_countries.geojson", d / "ne_10m_admin_0_countries.geojson", force)
    get(NE_URL + "ne_50m_lakes.geojson", d / "ne_50m_lakes.geojson", force)
    get(WORLDCLIM_PREC_URL, d / "wc2.1_10m_prec.zip", force)


def pick_adm1(meta):
    """iso3 -> metadata entry. A few countries are listed twice (e.g. India with 1 and
    36 units); keep the entry with the most units."""
    best = {}
    for m in meta:
        iso = m["boundaryISO"]
        if iso not in best or int(m.get("admUnitCount") or 0) > int(best[iso].get("admUnitCount") or 0):
            best[iso] = m
    return best


def get(url: str, dest: pathlib.Path, force: bool) -> None:
    if dest.exists() and not force:
        print(f"  cached  {dest.name}")
        return
    print(f"  fetch   {url}")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=180) as r:
                data = r.read()
            break
        except Exception as e:  # network hiccups: retry with backoff
            if attempt == 3:
                raise
            print(f"    retry after error: {e}")
            time.sleep(2 ** attempt)
    tmp = dest.with_suffix(dest.suffix + ".part")
    tmp.write_bytes(data)
    tmp.replace(dest)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", default="data/raw")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--map-only", action="store_true", help="only the world-map inputs (build_map.py)")
    args = ap.parse_args()
    raw = pathlib.Path(args.raw)
    if args.map_only:
        fetch_map(raw, args.force)
        return 0
    (raw / "wdi").mkdir(parents=True, exist_ok=True)

    countries = ";".join(SLICE + ["WLD"])
    print("World Bank WDI")
    for code in WDI_INDICATORS:
        get(f"https://api.worldbank.org/v2/country/{countries}/indicator/{code}"
            f"?date=2010:2020&format=json&per_page=2000",
            raw / "wdi" / f"{code}.json", args.force)

    # All-country population: weights for the rest-of-world NPI average.
    get("https://api.worldbank.org/v2/country/all/indicator/SP.POP.TOTL?date=2019&format=json&per_page=400",
        raw / "wdi_population_all.json", args.force)

    print("OxCGRT (national, compact)")
    get("https://raw.githubusercontent.com/OxCGRT/covid-policy-dataset/main/data/OxCGRT_compact_national_v1.csv",
        raw / "oxcgrt_compact_national.csv", args.force)

    print("BIS policy rates")
    areas = "+".join(sorted(set(ISO2.values()) | {"XM"}))
    get(f"https://stats.bis.org/api/v2/data/dataflow/BIS/WS_CBPOL/1.0/D.{areas}"
        f"?startPeriod=2019-12-01&endPeriod=2020-12-31&format=csv",
        raw / "bis_cbpol.csv", args.force)

    print("World Bank WDI (all economies) + country metadata")
    (raw / "wdi_all").mkdir(parents=True, exist_ok=True)
    for code in WDI_INDICATORS:
        get(f"https://api.worldbank.org/v2/country/all/indicator/{code}?date=2010:2020&format=json&per_page=20000",
            raw / "wdi_all" / f"{code}.json", args.force)
    get("https://api.worldbank.org/v2/country?format=json&per_page=400", raw / "wb_countries.json", args.force)
    # Worldwide Governance Indicators (source 3): regime openness and state capacity priors for AI agents.
    for code in ("GOV_WGI_VA.EST", "GOV_WGI_GE.EST"):
        get(f"https://api.worldbank.org/v2/country/all/indicator/{code}?date=2015:2019&format=json&per_page=20000",
            raw / "wdi_all" / f"WGI_{code}.json", args.force)

    print("BIS policy rates (all areas)")
    get("https://stats.bis.org/api/v2/data/dataflow/BIS/WS_CBPOL/1.0/D.?startPeriod=2019-12-01&endPeriod=2020-12-31&format=csv",
        raw / "bis_cbpol_all.csv", args.force)

    print("geoBoundaries ADM1")
    gb_dir = raw / "geoboundaries"
    gb_dir.mkdir(parents=True, exist_ok=True)
    get("https://www.geoboundaries.org/api/current/gbOpen/ALL/ADM1/", raw / "geoboundaries_adm1.json", args.force)
    meta = json.loads((raw / "geoboundaries_adm1.json").read_text())
    for m in pick_adm1(meta).values():
        get(m["simplifiedGeometryGeoJSON"], gb_dir / f"{m['boundaryISO']}.geojson", args.force)

    print("Natural Earth populated places")
    get("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_10m_populated_places_simple.geojson",
        raw / "ne_populated_places.geojson", args.force)

    print("Brent (EIA RBRTE)")
    get("https://raw.githubusercontent.com/datasets/oil-prices/main/data/brent-daily.csv",
        raw / "brent.csv", args.force)

    print("Energy (OWID: oil, gas and coal production and consumption by country)")
    get("https://raw.githubusercontent.com/owid/energy-data/master/owid-energy-data.csv", raw / "owid_energy.csv", args.force)

    print("Excess deaths (The Economist model): COVID death reporting ratios")
    get("https://raw.githubusercontent.com/TheEconomist/covid-19-the-economist-global-excess-deaths-model/main/output-data/export_country.csv",
        raw / "economist_excess.csv", args.force)

    # The 2008 calibration slice (build_slice2008.py): 2007 state, 2008-09 outcomes.
    print("2008 slice: World Bank WDI 2000-2010, WGI 2003-2007, BIS policy rates 2007-12..2009-12")
    (raw / "wdi2008").mkdir(parents=True, exist_ok=True)
    for code in WDI_INDICATORS:
        get(f"https://api.worldbank.org/v2/country/{countries}/indicator/{code}"
            f"?date=2000:2010&format=json&per_page=2000",
            raw / "wdi2008" / f"{code}.json", args.force)
    for code in ("GOV_WGI_VA.EST", "GOV_WGI_GE.EST"):
        get(f"https://api.worldbank.org/v2/country/all/indicator/{code}?date=2003:2007&format=json&per_page=20000",
            raw / "wdi2008" / f"WGI_{code}.json", args.force)
    get(f"https://stats.bis.org/api/v2/data/dataflow/BIS/WS_CBPOL/1.0/D.{areas}"
        f"?startPeriod=2007-12-01&endPeriod=2009-12-31&format=csv",
        raw / "bis_cbpol_2008.csv", args.force)

    fetch_map(raw, args.force)

    (raw / "sources.json").write_text(json.dumps(SOURCES, indent=1))
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
