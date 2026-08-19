# TraitExt — Design

A unified inheritance / trait / dynamic-modifier system for Red Alert 2: Yuri's
Revenge, built as a standalone Syringe DLL that coexists with Phobos.

Status: design draft. Nothing here is committed to code yet.

---

## 1. The one idea

There is exactly **one primitive** and **three ways to apply it**.

- **Trait** (noun): a named, reusable bundle of INI key overrides. It lives in a
  section that is not itself a spawnable type. It is a *superset* of Phobos
  `$Inherits`: `$Inherits` merges from a real unit section, a Trait merges from a
  dummy section — same merge, different source.
- **Apply verbs**:
  - **Static** — a fixed list of Traits (this is `$Inherits` parity).
  - **Random** — pick N Traits from a pool (`min,max`, default `1,1`).
  - **Conditional / runtime** — apply Traits while some game state holds
    (a building exists, a tech level is met, an aura is present…). This is the
    generalized, modernized version of Phobos discussion #963 (Industrial Plant
    Expansion), without being tied to its specifics.

All three verbs produce the same thing: **an ordered set of Traits applied to a
target**. That set is fed to a single **merge engine** with pluggable per-key
conflict modes. Random-pick conflicts, upgrade stacking, and multi-parent
inheritance are therefore *the same code path*.

```
            static list ─┐
   random pick (N of M) ─┼─▶ ordered set of Traits ─▶ merge engine ─▶ result
    conditional/runtime ─┘        (per target)          (per-key modes)
```

---

## 2. The primitive: a Trait

```ini
; A Trait is a section that is not a real type. Convention: prefix to avoid
; colliding with unit names. (Prefix TBD — e.g. "T_" or a registered list.)
[T_HeavyArmor]
Armor=heavy
Strength=+200        ; leading + => this key uses the Add merge mode
Cost=*0.75           ; leading * => Multiply merge mode
```

A Trait carries only the keys it wants to override. Anything it does not mention
is left untouched by that Trait.

---

## 3. Apply verbs

### 3.1 Static (= `$Inherits` parity)

```ini
[MyTank]
$Inherits=HTNK,T_HeavyArmor   ; parent may be a real type OR a Trait
```

**Phobos semantics (verified from source, 2026-08-18):** the child's own key
always wins; on a miss the parent list is searched **left→right, FIRST-found
wins** (depth-first through each parent's own `$Inherits`). NOT last-wins.
Phobos has **no cycle detection** (a `$Inherits` cycle = stack overflow) —
TraitExt should validate Trait graphs for cycles at load and reject them, since
Traits are our sections; real-unit cycles remain the modder's problem under
Phobos. This is the parity baseline.

### 3.2 Random

```ini
[Conscript]
$RandomPool=T_VoiceA,T_VoiceB,T_VoiceC
$RandomCount=1,1              ; min,max ; default 1,1
$RandomScope=Instance         ; Instance = each spawned unit rolls its own
                              ; Type     = one roll shared by the whole type
```

Selection is just a strategy that emits Traits into the same applied set. When
`RandomCount` > 1 and the picks conflict, the merge engine resolves them exactly
like any other multi-Trait conflict (§4).

### 3.3 Conditional / runtime — ONE engine, two directions, pluggable condition

All the "runtime" ideas (Industrial-Plant / #963 push, building-grants-to-all,
prerequisite-unlocked self-buffs) are the **same machinery**:

> *While condition **C** holds for scope **S**, fold trait set **T** into the
> effective stats of the selected targets; when C stops, recompute without T.*

They differ on only three parameters:

- **Direction** — who the targets are:
  - **Push (outward):** a source object applies traits to *other* matching types
    (a building buffs all vehicles). Trigger = the source's lifecycle.
  - **Pull (self):** a type carries its *own* traits that activate on *its owner*
    meeting a condition (a unit gains a buff once you have a Battle Lab). Trigger =
    the owner's tech-state change.
- **Condition C** — pluggable: `Exists` (source present), `Prerequisite` (a
  Requirement expression true for the owner — prereqs / RequiredHouses / tech
  level), later `AttachEffect`, `Veterancy`, etc.
- **Merge** — each trait's mode (§4).

#### 3.3a Push — building applies outward (the #963 blueprint, cleaned up)

Rex's blueprint, re-expressed in unified vocabulary. Each application is a named
**rule** (traits + filter + optional condition) rather than fragile parallel
`.X` arrays — rules apply in list order, last wins for `Override`:

```ini
[InheritanceBuilding]          ; a building (a Trait *source*)
Applications=BuffAll,OverrideStructures   ; ordered; later applies later
[BuffAll]
Traits=ProduceCashAdd,ProduceCashMult     ; order within = merge order
To=Infantry,Vehicles                       ; category filter (white/blacklist)
[OverrideStructures]
Traits=ProduceCashOverride
To=Buildings

[T_ProduceCashAdd]             ; a Trait; Merge sets its default fold mode
Merge=Addition                 ; numeric keys += ; non-numeric silently Override
ProduceCashAmount=300
Strength=49
Radar=yes                      ; not numeric → Overrides (as Rex noted)
[T_ProduceCashMult]
Merge=Multiply                 ; numeric keys *= (applied after Add if listed after)
Strength=2.0
[T_ProduceCashOverride]
Merge=Override                 ; absolute reset; ignores prior contributors
Strength=500
```

Opt-out on the target side:
```ini
[InfantrySoldier]
BlockTraitSource=InheritanceBuilding          ; refuse everything from this source
[SomeBuilding]
BlockTraits=T_ProduceCashMult                 ; refuse one trait, any source
BlockTraitSource=InheritanceBuilding.OverrideStructures  ; refuse one rule
```
Block is absolute (block > apply). Note `Merge`/`Traits`/`To`/`BlockTraits`
replace the misspelled, `$Inherits`-colliding `Inheritence*` names.

#### 3.3b Pull — prerequisite-unlocked self-traits (the "reverse" idea)

Same engine, `Direction=Self`, `Condition=Prerequisite`. A type's own traits
switch on when *its owner house* satisfies a Requirement, off when it doesn't:

```ini
[EliteConscript]
SelfTraits=T_VeterancyBoost
SelfTraits.Requirement=BattleLab,SovietTech   ; unlock condition (per owner house)
```

- Evaluated **per owner house** (prereqs are house-scoped) → per-instance/layered.
- Re-evaluated when the owner's tech state changes (building gained/lost, capture).
- The Requirement expression should be **PrerequisiteExt's `Requirement`
  primitive**, not a reimplementation — TraitExt *consumes* it, exactly like it
  defers `$Inherits` to Phobos. Compose, don't duplicate. See
  `prerequisiteext-project`.

#### 3.3c Graduated tiers (powerful, but no new syntax)

A "tier" is just a pull rule with its own condition — so tiers are **the same
rule list**, and binary is simply the one-tier case. Nothing new to learn:

```ini
[EliteConscript]
SelfTraits=Rookie,Trained,Elite      ; ordered tier rules
[Rookie]
Requirement=BattleLab
Traits=T_Boost1
[Trained]
Requirement=Reactor>=2               ; count threshold (see note)
Traits=T_Boost2
[Elite]
Requirement=BattleLab,Reactor,Nuke
Traits=T_Boost3
```

- **Default = cumulative:** every satisfied tier folds in, in list order, through
  the §3.4 accumulator. So Rookie+Trained both active = both apply.
- **`SelfTraits.Exclusive=yes`** flips to **highest-satisfied-only** — the classic
  "rank" feel (Elite replaces Trained rather than stacking on it). One flag, two
  behaviors.

**Count thresholds** (`Reactor>=2`) belong in the **Requirement grammar**, i.e.
extend PrerequisiteExt's primitive (count + comparison), so every consumer gets
them — don't special-case them in TraitExt.

**Where the complexity boundary sits (kept off the friendly path):**
- *Discrete* scaling = list tiers (above). Covers "level 1/2/3", "with/without
  Battle Lab". This is the user-friendly default.
- *Continuous per-count* scaling ("+50 Strength **per** owned Reactor") is the one
  genuinely advanced knob — an optional `ScalePerCount=Reactor` on a rule. Off by
  default; power users only.
- **Push already scales by count for free:** N source buildings = N contributors
  through the merge accumulator (2 Industrial Plants → `×0.75` twice). So only the
  *pull* direction ever needs `ScalePerCount`; push never does.

Sync: tier state is derived from the owner's (synced) tech state, re-evaluated on
tech change, folded in deterministic list order → sync-safe by construction.

### 3.4 Merge accumulator (formalized, from the blueprint)

Effective value of a key = start from the statically-resolved base (vanilla +
`$Inherits`), then fold each applied trait **in application order**:

| Trait `Merge` | Effect on running value `v` (given trait value `x`) |
|---|---|
| `Addition` | `v += x` (numeric only; non-numeric → Override) |
| `Multiply` | `v *= x` (numeric only; non-numeric → Override) |
| `Override` | `v = x` (absolute; discards prior contributors) |

So Rex's `Strength` example resolves to **500**: `base` → `+49` → `×2.0` → `=500`
(Override is last in order, wins). `Radar` (non-numeric) always Overrides. This is
the same per-key model as §4 — `Merge` is just the per-*trait* default, still
overridable per key.

---

## 4. The merge engine & conflict modes

Every apply verb ends with an **ordered list of contributors per key**. The
engine reduces that list with a per-key mode. Mode is chosen by (in priority
order): a key sigil (`+`/`*`), an explicit per-key policy, or the application's
`$ConflictDefault`, falling back to global `Override`.

| Mode | Reduces to | Order-sensitive | Notes |
|------|------------|-----------------|-------|
| `Override` (default) | last contributor | **yes** | Phobos-compatible |
| `Keep` | first contributor | **yes** | first-wins |
| `Add` | sum (numeric) | no | stacking upgrades |
| `Multiply` | product | no | Industrial-Plant-style |
| `Min` / `Max` | clamp | no | |
| `Average` | sum ÷ count | no | needs full set, not pairwise |
| `Error` | refuse | no | authoring safety net |

This directly answers the "multiple random picks" question: **first-instance /
last-instance / accumulate / average are simply `Keep` / `Override` / `Add` /
`Average`.** Random inherits the whole table for free.

**Determinism rule (sync safety):** `Override` and `Keep` depend on order, so the
contributor list is sorted by a key that is identical on every client before
reduction:
- static → declaration order
- random → selection order from a synced roll (for Logical keys; see §5)
- runtime → a declared priority, with a stable tiebreak (e.g. building index)

`Add`/`Multiply`/`Min`/`Max`/`Average` are commutative, so they are order-safe by
construction.

> Possible extension (not in v1): weighted Average — each contributor carries a
> weight. Cheap to add later; left out to keep v1 lean.

---

## 5. Safety model — the guarantee of "no desync, no slowdown"

Two rules make the two constraints structural rather than something to keep
testing for.

### Rule 1 — every overridable key is classified Cosmetic or Logical (in the tool, not the modder's job)

- **Cosmetic** (image, cameo, voice/EVA, sequence…): may be selected/applied
  **per-client with an unsynced RNG**. Never touches game state, so it cannot
  desync.
- **Logical** (weapon, armor, strength, speed, cost, ROF…): must resolve
  **identically on every client**. Any randomness here draws from the game's
  **synchronized** RNG. This is the exact failure mode of the Kratos
  `std::minstd_rand` desync — mixing unsynced randomness into synced logic — and
  the classification is the wall that prevents it.

### Rule 2 — materialize when you can, layer only when you must

- **Static + runtime-conditional → materialize.** Compute merged concrete values
  *once*, when the applied-Trait set changes (load time, or when a building goes
  up/down). Between those events there is **zero per-frame cost**. Because the
  changes are driven by synced state with deterministic ordering (§4), the
  materialized result is identical on all clients.
- **Per-instance variation → store a compact selection (Trait indices) on the
  unit's Ext, resolve at point of use.** Random voice per soldier = roll one
  index at spawn, look it up when the voice plays. An indexed lookup, not a stat
  recompute.
- **Never recompute a unit's effective Logical stats per frame** — only when its
  applied-Trait set changes. This caching is what keeps "one building buffing 40
  tanks" cheap.

### Per-type vs per-instance

Type classes in YR are shared singletons, so this choice drives the whole cost
model:

- **Per-type**: change the type; every unit of it updates. Cheap, materialized,
  but no per-unit divergence.
- **Per-instance**: each affected unit carries its own applied set. Enables
  "only tanks near the building," partial/random upgrades, per-soldier randomness
  — at the cost of the layered path + cache.

Random-per-instance forces the layered path to exist anyway. **DECIDED
(2026-07-20): per-instance is the semantic model; per-type materialization is an
automatic optimization** applied whenever a target's applied set is uniform across
all instances of its type (no positional/random/per-unit filter). Capability by
default, cheap path for free when the situation allows — one model, no either/or.

---

## 6. Findings from the Hook Encyclopedia (2026-07-20)

Reconnaissance against the address registry changed the plan. Key result:

**Phobos already implements `$Inherits` at the INI-READ level, not as a flatten.**
`src/Misc/Hooks.INIInheritance.cpp`:
- `0x474230 CCINIClass_Load_Inheritance` — builds the per-section parent map at
  INI load.
- `0x528BAC INIClass_GetString_Inheritance_NoEntry` — the core fallback: on a key
  miss in the child section, walk its `$Inherits` chain.
- `0x5276D0/0x527920/0x5283D0/0x5295F0/0x529880/0x529CA0` — typed
  `ReadInt/GUID/Double/Bool/Point2D/Point3D` overwrites routing through the same
  fallback.

Consequences:
1. **There is no "flatten seam timing" problem for static inheritance.** Types are
   registered normally (modder lists them in `[VehicleTypes]` etc.); inheritance
   only affects *value reads*. New-type creation mid-load is NOT required.
2. **Do not reimplement `$Inherits` when Phobos is present** — hooking the same
   address family collides head-on with a maintained feature. TraitExt should
   **defer** to Phobos for static resolution (resolves old Open Decision #2).
3. TraitExt's genuinely new code is therefore only **random selection** and
   **runtime application** — static Traits-as-parents come *for free* via Phobos
   the moment a Trait is just an ordinary section a `$Inherits` points at.

**Rules-load family (for the runtime/materialize paths, not static):**
- `0x668F6A` — AMBIGUOUS: registry lists it as both `RulesClass_Read_File_LoadTypes`
  and `RulesData_InitializeAfterAllLoaded`. **CONTESTED** — our own
  `Country_Extension` and the `AggressiveStance` fork already hook it
  (`RulesData::InitializeAfterAllLoaded`). Needs binary verification before use;
  avoid if possible.
- `0x679CAF RulesData_LoadAfterTypeData` (Ares/Antares/Phobos) — clean late seam,
  after all type data is read; good for materializing type-level merges.
- `0x668BF0 RulesClass_Addition` — map-rules override combine.
- `0x6873AB INIClass_ReadScenario_EarlyLoadRules` (Ares/Antares) — map-rules early
  load timing.
- `0x474200/0x474314 CCINIClass_ReadCCFile1/2` (Ares/Antares) — `[#include]`
  support; relevant to "works in maps + includes" correctness.

## 6b. Revised architecture: companion vs standalone

- **Companion mode (default, recommended).** Assume Phobos is loaded. Do NOT touch
  the INIInheritance address family. TraitExt owns only:
  - **Random**: **no Phobos ordering constraint** (verified 2026-08-18 — see
    below). Phobos resolves `$Inherits` lazily at read time (`0x528BAC`), so
    TraitExt simply writes the rolled `$Inherits` value / keys into the in-memory
    `CCINIClass` at any point after the INI (incl. `[$Include]`s) is fully loaded
    and before the FIRST read of the affected section — e.g. at rules-read start.
    The former "must run before `0x474230`" requirement was based on that hook's
    misleading name; it actually implements `[$Include]`, not `$Inherits`.
    Caveat: never inject after a section has already been read once — Phobos
    caches the section's `$Inherits` string on first miss. (Logical picks use
    synced RNG; cosmetic use unsynced — §5.)
  - **Runtime**: building-driven apply/remove on existing types/instances, via the
    rules-load + building lifecycle hooks below. Independent of Phobos INI code.
- **Standalone mode (optional, later).** No Phobos present → TraitExt must
  reimplement the read-level fallback itself (its own hooks equivalent to the
  INIInheritance family). Mutually exclusive with Phobos on those addresses; only
  for Phobos-less setups.

### Data model
- `TraitDef` — parsed once: key → (value, merge-mode hint, cosmetic/logical class).
- `AppliedSet` — ordered contributor list attached to a target (type-level for
  materialized, instance-level on Ext for layered).
- `MergeEngine` — reduces an `AppliedSet` per key using §4 modes.

### Hook points (candidates from registry; ✔ = registry-confirmed, ⚠ = HEURISTIC,
must be Ghidra-verified against gamemd.exe before trusting — the registry is an
unverified triage index)

**Load / static (P1–P2 injection):**
1. ✔✔ `0x474230` — BINARY-VERIFIED 2026-08-18: tail of `CCINIClass::ReadCCFile`
   (entry `0x474200`), fires post-parse per file. Phobos's hook here does
   `[$Include]`, NOT `$Inherits` (name misleads). TraitExt does not hook it and
   has no ordering constraint against it; injection seam = after full INI load,
   before first section read. See encyclopedia/INI-Read-Inheritance.md.
2. ✔✔ `0x679CAF RulesData_LoadAfterTypeData` — PDB-confirmed name; clean late
   seam; materialize type-level (uniform) merges after type data is read.
   (Too late for random *injection* — keys already read by then.)
3. ✔✔ **`0x668BF0 RulesClass::Read_File` entry = THE injection seam (pinned
   2026-08-18).** YRpp identity confirmed; binary-verified that `RulesClass::Init
   (0x6686C0)` calls it 3× internally (`0x668A27/0x668B05/0x668BAA`), and it also
   runs for the game-mode INI and the map INI — so ONE cooperative entry hook
   sees EVERY rules pass, with `ECX=RulesClass*`, stack+0x4 = `CCINIClass* pINI`.
   TraitExt hooks here (chaining beside Phobos's `return 0` RulesClass_Addition),
   writes rolled `$Inherits`/keys into `pINI` before the vanilla readers run.
   `0x668F6A` = the TAIL of this same function (registry ambiguity resolved:
   Phobos stacks two hooks there — deferred LoadTypesFromINI +
   InitializeAfterAllLoaded). See encyclopedia/Rules-Load.md.
   **Seed-timing caveat for Logical per-type randoms:** the rulesmd passes run at
   program init, likely BEFORE the scenario's synced seed is set. Cosmetic rolls
   may happen any pass; Logical per-type rolls must resolve during scenario-load
   passes (map/mode `Read_File`, seed known) or defer resolution to
   LoadTypesFromINI time. Verify seed-set timing vs the three Init calls before
   coding P2 logical randoms.

**Per-instance store (P2 random, P3 per-instance upgrades):**
3. ⚠ `0x6F3260 TechnoClass_CTOR` — roll + store random Trait indices on Ext for
   units/infantry/aircraft.
4. ⚠ `0x43BCBD BuildingClass_CTOR` — same for buildings.

**Point-of-use resolvers (read the stored index):**
5. Weapon (LOGICAL → synced roll): ⚠ `0x6F3360 TechnoClass_WhatWeaponShouldIUse_MultiWeapon`
   (Phobos already here — chain cooperatively) / `0x6F3330 TechnoClass_SelectWeapon`
   (Ares/Antares).
6. Voice (COSMETIC → unsynced): ⚠ `0x7090A0 TechnoClass_VoiceAttack`,
   `0x708FC0 TechnoClass_ResponseMove`, `0x7090A8 SelectFiringVoice`.
7. Image/cameo: harder — type-level art. Per-instance image likely needs draw-time
   hooks; treat as an advanced case, not P2 baseline. *(candidates TBD.)*

**Runtime PUSH (P3, §3.3a) — mirror Phobos's FactoryPlant lifecycle:**
8. ⚠ `0x441501 BuildingClass_Unlimbo_FactoryPlant` region / `0x43BCBD Unlimbo` —
   building becomes active → add its Trait set to matching targets.
9. ⚠ `0x445D87 BuildingClass_Limbo_*` — building removed → remove its set.
10. ⚠ `0x448A31 / 0x449149 BuildingClass_Captured_FactoryPlant1/2` — ownership
    change → re-evaluate applicability. (Sell/deploy fold into Limbo/Unlimbo.)

> Precedent: Phobos implements the vanilla Industrial Plant at exactly hooks 8+10,
> so the #963-style PUSH verb reuses proven lifecycle seams rather than new RE.

**Runtime PULL / prerequisite-unlock (P3, §3.3b) — condition eval + re-eval trigger:**
11. ⚠ `0x505360 HouseClass_PrerequisitesForTechnoTypeAreListed` (Antares) — the
    canonical per-house prerequisite check; the unlock evaluator (or call
    PrerequisiteExt's `Requirement`). Antares also has a whole
    `Hooks.Prerequisites.cpp` subsystem tracking prereq counts.
12. ⚠ `0x4F92FB HouseClass_UpdateTechTree` (Phobos) + `0x44840B
    BuildingClass_ChangeOwnership_Tech` (Antares) — fire when a house's tech state
    changes → re-evaluate `SelfTraits.Requirement` for that house's units.

> NB: cite **Antares**, not Ares, for the shared-address rows (per project policy).

---

## 7. Coexistence with Phobos / Ares
- Runs as its own Syringe DLL (same pattern as TechnoAttachmentExt, GiftBoxHost).
- **`$Inherits` ownership: DEFER to Phobos in companion mode** (§6). Only
  reimplement in standalone mode.
- Random-injection hook must be ordered before `0x474230` so Phobos sees the
  chosen parent.
- Watch the contested `0x668F6A` — avoid sharing it with our own Country_Extension
  / AggressiveStance fork; prefer `0x679CAF` for the late seam.

---

## 8. Open decisions
1. **Trait namespace**: reserved prefix (`T_`) vs an explicit `[TraitTypes]`
   registry list vs both. *(Leaning: `[TraitTypes]` registry — explicit, avoids
   accidental collisions with real unit IDs, mirrors vanilla list sections.)*
2. **Terminology**: "Trait" (current pick) vs "Package"/"Mixin".

**Resolved:** `$Inherits` ownership → defer to Phobos (§6). Runtime scope →
per-instance semantic + auto per-type materialization (§5). Contested `0x668F6A`
→ verified benign, not used by TraitExt anyway (§6/§7).

---

## 9. Phasing
1. **P1 — Static core.** Flatten pass, `$Inherits`/Trait parity, merge engine
   with `Override`/`Keep`/`Add`/`Multiply`/`Min`/`Max`/`Average`. Cosmetic/Logical
   classification table. This is the foundation everything else reuses.
2. **P2 — Random.** Pool + `min,max` + scope. Per-type (materialized) first, then
   per-instance (Ext index + point-of-use resolvers), cosmetic-unsynced vs
   logical-synced RNG.
3. **P3 — Conditional/runtime.** Building-driven apply/remove, filters,
   stacking, effective-stat cache + invalidation, exact reversibility across
   capture/deploy/sell.

Difficulty: P1 ≈ the couple-of-weeks estimate (algorithm is small; the INI seam
and edge cases are the cost). P2 and P3 are each comparable to or larger than P1,
and are where the real sync/perf engineering lives.
