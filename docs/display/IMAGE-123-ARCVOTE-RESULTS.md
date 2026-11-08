# Image 123 — votes real, PERF NACKed (outcome D)

## ARC tables (runtime, all from existing OPP entries)
- `cmd_db counts gfx.lvl=16 cx.lvl=16 mx.lvl=16`.
- GPU (`2c00000.qcom,kgsl-3d0`): `0->0/0x0`, `257M->65/0x00800204`,
  `345M->129/0x00c00305`, `427M->193/0x00e00406`, `499.2M->225/0x01000407`,
  `585M->257/0x01400508`, `675M->321/0x01800609` (monotonic, sane).
- GMU (`2c6a000.qcom,gmu`): `0->0/0x0`, `19.2M->49/0x00400202`,
  `200M->49/0x00400202`, `500M->129/0x00c00304`.
- No lookup failure (no C-case); no fallback used.

## Bootstrap (D: PERF itself NACKed)
- `FW_VERSION -> ACK (id=1, error=0)`.
- PERF (`gpu=7, gmu=2`, real votes, `acd=0xffffffff`) → NACK:
  `HFI RX: header=0x00110340 id=64 type=1 size=3 seq=1`,
  words `00110340 0d926540 0000006f`, `error=111`.
  `send_perftbl ret=-110`. BW/ECP/core not attempted (stop rule).
- NACK framing varies run to run (`id=0/type=4` in 121, `id=64/type=2`
  here); zero-vote PERF was ACKed 3/4 times (118/120/122) and NACKed
  once (121). Single samples — no vote-causality claim; firmware
  response to PERF is nondeterministic across runs.

## Remaining items
- SMMU faults/panics: 0. Device alive (uptime 148s), gate held.
- First subsequent failure: PERF NACK.

Image 123 `d08be177852aecf4d60113694d616c1694f6fd18621b3128d3d50dd9dd05e568`.
SDE 0, DT = 121 content, production untouched. Stopping.
