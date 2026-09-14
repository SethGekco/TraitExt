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
* **Cameos come from `artmd.ini`**, which TraitExt does not process, so a cameo
  cannot be set directly — only inherited via `Image=` (or kept, the default).
