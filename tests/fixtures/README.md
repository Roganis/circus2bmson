# Test fixtures

Small, license-clean tracker modules used by the tests and spikes. All are
sourced from **The Mod Archive**, where they are filed under the
**Public Domain** license.

| File | Format | Title | Source (The Mod Archive) | Module ID |
|------|--------|-------|--------------------------|-----------|
| `10k_reggae_dub.mod` | MOD (M.K., 4ch) | 10k Reggae Dub | https://modarchive.org/index.php?request=view_by_moduleid&query=201827 | 201827 |
| `8bit_castle.mod` | MOD (4CHN) | 8-bit Castle | https://modarchive.org/index.php?request=view_by_moduleid&query=195782 | 195782 |
| `neurosys.xm` | XM (4ch, 7 instruments) | neurosys | https://modarchive.org/index.php?request=view_by_moduleid&query=166135 | 166135 |
| `chip_ultimatum.it` | IT (4ch, 32 instruments) | chip ultimatum | https://modarchive.org/index.php?request=view_by_moduleid&query=36361 | 36361 |

Direct download endpoint (used to fetch them):
`https://api.modarchive.org/downloads.php?moduleid=<ID>`

`10k_reggae_dub.mod` (~10 KB) is the fast default fixture. The Public Domain
pool on The Mod Archive contained no S3M modules at the time of fetching, so
S3M is exercised only indirectly (same code path as IT).
