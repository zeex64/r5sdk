# CrouchSpamGuard

`CrouchSpamGuard` is an authoritative R5SDK server plugin with an optional
client prediction component. The server filters `IN_DUCK` immediately before
`CPlayer::PlayerRunCommand` processes each user command. When clients install
the same DLL, it also filters commands after `CInput::CreateMove` and before
local prediction, avoiding repeated reconciliation while crouch is held.

The Release build is written to:

```
game/bin/x64_retail/plugins/CrouchSpamGuard.dll
```

## Configuration

| ConVar | Default | Description |
| --- | ---: | --- |
| `sv_crouchspam_enable` | `1` | Enables authoritative crouch filtering. |
| `sv_crouchspam_min_hold_ms` | `150` | Minimum accepted crouch duration, in milliseconds. |
| `sv_crouchspam_debounce_ms` | `100` | Minimum interval between accepted crouch state transitions, in milliseconds. |
| `cl_crouchspam_predict` | `1` | Enables the optional client prediction filter. |

Both timing cvars are clamped to the range `0`-`2000`. A crouch press uses the
larger of the hold and debounce windows. A stand transition uses the debounce
window, preventing an immediate re-crouch.

The three `sv_` cvars are replicated so an installed client uses the server's
timing. Clients without the DLL remain compatible and are still subject to
server enforcement, but can briefly predict a transition that the server
filters. Install the DLL in the normal plugin directory on a client to enable
the prediction component; set `cl_crouchspam_predict 0` to disable it locally.
