# Bridge window takeover

```sh
python3 tests/bridge-window/run.py
```

Compiles the production header `retoque/with_dx12/Dx11wDx12_Window.h` with g++ under ASan/UBSan and drives it with
plain structs for bridges. That header is the device-free half of the fix for a game that creates a second swapchain
on its window while the first one lives (Metal: Hellsinger on Rafael's RTX 3060, 2026-10-13). DXGI allows one
flip-model swapchain per window, so the bridge's second presenter was refused with E_ACCESSDENIED and the title lost
its bridge, and with it the neural pass, for the session. The design is in
`docs/dlssnr/design/nr-dx11-bridge-host.md`, "A second swapchain on the same window".

Checked:

- **The registry.** Live bridges by window. Null windows and bridges are not entries. A bridge is found by its window
  and nothing else, newest first. One whose gate is closed (it already gave its presenter up) is passed over for the
  next newest. A count at zero is never revived, and the search goes on past it. `Remove` takes every entry of that
  bridge and only that bridge.
- **The registry's lock.** `acquire` runs under the lock `Remove` takes. A `Remove` started while `acquire` decides
  returns only after it: this is what keeps the bridge `acquire` is handed alive, since the destructor removes it
  before anything is freed.
- **The presenter gate.**
  - The game's calls nest and overlap, and a takeover is refused while any is in flight.
  - Closed with none in flight, every later call is refused, and a refused call leaves nothing behind.
  - `Reopen` gives a presenter back after a takeover that could not finish, and never opens a gate a call holds.
  - A race of two calling threads against a closing one never closes over a call, and admits none after.
- **The takeover sequence.**
  - Anything but E_ACCESSDENIED is untouched: nothing is found, asked, released or created. That covers S_OK,
    E_FAIL, E_NOINTERFACE (what frame generation answered in the log), DXGI_ERROR_INVALID_CALL, and
    DXGI_ERROR_ACCESS_DENIED, a different code.
  - With no bridge on that window, nothing is created again.
  - Each refusal (busy, frame generation's presenter, undrained) drops the reference it took once, creates nothing
    and returns the original result.
  - A takeover runs in the order surrender, release, create, each once. The result is the retry's, success or not.
  - A third swapchain on the window finds nothing left to take, and a quarantined bridge (count at zero) is not
    taken.
  - Every outcome has a log text of its own.

What the production class does with it is `bridge-lifetime`'s `takeoverCases`. That suite runs the production surrender,
`Present`, both resizes, fullscreen and the final release over its scripted COM fakes. This suite runs no D3D11,
D3D12 or DXGI. Whether Windows really frees the window when the presenter is released, so that the retry succeeds,
needs a game on native Windows.
