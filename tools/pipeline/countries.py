"""Nations simulated in the 2020 slice. ROW = world minus these.

The list is the G20 member states (the EU is represented by its members
DEU/FRA/ITA, plus ESP as a permanent guest). Add a nation here and rerun the
pipeline to extend the slice.
"""

SLICE = [
    "USA", "CHN", "JPN", "DEU", "GBR", "FRA", "ITA", "ESP", "CAN", "KOR",
    "AUS", "BRA", "MEX", "IND", "RUS", "TUR", "IDN", "SAU", "ZAF", "ARG",
]

ROW = "ROW"

# ISO3 -> ISO2 (BIS uses ISO2; XM = euro area)
ISO2 = {
    "USA": "US", "CHN": "CN", "JPN": "JP", "DEU": "DE", "GBR": "GB", "FRA": "FR",
    "ITA": "IT", "ESP": "ES", "CAN": "CA", "KOR": "KR", "AUS": "AU", "BRA": "BR",
    "MEX": "MX", "IND": "IN", "RUS": "RU", "TUR": "TR", "IDN": "ID", "SAU": "SA",
    "ZAF": "ZA", "ARG": "AR",
}

EURO_AREA = {"DEU", "FRA", "ITA", "ESP"}

# Euro area members in 2020 (ECB rate applies).
EURO_AREA_2020 = {
    "AUT", "BEL", "CYP", "DEU", "ESP", "EST", "FIN", "FRA", "GRC", "IRL", "ITA", "LTU", "LUX", "LVA",
    "MLT", "NLD", "PRT", "SVK", "SVN",
}
