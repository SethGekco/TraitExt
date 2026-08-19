# TraitExt

A standalone Syringe DLL for Red Alert 2: Yuri's Revenge implementing a unified
**Trait** system: reusable bundles of INI key overrides applied three ways —
static (`$Inherits`-compatible), random (per-type or per-instance pools), and
runtime (building-driven "push" upgrades and prerequisite-unlocked "pull"
self-buffs) — all through one per-key merge engine
(Override/Keep/Add/Multiply/Min/Max/Average).

Designed to **coexist with Phobos** (companion mode): static `$Inherits` is
deferred to Phobos's lazy read-time resolution; TraitExt only owns random
selection and runtime application. Runtime target: Antares + Phobos.

- Full design: [docs/DESIGN.md](docs/DESIGN.md)
- Verified hook seams: the YR Hook Encyclopedia pages `Rules-Load.md` and
  `INI-Read-Inheritance.md`

## Status

Phase P1 scaffold: builds a DLL with a proof-of-life hook at the verified
injection seam (`0x668BF0` = `RulesClass::Read_File` entry — fires for every
rules pass: rulesmd ×3, game mode, map) that logs each pass to the debug log.

## Building

CI builds on GitHub Actions (MSVC, DevBuild config) — no local Windows needed.
Submodules: YRpp (phobos-dev) + Phobos (develop, utility sources only).

```
git clone --recurse-submodules <repo-url>
msbuild TraitExt.sln /p:Configuration=DevBuild /p:Platform=x86
```
