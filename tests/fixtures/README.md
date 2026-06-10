# Test fixtures

Small, license-clean tracker modules used by the tests and spikes.

Both modules are 4-channel ProTracker (`M.K.`) MODs sourced from
**The Mod Archive**, where they are filed under the **Public Domain** license.

| File | Title | Source (The Mod Archive) | Module ID | License |
|------|-------|--------------------------|-----------|---------|
| `10k_reggae_dub.mod` | 10k Reggae Dub | https://modarchive.org/index.php?request=view_by_moduleid&query=201827 | 201827 | Public Domain |
| `8bit_castle.mod` | 8-bit Castle | https://modarchive.org/index.php?request=view_by_moduleid&query=195782 | 195782 | Public Domain |

Direct download endpoint (used to fetch them):
`https://api.modarchive.org/downloads.php?moduleid=<ID>`

`10k_reggae_dub.mod` (~10 KB) is the fast default fixture; `8bit_castle.mod`
(~59 KB) is a richer real-world module for the same checks.
