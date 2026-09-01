"""
DISCO-2 pass prediction for the Aarhus University TT&C ground station.

Aarhus University (AU) is DISCO-2's sole uplink TT&C station, so this script
predicts passes for AU only. This matches the thesis, which cites AU-only
figures (Chapter 2). A receive-only observation station at the IT University
of Copenhagen extends downlink coverage but provides no additional uplink
opportunity, so it is not modelled here.

  - Aarhus University (AU) -- confirmed, 56.167 N, 10.200 E (disco2_req_submitted.pdf, sec 7.2)

TLE source: celestrak.org (epoch 2026 day 131.880 = 2026-05-11 21:07 UTC)
Propagator: SGP4 via Skyfield
"""

from skyfield.api import EarthSatellite, load, wgs84
import numpy as np

TLE_LINE1 = "1 68431U 26067R   26131.88028413  .00004272  00000+0  21018-3 0  9999"
TLE_LINE2 = "2 68431  97.4579  90.9313 0004227 107.1747 252.9950 15.18533203  6438"

# Station configuration.
# elevation_mask_deg: minimum elevation for geometric visibility
# useful_max_el_deg: max-elevation threshold for a productive 4800 bit/s session
STATION_NAME = "Aarhus University (AU)"
STATION = {
    "lat": 56.167,
    "lon": 10.200,
    "elevation_m": 20,
    "elevation_mask_deg": 5,
    "useful_max_el_deg": 20,
}

N_DAYS = 14

ts = load.timescale()
sat = EarthSatellite(TLE_LINE1, TLE_LINE2, "DISCO-2", ts)
t0 = ts.utc(2026, 5, 12, 0, 0, 0)
t1 = ts.utc(2026, 5, 26, 0, 0, 0)


def compute_passes_for_station(station_cfg):
    """Return list of pass dicts: rise, culm, set, duration_s, max_el_deg."""
    topo = wgs84.latlon(
        station_cfg["lat"], station_cfg["lon"], elevation_m=station_cfg["elevation_m"]
    )
    mask = station_cfg["elevation_mask_deg"]
    times, events = sat.find_events(topo, t0, t1, altitude_degrees=mask)
    passes = []
    cur = {}
    for t, e in zip(times, events):
        if e == 0:
            cur = {"rise": t}
        elif e == 1 and "rise" in cur:
            cur["culm"] = t
        elif e == 2 and "rise" in cur:
            cur["set"] = t
            cur["duration_s"] = (
                cur["set"].utc_datetime() - cur["rise"].utc_datetime()
            ).total_seconds()
            if "culm" in cur:
                alt, _, _ = (sat - topo).at(cur["culm"]).altaz()
                cur["max_el_deg"] = alt.degrees
            passes.append(cur)
            cur = {}
    return passes


def report_station(name, cfg, passes):
    durs = np.array([p["duration_s"] for p in passes])
    els = np.array([p.get("max_el_deg", 0) for p in passes])
    daily = {}
    for p in passes:
        daily.setdefault(p["rise"].utc_datetime().date(), []).append(p)
    counts = [len(v) for v in daily.values()]
    useful = [p for p in passes if p.get("max_el_deg", 0) >= cfg["useful_max_el_deg"]]

    print(f"=== {name} ===")
    print(
        f"  Coords: {cfg['lat']:.3f} N, {cfg['lon']:.3f} E"
        f"   mask={cfg['elevation_mask_deg']} deg"
        f"   useful>={cfg['useful_max_el_deg']} deg"
    )
    if not passes:
        print("  No passes in window.\n")
        return useful
    print(f"  All passes:    {len(passes)} total, {len(passes)/N_DAYS:.2f}/day "
          f"(range {min(counts)}-{max(counts)})")
    print(f"  Duration (min): min={durs.min()/60:.1f}  "
          f"mean={durs.mean()/60:.1f}  max={durs.max()/60:.1f}")
    print(f"  Max elev (deg): min={els.min():.1f}  "
          f"mean={els.mean():.1f}  max={els.max():.1f}")
    if useful:
        u_durs = np.array([p["duration_s"] for p in useful])
        print(f"  Useful passes: {len(useful)} total, {len(useful)/N_DAYS:.2f}/day, "
              f"mean duration {u_durs.mean()/60:.1f} min")
    else:
        print("  Useful passes: 0")
    print()
    return useful


def main():
    print(f"DISCO-2 pass prediction over {N_DAYS} days from {t0.utc_iso()} UTC")
    print(f"Orbital period: {1440/15.18533:.2f} min, altitude ~{6889.7-6378.137:.1f} km")
    print()

    passes = compute_passes_for_station(STATION)
    report_station(STATION_NAME, STATION, passes)


if __name__ == "__main__":
    main()
