# TraitExt — test guide

Everything below is **load-time** behaviour: it resolves once while rules are
read, and every change is written into the INI before the game parses it. That
means you verify it two ways — in `debug.log` (exact `base -> result` lines) and
in-game (unit stats actually differ).

## Setup

1. TraitExt.dll is in the RA2 folder and listed in
   `Resources/Compatibility/Unix/wine-game.sh` as `-i=TraitExt.dll`.
   **That file is the real inject list on Linux, not ClientDefinitions.ini.**
2. Paste the test block below into `rulesmd.ini` (anywhere — position never
   matters, see "Ordering" below).
3. Launch, start a skirmish, then check `debug.log` for `[TraitExt]` lines.

## Test block

```ini
[TraitExt]
RandomSeed=12345          ; change this to get a different (still sync-safe) draw

[TraitTypes]
0=T_Beefy
1=T_Cheap
2=T_Absolute
3=T_VoiceA
4=T_VoiceB
5=T_VoiceC

; --- merge modes -------------------------------------------------------
[T_Beefy]
Merge=Addition            ; numeric keys +=, non-numeric Override
Strength=+200             ; sigil also works per-key
Armor=heavy               ; not numeric -> Overrides

[T_Cheap]
Merge=Multiply
Cost=0.5                  ; halves whatever Cost resolved to so far

[T_Absolute]
Merge=Override
Strength=500              ; wins outright if listed last

; --- random pool members ----------------------------------------------
[T_VoiceA]
Merge=Override
VoiceSelect=1
[T_VoiceB]
Merge=Override
VoiceSelect=2
[T_VoiceC]
Merge=Override
VoiceSelect=3

; --- targets -----------------------------------------------------------
[GGI]
Traits=T_Beefy,T_Cheap    ; Strength +200 then Cost x0.5

[E1]
Traits=T_Beefy,T_Absolute ; Strength ends at 500 (Override is last)

[E2]
Traits=T_Beefy
BlockTraits=T_Beefy       ; blocked -> nothing happens

[HTNK]
TraitsRandom=T_VoiceA,T_VoiceB,T_VoiceC
TraitsRandomCount=1,1     ; exactly one, deterministic per RandomSeed
```

## What to expect in debug.log

```
[TraitExt] Read_File pass 1: ... (INI_Rules)
[TraitExt] trait 'T_Beefy' Merge=Add keys=2
...
[TraitExt]   GGI.Strength: '120' -> '320'
[TraitExt]   GGI.Cost: '400' -> '200'
[TraitExt]   E1.Strength: '100' -> '500'
[TraitExt] E2: trait 'T_Beefy' blocked
[TraitExt] HTNK: random pool 1/3 picked
[TraitExt] applied traits to N target(s) from 6 trait definition(s)
```

Also note the **pass count** — that is the empirical answer to how many rules
passes really fire (predicted: rulesmd x3 via `RulesClass::Init`, plus game mode
and map). Traits apply on the first pass only; later passes are skipped by the
`$Applied` guard.

## Checklist

| # | Test | Pass condition |
|---|------|----------------|
| 1 | Hook fires | `Read_File pass N` lines exist |
| 2 | Registry parsed | one `trait '...' Merge=... keys=N` line per trait |
| 3 | Add | `GGI.Strength: '120' -> '320'` |
| 4 | Multiply | `GGI.Cost` halved |
| 5 | Non-numeric under Add | `GGI.Armor` becomes `heavy`, not a number |
| 6 | Order / Override wins | `E1.Strength` ends at `500` |
| 7 | Block | `E2` reports blocked, `E2.Strength` unchanged |
| 8 | Random | `HTNK: random pool 1/3 picked`, one VoiceSelect written |
| 9 | Determinism | same `RandomSeed` -> identical picks across runs |
| 10 | Idempotency | Strength does **not** grow on later passes |
| 11 | Ordering-free | move `[T_Beefy]` to the very bottom of the file; results identical |
| 12 | In-game | GGI visibly tougher / cheaper than vanilla |

## Ordering (why position never matters)

The old Ares/Antares `[Child]:[Parent]` header syntax copies at parse time, so
the parent had to appear **above** the child. TraitExt never has that constraint:
traits resolve after the whole INI is parsed. Test 11 proves it.

## Known limits (by design, this phase)

- **Numeric folds need an INI-visible base.** `Add`/`Multiply` read the target's
  current value via the INI (including anything inherited through `$Inherits`).
  If a key isn't written anywhere in the INI, the engine can't know the engine's
  built-in default, so it logs a WARN and applies the trait value as `Override`.
  In practice most rules keys are present in `rulesmd.ini`.
- **Scalars only.** Comma-lists (e.g. `Prerequisite=`) are treated as
  non-numeric, so they Override rather than merge element-wise.
- **Targets are TechnoTypes** — sections listed in `[VehicleTypes]`,
  `[InfantryTypes]`, `[BuildingTypes]`, `[AircraftTypes]`.
- **Random is per-type and deterministic**, not per-instance and not per-match.
  Per-instance random (each soldier a different voice) is a later phase and needs
  the synced-RNG work.
- **Runtime verbs are not implemented yet** — building-driven "push" upgrades and
  prerequisite-unlocked "pull" self-buffs are the next phase.
