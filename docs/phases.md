# Phase status

Tracks the phases of [`../../implementation_plan.md`](../../implementation_plan.md) and their acceptance criteria.

Legend: ☐ not started · ◐ in progress ☑ done

## ☑ Phase 0 — Repository, build, and test wiring

| Acceptance criterion | Status | Evidence |
|---|---|---|
| Clean checkout configures and builds with one documented command | ☑ | `cmake --preset host-debug && cmake --build --preset host-debug` |
| `ctest` runs with at least one Catch2 test | ☑ | `uwb_unit_tests`, label `unit` |
| Host-only config requires no Pico SDK | ☑ | `host-debug` configures with `PICO_SDK_PATH` unset |
| Agreed top-level directories exist | ☑ | `shared/`, `server-core/`, `simulator/`, `client-core/`, `apps/`, `pico/`, `tests/` |
| Sanitizer preset builds and runs a trivial test | ☑ | `host-asan` preset |
| No UI library linked into protocol/server-core | ☑ | target link lists in `docs/dependency-policy.md` |
| No Pico SDK library linked into shared host targets | ☑ | `uwb_protocol`, `uwb_domain`, `uwb_server_core` link nothing platform-specific |
| CI runs host checks; HIL stays manual | ☑ | `.github/workflows/ci.yml`, `ci/run_local_ci.sh` |

## ☐ Phase 1 — Shared protocol foundation
## ☐ Phase 2 — Server core foundation
## ☐ Phase 3 — Simulator core
## ☐ Phase 4 — Client core foundation
## ☐ Phase 5 — Pico bootstrapping
## ☐ Phase 6 — UWB adapter and local AT engine
## ☐ Phase 7 — Persistent config, LittleFS, reboot semantics
## ☐ Phase 8 — Measurement pipeline
## ☐ Phase 9 — Full routine engine
## ☐ Phase 10 — Discovery, device status, capabilities
## ☐ Phase 11 — Device control CLI and maintenance routines
## ☐ Phase 12 — TUI / GUI clients
## ☐ Phase 13 — HIL and regression hardening
