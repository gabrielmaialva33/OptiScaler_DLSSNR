# NGX shutdown adapter

`python3 tests/nr-shutdown/run.py` compiles the production core-shutdown typedefs,
`ShutdownDx12Device` adapter and public-signature getter with host fakes under ASan/UBSan.

The fake core writes to the mandatory second argument, verifies its initial value and the forwarded
device, and returns success/failure codes. Tests verify that results are preserved, uninitialized or
missing entries are not exposed, and a null device selects the all-devices API instead of dereferencing
null inside the core. The public callable retains the SDK's one-argument type.

These are host behavior checks; the driver ABI is established separately through binary inspection
and the real Proton loopback. See `OptiScaler/dlssnr/design/ngx-shutdown-order.md` for that evidence.
