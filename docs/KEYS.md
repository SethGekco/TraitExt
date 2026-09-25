# TraitExt key reference

Everything below is verified in-game unless marked otherwise.

## Declaring traits

```ini
[TraitTypes]        ; the registry. Declaration order IS fold order.
0=T_Beefy
1=T_Cheap
```

A trait is a section listed there. It holds keys to apply, plus the config keys
below (which are consumed by TraitExt and never written to targets).

| Key | Meaning |
|---|---|
| `Merge=` | fold mode for this trait's keys: `Override` (default), `Keep`, `Addition`, `Multiply`, `Min`, `Max`, `Average`, `Append`, `Remove` |
| `<Key>.Merge=` | fold mode for one key only |
| `Traits=` | compose other traits into this one (cycle-checked) |
| `AppliesTo=` | targets this trait applies to. **Also registers that section as a target**, which is how weapons/warheads work |
| `RandomPoolFor=` | targets whose random pool this trait belongs to |
| `RandomScope=` | `Type` (default) or `Instance` for those pools |
| `RerollInterval=` | morph cadence in frames, `N` or `min,max` |
| `InheritFrom=` | copy a whole TechnoType's tags into this trait (see below) |
| `InheritOnly=` | restrict the copy to these keys **or families** |
| `InheritExcept=` | drop these keys **or families** from the copy (adds to the global `[TraitExt] InheritExcept=`) |
| `InheritCoherence=` | `no` disables family coherence for this trait (see below) |
| `TurretFrom=` | take turret + barrel art from another unit (mixed turrets) |
| `ForceBodyFacing=` | unit must rotate its hull to fire, like a turretless tank |
| `ForceWeapon=` | `yes` = this variant's gun beats the slot the engine asks for (see below) |
| `Requirement=` | the unit's OWNER must have all of these present |
| `NearTypes=` | applies only while one of these stands within `NearRange` |
| `NearRange=` | proximity radius in **cells** (required by `NearTypes`) |
| `NearOwner=` | whose objects count: `Owner` (default), `Ally`, `Enemy`, `Any` |
| `RequirePower=` | `yes` = only while the owner's base has FULL power; `no` = only while it is in LOW power |
| `RequireNot=` | the owner must have **none** of these — the "while you lack X" case |
| `RequireHealthBelow=` | only while the unit is at or below this **percent** health |
| `RequireVeterancy=` | only at this rank or above: `rookie`, `veteran`, `elite` |
| `NearCount=` | how many must be in range (default 1) |
| `NearNot=` | `yes` inverts the proximity gate: applies while **nothing** is near |
| `RequireAmmoBelow=` | only while the unit has this much ammo or less |
| `BlockedFor=` | targets this trait REFUSES — the mirror of a target's `BlockTraits=` |

All gates combine — every gate present must hold. Any one alone is fine too.

```ini
[T_LastStand]               ; desperate form: hurt, and no Battle Lab left
AppliesTo=MTNK
RequireHealthBelow=35
RequireNot=GATECH
Weapon1=Comet

[T_Alone]                   ; no friendly tank within 8 cells
AppliesTo=MTNK
NearTypes=MTNK
NearRange=8
NearNot=yes

[T_Swarm]                   ; only once four of them gather
AppliesTo=MTNK
NearTypes=MTNK
NearRange=5
NearCount=4
```

⚠ A trait must not gate on `RequireVeterancy` **and** set `Veterancy` — it would
re-trigger its own condition. TraitExt warns at load.

`RequirePower=no` is the "powered unit" case: the unit converts when the base
browns out, and converts back when power is restored. Power is a HOUSE property,
so every unit of that owner flips together — which is what makes it read as the
base losing power rather than the unit.

```ini
[T_Brownout]                ; Prism Tank drops to a plain cannon on low power
AppliesTo=SREF
RequirePower=no
InheritFrom=MTNK
```

```ini
[T_Escort]                  ; veteran only while it stays near a Battle Fortress
AppliesTo=FV
NearTypes=BFRT
NearRange=6
Veterancy=2.0
```

### Applying a whole unit — `InheritFrom=`

Name a donor type instead of hand-writing its keys:

```ini
[T_BecomeApoc]
AppliesTo=BFRT
InheritFrom=HTNK        ; Battle Fortress becomes an Apocalypse
```

It copies **everything** the donor section defines — including `UIName`, `Cost`,
`Prerequisite` and `Owner` — plus its look (the art name is synthesised, since
`[SREF]` does not contain `Image=SREF`).

Taking everything is deliberate: the donor is data, so the way to not inherit a
key is to leave it out of the donor. Point `InheritFrom=` at a **dummy type**
that lists only what you want and nothing is filtered in code behind your back.
When the donor is a real unit and some keys are unwanted, exclude them:

```ini
[TraitExt]
InheritExcept=Identity,Economy   ; global default for every InheritFrom
```

```ini
[T_BecomeApoc]
AppliesTo=BFRT
InheritFrom=HTNK
InheritExcept=Cost,UIName        ; this trait only; adds to the global list
```

One behaviour is not a plain copy, because no INI can express it:

* **Tag families move together, and an unused family gets switched off.** If the
  donor has no turret, the target is written `Turret=no` rather than keeping its
  own `Turret=yes` and hunting a voxel that doesn't exist. If the donor has no
  `WeaponCount`, the target's `Weapon1..N` list is disabled so the inherited
  `Primary=` is actually read.

This is on by default because both failures are invisible — the unit just looks
or behaves wrong with nothing in the log. Turn it off for a literal copy:

```ini
[TraitExt]
InheritFamilyCoherence=no   ; global

[T_Literal]
InheritCoherence=no         ; this trait only
```

With it off nothing is switched off on your behalf, and the log says so.

Families for `InheritOnly=` / `InheritExcept=`: `Art`, `Turret`, `Weapons`,
`Armor`, `Movement`, `Economy`, `Identity`.

```ini
[T_PrismGun]
AppliesTo=MGTK
InheritFrom=SREF
InheritOnly=Weapons,Turret   ; the gun and turret only; keep its own body
```

Your own keys in the trait always beat the inherited ones, and the whole result
then flows through the normal merge modes, random pools and gates.
### `ForceWeapon=` — beating the passenger

A `Gunner=yes` unit (the IFV) picks its weapon slot by **passenger**. A variant
that declares `Weapon1=Comet` therefore fires the prism beam only while empty —
board a GI and the engine asks for the GI's slot, which the clone merely
*inherited*, so the passenger's gun wins.

`ForceWeapon=yes` answers from the variant's own **declared** slot instead, so
the variant's gun holds regardless of who is aboard:

```ini
[T_IGun1]
RandomPoolFor=FV
RandomScope=Instance
Weapon1=Comet
ForceWeapon=yes          ; prism beam even with passengers
```

Default is `no` (vanilla gunner behaviour untouched); `[TraitExt] ForceWeapon=yes`
flips the default for every variant. "Declared" means a slot the trait spelled
out — slots the clone inherited from its base are not the variant's own gun.

Value sigils: `Strength=+200` forces `Add`, `Cost=*0.75` forces `Multiply`.

## Declaring on the target instead

| Key | Meaning |
|---|---|
| `Traits=` | traits to apply |
| `BlockTraits=` | refuse specific traits |
| `TraitsRandomPool=` | pool members (alias: `TraitsRandom`) |
| `TraitsRandomCount=` | how many to draw — a `min,max` RANGE, not a list |
| `TraitsRandomScope=` | `Type` or `Instance` |
| `TraitsRandomInterval=` | morph cadence, `N` or `min,max` |
| `SpyTraits=` | forced onto this building when a spy infiltrates it |

Target-side keys are convenient but live in unit sections, which other tools
rewrite. Trait-side (`AppliesTo=` / `RandomPoolFor=`) keeps everything in one
block and survives that.

## Global config

```ini
[TraitExt]
Disable=no              ; yes = TraitExt does nothing at all this run
RandomSeed=0            ; 0 = per-match. Non-zero pins a draw for repeatable tests
KeepOriginalCameo=yes   ; keep the unit's own cameo when Image changes
VariantArt=yes          ; kill switch for per-unit looks
MixedTurrets=yes        ; kill switch for TurretFrom
InheritExcept=          ; keys/families every InheritFrom skips by default
InheritFamilyCoherence=yes ; switch off families the donor doesn't use
ForceWeapon=no          ; default for variants: does their gun beat the asked-for slot
TargetLists=            ; extra list sections to scan

[TraitTargets]          ; individual sections to treat as targets
0=SomeWeapon
```

## Scope: what each can vary

The line is **what the engine reads from the type while the variant is swapped
in for the draw** versus everything else.

| | Type scope | Instance scope |
|---|---|---|
| When | once at load, whole type shares it | per unit, at its first tick |
| Cost / UIName / Prerequisite | ✅ | ❌ charged against the shared buildable |
| Armor / Speed / ROT / max Strength | ✅ | ❌ read outside the draw |
| Weapons (`Primary=`, `Weapon1=`, …) | ✅ one draw shared by the whole type | ✅ per unit, via `TechnoClass::GetWeapon` |
| Body art, turret art, barrels | ✅ | ✅ |
| Health, Veterancy, Ammo | ✅ | ✅ genuine per-instance fields |
| Mid-match morph | — | ✅ appearance only |

Both scopes can run on the same unit: Type for the price and stats that vary per
match, Instance for per-unit looks.

**Type scope draws ONCE per match.** A `RandomScope=Type` weapon pool gives every
unit of that type the same weapon for the whole game (it varies between matches,
not within one). For a mixed group, use `RandomScope=Instance` — each unit draws
its own. Don't point both a Type and an Instance pool at the same unit; they
fight over it.

## Load-time self-checks

TraitExt reports the two mistakes that look like a broken feature:

* **`WARN trait 'X' was NEVER APPLIED to any target`** — declared but not wired
  to anything. Check this line first when a test appears to do nothing; gated
  traits are exempt, since they apply per unit at runtime.
* **`WARN trait 'X': key 'Y' looks like a TraitExt config key but is not one`** —
  a misspelled config key (`RequirePowre=`) is otherwise invisible: it gets
  written to targets as data and the feature silently never happens.

## Things that will bite

* **`Image=` names an ART SECTION, not a unit.** Many units redirect their own
  art (`MGTK`→`RTNK`, `APOC`→`MTNK`), so their ID is not a valid art section.
  TraitExt follows the redirect for you, but a hand-written `Image=` will not.
* **Art must suit the unit class.** The IFV picks turret art per passenger
  (`WeaponNFLH`); giving it art that only defines `PrimaryFireFLH` renders
  nothing. Swaps within a class are safe.
* **Circular `Image=`** (A→B and B→A) renders nothing; TraitExt warns.
* **Numeric folds need an INI-visible base.** Engine defaults are in code, not
  INI, so `Add`/`Multiply` on a key absent from rules logs a warning and applies
  as `Override`.
* **`Multiply` compounds against the base, not against your intent.** `Damage=2.0`
  on a 300-damage weapon gives 600; `Damage=200.0` gives 60,000. Easy to crank a
  test value and forget what the comment said.
* **Variant art (per-unit looks) is VEHICLE-ONLY.** Clones resolve through
  `UnitTypeClass` and the draw swap hooks `UnitClass`, so infantry, aircraft and
  buildings cannot change appearance — their other trait keys still apply, and
  TraitExt warns at load.
* **`Primary=` is ignored by units that define `WeaponCount`.** Those answer
  from their `Weapon1..N` list instead — and for `Gunner=yes` units (the IFV is
  `WeaponCount=17`) the slot is chosen by **passenger**, so `Primary=` is never
  read at all. The write succeeds and the log shows the change, which makes it
  look like it worked; the unit keeps firing its old gun. Set `Weapon1=` (the
  default/empty slot) instead. TraitExt now warns at load when a trait sets
  `Primary=`/`Secondary=` on such a type. The Prism Tank is the same shape:
  `WeaponCount=1` with `Weapon1=Comet` and `Primary=` commented out.
* **Gated STATS do not roll back when the gate closes.** Appearance and weapon
  follow the gate both ways, but `Veterancy`/`Health`/`Ammo` are applied when it
  opens and left alone when it shuts — un-applying a fold is not generally
  possible. So a unit that drives out of a `NearTypes=` radius keeps the
  veterancy it gained. Use proximity gates for looks and weapons; treat any stat
  bonus from one as permanent.
* **Cameos come from `artmd.ini`**, which TraitExt does not process, so a cameo
  cannot be set directly — only inherited via `Image=` (or kept, the default).
