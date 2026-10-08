# bc7enc_rdo (vendored)

BC7 / BC1-BC5 block encoders from https://github.com/richgel999/bc7enc_rdo (`bc7enc.cpp/h`, `rgbcx.cpp/h`,
`rgbcx_table4_small.h`; `bc7decomp.cpp/h` is the reference decoder, used by the tests only). License: MIT or public domain, your choice (see LICENSE; `bc7e.ispc`, which we do not
use, is Apache 2.0). Fetched from the `master` branch on 2026-10-08.

We use `bc7enc_compress_block` for BC7 and `rgbcx::encode_bc5` / `encode_bc4` for normal and single-channel
data. Both need one-time initialization (`bc7enc_compress_block_init`, `rgbcx::init`) and are thread safe afterwards;
engine/assets/src/import/texture_cook.cpp does that. To update, replace the files together and re-run the asset tests.
