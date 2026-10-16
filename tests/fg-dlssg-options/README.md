# DLSS-G option caching

Run `python3 tests/fg-dlssg-options/run.py` on the host with g++.

The runner compiles production `DLSSG_Common::SetOptions`, its cache declarations and
`StreamlineHooks::applyMenuDlssgInterlock` with real Streamline SDK option types under
AddressSanitizer and UndefinedBehaviorSanitizer. Only configuration, menu visibility,
marker/generation observations and SDK exports are fake; the exports retain their
arguments and can return transient errors.

Thirteen cases cover successful duplicate suppression, independent DLSS-G and Reflex
retries after failure, external option generation changes (including failures during
those resends), the MFG eOff/eOn transition, retries of a rejected eOff, reversal to
the previously cached multiplier before resuming, count changes during failed
transitions, the menu during MFG restart, opening/closing the menu,
dynamic targets, Reflex boost and API-specific marker handling. The failure cases
catch caches populated before the SDK accepts their values: a failed call must be
retried next frame, and a successful retry must then suppress duplicates.

This validates host policy and SDK arguments. It does not execute Streamline plugins,
NvAPI, Vulkan/D3D12, GPU interpolation or real-game pacing.
