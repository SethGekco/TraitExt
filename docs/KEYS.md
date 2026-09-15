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
| `TurretFrom=` | take turret + barrel art from another unit (mixed turrets) |
| `ForceBodyFacing=` | unit must rotate its hull to fire, like a turretless tank |
| `Requirement=` | the unit's OWNER must have all of these present |
| `NearTypes=` | applies only while one of these stands within `NearRange` |
| `NearRange=` | proximity radius in **cells** (required by `NearTypes`) |
| `NearOwner=` | whose objects count: `Owner` (default), `Ally`, `Enemy`, `Any` |

`Requirement=` and `NearTypes=` can be combined — both must hold. Either alone
is fine too.

```ini
[T_Escort]                  ; veteran only while it stays near a Battle Fortress
AppliesTo=FV
NearTypes=BFRT
NearRange=6
Veterancy=2.0
```

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
RandomSeed=0            ; 0 = per-match. Non-zero pins a draw for repeatable tests
KeepOriginalCameo=yes   ; keep the unit's own cameo when Image changes
VariantArt=yes          ; kill switch for per-unit looks
MixedTurrets=yes        ; kill switch for TurretFrom
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
| Weapons (`Primary=`, `ROF=`, …) | ✅ | ❌ (would need the swap extended to the fire path) |
| Body art, turret art, barrels | ✅ | ✅ |
| Health, Veterancy, Ammo | ✅ | ✅ genuine per-instance fields |
| Mid-match morph | — | ✅ appearance only |

Both scopes can run on the same unit: Type for the price and stats that vary per
match, Instance for per-unit looks.

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
* **Gated STATS do not roll back when the gate closes.** Appearance and weapon
  follow the gate both ways, but `Veterancy`/`Health`/`Ammo` are applied when it
  opens and left alone when it shuts — un-applying a fold is not generally
  possible. So a unit that drives out of a `NearTypes=` radius keeps the
  veterancy it gained. Use proximity gates for looks and weapons; treat any stat
  bonus from one as permanent.
* **Cameos come from `artmd.ini`**, which TraitExt does not process, so a cameo
  cannot be set directly — only inherited via `Image=` (or kept, the default).
