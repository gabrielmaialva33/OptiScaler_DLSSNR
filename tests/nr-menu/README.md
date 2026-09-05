# NR pass menu verification

Run `python tests/nr-menu/run.py` from the repository root. Requires Python and GCC
with C++20, ASan and UBSan. Outputs are temporary and excluded from release packaging.

The suite extracts the actual deferred slider and pass-editor functions from
`DlssNr_Menu.cpp`, then compiles them against scripted ImGui events and a Config facade
using the production sparse settings types. Fifteen frames cover unsupported backends,
one/master defaults, every optional field, live inheritance, explicit overrides,
field/pass clearing, active-pass selection, retained inactive settings and disabling then
reenabling individual tuning. Source guards reject direct master writes and check both
new runtime translation units are registered exactly once in the project and filters.
This complements the real Config transaction/codec and real ImGui hash suites. It does
not render pixels or exercise the actual Windows Config, renderer, NGX or GPU.

## Menu state and adversarial review

| State | Controls and meaning |
| --- | --- |
| Native Vulkan (including before first NR evaluation) or driver proxy | Single/master limitation text; no count or individual editors |
| D3D12, individual off | Requested count 1..3, last-evaluation count/status, individual toggle; no per-pass editor |
| D3D12, individual on, one requested pass | Pass 1 fields; no meaningless pass-selection slider |
| D3D12, individual on, multiple requested passes | Selector limited to requested passes; seven optional overrides |
| Field inherited | Readout of current master; Override can create an explicit value |
| Field explicit | Its editor; unchecking Override clears the sparse value |
| No explicit fields | No clear-pass button |
| At least one explicit field | Inherit-all button removes only the selected pass's overrides |
| Lowered count or individual off | Inactive overrides retained, never silently deleted |
| Renderer restart/refusal/waiting | Actual renderer status shown; requested count is never described as completed work |

The Model section continues to edit master values through short Config setters. The
renderer may rebuild after changes; no Config lock is held over retry or renderer calls.
Deferred values commit on release; pass scopes also separate pending ImGui IDs. The
white-point source and anchor block is untouched by this integration: source 0 has
paper white; source 1 has exposure trim; source 2 without anchors has paper white and no
trim; source 2 with anchors has scan trim, with paper white only while editing a point;
the direction checkbox remains limited to exactly one anchor. The added controls change
neither the white-point settings nor exposure availability conditions.

Translation remains partial: new stable-ID count, selection, override and clear controls
can use the optional Portuguese pack. Legacy model labels and per-field value labels
remain English to preserve their original identity; supported status and explanatory text
translate, unknown diagnostic reasons fall back to English. No complete-menu translation
or in-game layout validation is claimed. Neither optional dictionary activation nor game
installation is performed by these tests.

Backend visibility additionally checks the known input API and whether the current
feature uses the D3D12 bridge, because `IsRunningVk()` alone is false before the first
native NR evaluation. Unknown Vulkan features hide the editor conservatively. This
selection is inspected in source; the scripted ImGui harness receives the resulting
backend flag rather than constructing the real State/feature graph.
