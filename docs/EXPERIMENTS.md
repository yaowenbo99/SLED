# Experiment guide

## Inputs and protocol

Supply a directory of canonical JSONL files using `--data-dir`. The expected
files are:

| Paper name | Dataset key / filename stem |
|---|---|
| Election | `uk_pcon_2024_boundaries_records.jsonl` |
| Hydrography | `nyc_planimetric_hydrography_records.jsonl` |
| Parks | `nyc_parks_permit_areas_records.jsonl` |
| Parcels | `sla_cadastral_land_parcel_records.jsonl` |
| NYC Taxi Zones (progressive example) | `nyc_taxi_zones_records.jsonl` |

One line represents one record (possibly a MultiPolygon), with `cell_id`,
`geometry`, and `weight.value`. 

The experiment defaults are 1600×1200, one computation thread, streaming mode,
16 rows per batch, APRIL raster resolution 512×512, and interaction seed 42.
The seed is fixed in the implementation.

## Progressive visualization

```sh
# Three timing processes per dataset; no snapshot copying.
python3 scripts/progressive.py --data-dir "$DATA_DIR" --executable build/sled \
  --mode timing --output results/progressive
# A separate process per dataset creates actual live row snapshots.
python3 scripts/progressive.py --data-dir "$DATA_DIR" --executable build/sled \
  --mode snapshots --output results/progressive
```

