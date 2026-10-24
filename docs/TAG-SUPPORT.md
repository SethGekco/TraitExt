# Which TechnoType tags does trait inheritance support?

Checked against the live `rulesmd.ini` (**629 distinct TechnoType keys across
582 types**) and against the classification the code actually applies -
`IsWeaponKey`, the draw-key list and the instance-field set - not from a
remembered tag list.

## There is no single column, because there are two paths

A trait reaches a unit in one of two ways, and they have completely different
tag coverage. Marking each tag once would be wrong for half the cases.

### Path 1 - LOAD-TIME traits: every tag works (all 629)

`AppliesTo=` / `Traits=` / `AppliesToList=` / `InheritFrom=` / `RandomScope=Type`.

These write into the unit's own section **before the engine parses it**, so the
engine reads the value exactly as if you had typed it by hand. There is nothing
to support per-tag:

> **If you can write it in `rulesmd.ini`, a load-time trait can set it.**

`Cost`, `Prerequisite`, `Owner`, `TechLevel`, `Armor`, `Strength`, `Speed`,
`Locomotor`, `Landable`, `Passengers`, `Voice*`, `Sight`, `Explodes` - all of it,
including keys added by Antares and Phobos, because we never needed to know what
a key means to copy it.

The only limits on this path are deliberate:

* `InheritExcept=` / `Exclude=` drop keys you name (or a whole family:
  `Art Turret Weapons Armor Movement Identity Economy`).
* The `Identity` family (`UIName`, `Name`, `DeploysInto`, `UndeploysInto`,
  `Category`, `AIBasePlanningSide`) is **included** by default, per your call -
  exclude it in a type block when you do not want it.
* Family coherence may switch OFF a family the donor does not use, so the
  target's leftover setting cannot contradict what was just copied.

### Path 2 - GATED / PER-UNIT traits: only 70 tags work

`Requirement=` / `NearTypes=` / `RequirePower=` / `RequireHealthBelow=` /
`RequireMission=` / `RequireCountry=` / any other gate, plus
`RandomScope=Instance` and the morph re-roll.

These change **one unit while the match is running**, and a `TechnoTypeClass` is
shared by every unit of that type in every house. So they only work where the
engine asks the **instance** instead of the type. That is three places:

| path | why it can be per-unit |
|---|---|
| the **draw** | `Type` is swapped to the variant around `UnitClass::DrawAsVXL` / `DrawAsSHP` / `InfantryClass::DrawIt`, then restored |
| **`TechnoClass::GetWeapon`** | takes the instance, so the weapon can be answered per unit |
| genuine instance fields | HP, veterancy and ammo live on the object, not on the type |

Everything outside those paths is read from the shared type, so a gate
**cannot** change `Cost` (charged before the unit exists), `Armor`, `Speed`,
`Prerequisite`, `Owner` or `Landable` for a single unit. TraitExt logs a `WARN`
naming the key when a gated trait sets one.

## The 70 tags a GATE can change

```
  weapon selectors            via TechnoClass::GetWeapon
    Primary                    ✓
    Secondary                  ✓
    ElitePrimary               ✓
    EliteSecondary             ✓
    WeaponCount                ✓
    Gunner                     ✓
    IsGattling                 ✓

  weapon slots                via TechnoClass::GetWeapon (all 18 engine slots)
    Weapon1 ... Weapon18       ✓
    EliteWeapon1 ... EliteWeapon18 ✓

  art                         via the draw-time Type swap
    Image                      ✓
    Voxel                      ✓
    Remapable                  ✓
    AlphaImage                 ✓
    Palette                    ✓

  turret family               via the draw-time Type swap
    Turret                     ✓
    TurretCount                ✓
    TurretOffset               ✓
    UseTurretShadow            ✓
    Turret.RangeBands          ✓
    Turret.RangeIndices        ✓
    TurretNotExportedOnGround  ✓
    TurretAnim                 ✓
    TurretAnimIsVoxel          ✓
    TurretAnimX                ✓
    TurretAnimY                ✓
    TurretAnimZAdjust          ✓
    WeaponTurretIndex1         ✓
    WeaponTurretIndex2         ✓
    WeaponTurretIndex3         ✓
    WeaponTurretIndex4         ✓

  cameo                       special-cased, applied to the loaded type
    Cameo                      ✓
    AltCameo                   ✓

  real per-instance fields    set on the object, not on the type
    Strength                   ✓   current HP on that unit (alias: Health)
    Health                     ✓   same thing, friendlier name
    Veterancy                  ✓   TraitExt payload key, not a vanilla tag
    Ammo                       ✓   current ammo (the TYPE's Ammo= is load-time)
```

### The same thing as one table, for the keys people actually reach for

| tag | load-time trait | gated / per-unit trait |
|---|---|---|
| `Image / Voxel / Palette` | ✓ | ✓ |
| `Turret / TurretCount / ...` | ✓ | ✓ |
| `Primary / Secondary` | ✓ | ✓ |
| `Weapon1 .. Weapon18` | ✓ | ✓ |
| `Elite* equivalents` | ✓ | ✓ |
| `Cameo / AltCameo` | ✓ | ✓ |
| `Strength (max HP / current)` | ✓ | ✓ |
| `Veterancy` | ✓ | ✓ |
| `Ammo` | ✓ | ✓ |
| `Armor` | ✓ | ✗ |
| `Cost / Soylent / Points` | ✓ | ✗ |
| `Prerequisite / Owner / TechLevel` | ✓ | ✗ |
| `Speed / Locomotor / SpeedType` | ✓ | ✗ |
| `Landable / JumpJet / FlightLevel` | ✓ | ✗ |
| `Passengers / SizeLimit` | ✓ | ✗ |
| `Sight / GuardRange` | ✓ | ✗ |
| `Voice* / VoiceSelect` | ✓ | ✗ |
| `UIName / Name` | ✓ | ✗ |
| `DeploysInto / UndeploysInto` | ✓ | ✗ |
| `Explodes / SelfHealing` | ✓ | ✗ |
| `ROT / TurnRate / Crusher` | ✓ | ✗ |
| `anything in artmd.ini` | ✗ | ✗ |

The right-hand column is short for a reason: it is not a list of features we
have not written yet. A gate changes one unit, and every key in the ✗ rows is
read from the type that all units share. Making those per-unit is a different
engine problem, not a bigger key list.
### Caveats that matter in practice

* **Art is VEHICLE + INFANTRY only.** Aircraft and buildings have no draw path
  that reads `Type`, so their look cannot change per unit. Weapons still can.
* **`Primary`/`Secondary` and `WeaponN` are mutually exclusive selectors.** A
  type reads its `WeaponN` list when that list is engaged (`WeaponCount>0`,
  `Gunner=yes`, `TurretCount>1`) and `Primary`/`Secondary` otherwise - never
  both, so one silently overwrites the other. Use the form the target natively
  uses; TraitExt warns rather than rewriting, because picking the wrong one is
  an authoring mistake, not something for us to guess at.
* **A type with no `Primary` has no targeting at all** (e.g. `LCRF`). Giving it
  a weapon through a gate does not make it able to acquire a target.
* **Rollback is uneven.** `Veterancy` is restored when the gate closes;
  `Health`/`Strength` and `Ammo` are not, because gameplay moves them in
  between and restoring would undo real damage.
* **`Strength` means two things.** At load it sets the type's maximum HP; as a
  gate payload it sets current HP on that one unit.
* **`Image=` on a rules section names an ART section**, resolved exactly one
  hop. `[APOC] Image=MTNK` wears art `MTNK`; it does not become the Grizzly,
  whose rules id also happens to be `MTNK`.

## Not reachable on either path

Keys that are not in `rulesmd.ini` at all. TraitExt reads the rules INI, so
**`artmd.ini` keys are outside it** - art geometry, `FLH` offsets, voxel/SHP
parameters, `Sequence` definitions. `Cameo=`/`AltCameo=` are the two exceptions,
special-cased because a trait that changes what a unit is should change its
icon.

## Complete load-time-only list (575 keys)

Every one of these is supported by a load-time trait and **not** by a gate:

`#BuildTimeMultiplier` `#Inherits` `:SecretBuilding` `AIBasePlanningSide`
`AIBuildThis` `Academy.Cap.Infantry` `Academy.InfantryVeterancy`
`Academy.Stacks` `Academy.VehicleVeterancy` `Accelerates` `AccelerationFactor`
`ActivateSound` `Adjacent` `AdjacentType` `Agent` `AggressiveStance.Always`
`AirRangeBonus` `AircraftCostBonus` `AirportBound` `AirstrikeRechargeTime`
`AirstrikeTeam` `AirstrikeTeamType` `AllowedToStartInMultiplayer`
`AlternateArcticArt` `AlwaysAvailable` `AmbientSound` `AntiAirValue`
`AntiArmorValue` `AntiInfantryValue` `Armor` `Assaulter` `Attachment0.FLH`
`Attachment0.IsOnTurret` `Attachment0.Powered` `Attachment0.PoweredBy`
`Attachment0.Prerequisite` `Attachment0.Prerequisite.Dynamic`
`Attachment0.RequiredHouses` `Attachment0.RotationAdjust`
`Attachment0.TechnoType` `Attachment0.Type` `Attachment1.FLH`
`Attachment1.IsOnTurret` `Attachment1.Powered` `Attachment1.PoweredBy`
`Attachment1.Prerequisite` `Attachment1.Prerequisite.Dynamic`
`Attachment1.RequiredHouses` `Attachment1.RotationAdjust`
`Attachment1.TechnoType` `Attachment1.Type` `AttachmentTopLayerMinHeight`
`AttackCursorOnFriendlies` `AttackFriendlies` `AutoCrush`
`AutoDeath.AfterDelay` `AutoDeath.Behavior` `AuxSound1` `AuxSound2`
`BalloonHover` `BarrelCompressFrames` `BarrelHoldFrames` `BarrelRecoverFrames`
`BarrelTravel` `BaseNormal` `BerserkFriendly` `Bib` `BombSight` `Bombable`
`Bounty.Leech` `Bounty.Leech.KillerHouses` `Bounty.Leech.Killers`
`Bounty.Leech.Range` `Bounty.Leech.Ratio` `Bounty.Leech.VictimHouses`
`Bounty.Leech.Victims` `Bounty.Value` `BrainBlastTurretIndex`
`BrainBlastTurretWeapon` `BridgeRepairHut` `BuildCat` `BuildLimit`
`BuildLimit.Scope` `BuildLimit.Scope.Max` `BuildLimit.Scope.RefundOnSell`
`BuildOffAnyBuilding` `BuildSpeed.Containers` `BuildSpeed.Stack.Max`
`BuildTimeMultiplier` `BuildingsCostBonus` `Bunker`
`Bunker.DeployCaptureRange` `Bunker.DeployToEnter` `BunkerDamageMultiplier`
`BunkerROFMultMultiplier` `Bunkerable` `BunkerableAnyway` `C4`
`CanApproachTarget` `CanBeOccupied` `CanC4` `CanDisguise` `CanOccupyFire`
`CanPassiveAquire` `CanRetaliate` `Capturable` `CaptureEvaEvent`
`CarriesCrate` `Carryall` `Category` `ChargedAnimTime` `ChronoInSound`
`ChronoOutSound` `ChronoTurretIndex` `ChronoTurretWeapon` `Civilian`
`ClickRepairable` `Cloakable` `CloakingSpeed` `Clone.Escalate.Global.Count`
`Clone.Escalate.Global.Index` `Clone.Escalate[0]` `Clone.Escalate[1]`
`Clone.Escalate[2]` `Cloning` `Cloning.ConsideredBuilt`
`Cloning.ConsideredBuilt.Weight` `Cloning.Mult` `Cloning.Mult.Blacklist`
`Cloning.Mult.Slots` `CloseRange` `ConcentricRadialIndicator`
`ConsideredAircraft` `ConstructionYard` `Cost` `CowTurretIndex`
`CowTurretWeapon` `Crashable` `CrashingSound` `CrateBeneath`
`CrateBeneathIsMoney` `CrateGoodie` `CreateSound` `CreateUnitSound` `Crewed`
`CrushSound` `Crushable` `Crusher` `DamageParticleSystems` `DamageSelf`
`DamageSmokeOffset` `DamageSound` `DeaccelerationFactor` `DeactivateSound`
`DeathWeapon` `DeathWeaponDamageModifier` `DebrisAnim` `DebrisAnims`
`DebrisMaximums` `DebrisTypes` `DefaultToGuardArea` `DefensesCostBonus`
`DeployFacing` `DeployFire` `DeploySound` `DeployTime` `DeployToLand`
`DeployedCrushable` `Deployer` `DeployingAnim` `DeploysInto` `DestroyAnim`
`DetectDisguise` `DetectDisguiseRange` `DieSound` `DisguiseWhenStill`
`DistributedFire` `Dock` `DockUnload` `DontScore` `Drainable`
`EligibileForAllyBuilding` `EligibleForDelayKill` `EliteAbilities`
`EliteAirstrikeRechargeTime` `EliteAirstrikeTeam` `EliteAirstrikeTeamType`
`EliteOccupyWeapon` `EliteStage1` `EliteStage2` `EliteStage3`
`EmptyReloadAmount` `Engineer` `Enslaves` `EnterTransportSound`
`EnterWaterSound` `ExitCoord` `ExplodeTurretIndex` `ExplodeTurretWeapon`
`Explodes` `Explosion` `ExtraPower` `Factory` `Factory.ExtraQueues`
`FactoryPlant` `Fearless` `Fighter` `FireAngle` `FlakTurretIndex`
`FlakTurretWeapon` `FlightLevel` `FlyBack` `FlyBy` `ForbiddenHouses`
`Fraidycat` `FreeUnit` `GDIBarracks` `Gap.Pattern`
`Gap.Pattern.Plume.Direction` `Gap.Pattern.Plume.Length`
`Gap.Pattern.Plume.Spacing` `Gap.Pattern.Plume.Speed`
`Gap.Pattern.Plume.Spread` `Gap.Pattern.Shift` `Gap.Temporary` `GapGenerator`
`GapRadiusInCells` `Gate` `GateCloseDelay` `Grinding` `GuardRange`
`GuardianTurretIndex` `GuardianTurretWeapon` `HalfDamageSmokeLocation1`
`HarvestRate` `Harvester` `HasRadialIndicator` `HasStupidGuardMode`
`HasTurretTooltips` `HealthBar.Hide` `Height` `Helipad` `Host.Delay`
`Host.Nums` `Host.OnlyBuilt` `Host.TriggeredTimes` `Host.Types` `HoverAttack`
`IFVMode` `IdleRate` `Immune` `ImmuneToPoison` `ImmuneToPsionicWeapons`
`ImmuneToPsionics` `ImmuneToRadiation` `ImmuneToVeins` `ImpactLandSound`
`InfantryAbsorb` `InfantryCostBonus` `InfantryGainSelfHeal` `Infiltrate`
`Infiltrate.Effects.Source` `InitialAmmo` `InitialPayload.Nums`
`InitialPayload.Types` `InitiateTurretIndex` `InitiateTurretWeapon`
`Insignificant` `Intel.Shrouds` `InvisibleInGame` `IsBase` `IsBaseDefense`
`IsChargeTurret` `IsImmuneToRadiation` `IsSelectableCombatant`
`IsSimpleDeployer` `IsTilter` `Ivan` `JumpJet` `JumpJetAccel` `JumpJetTurn`
`JumpJetTurnRate` `JumpjetClimb` `JumpjetCrash` `JumpjetDeviation`
`JumpjetHeight` `JumpjetNoWobbles` `JumpjetSpeed` `JumpjetWobbles` `KeepAlive`
`LandTargeting` `Landable` `LeadershipRating` `LeaveRubble`
`LeaveTransportSound` `LeaveWaterSound` `LegalTarget` `LightBlueTint`
`LightGreenTint` `LightIntensity` `LightRedTint` `LightVisibility` `Locomotor`
`MachineGunTurretIndex` `MachineGunTurretWeapon` `MaxDebris`
`MaxNumberOccupants` `Maxdebris` `MinDebris` `Mirage.Count` `Mirage.Decoys`
`Mirage.DefaultDisguises` `Mirage.Disguise` `Mirage.Distance`
`Mirage.FadeAudience` `Mirage.FadeOpacity` `Mirage.FadePulseRate`
`Mirage.FadeStyle` `Mirage.Health` `MissileSpawn` `MoveSound` `MoveStart`
`MoveToShroud` `MovementRestrictedTo` `MovementZone` `NODBarracks` `Name`
`Natural` `NaturalSmokeLocation` `Naval` `NavalTargeting` `NeedsEngineer`
`NoAmmoAmount` `NoAmmoWeapon` `NoManualEnter` `NoManualUnload` `NoShadow`
`NoSpawnAlt` `Nominal` `NormalTurretIndex` `NormalTurretWeapon` `NotHuman`
`NotWorkingSound` `NukeSilo` `NumberImpassableRows` `NumberOfDocks` `Occupier`
`Occupier.OpenTopped.Deny` `Occupier.OpenTopped.Force` `OccupyPip`
`OccupyWeapon` `OmniCrushResistant` `OmniCrusher` `OmniFire` `OpenTopped`
`OpenTopped.Profile` `OpenTopped.Profile.Elite` `OpenTopped.VeterancySource`
`OpenTransportWeapon` `OpportunityFire` `OrePurifier` `Organic`
`Overpowerable` `Owner` `Parasiteable` `Passengers` `PermaDisguise`
`PhysicalSize` `Pip` `PipScale` `PipsDrawForAll` `PistolTurretIndex`
`PistolTurretWeapon` `PitchAngle` `PitchSpeed` `PixelSelectionBracketDelta`
`PlaceAnywhere` `Points` `Power` `Powered` `PoweredBy` `PoweredSpecial`
`PoweredUnit` `PowersUnit` `Prerequisite` `Prerequisite.Containers`
`Prerequisite.StolenTechs` `PrerequisiteOverride` `PreventAttackMove`
`ProduceCashAmount` `ProduceCashDelay` `ProduceCashStartup`
`PsychicDetectionRadius` `ROT` `RadCannonTurretIndex` `RadCannonTurretWeapon`
`Radar` `RadarInvisible` `RadarVisible` `RadialFireSegments` `RateDown`
`RateUp` `Rebate.Containers` `Refinery` `RefinerySmokeFrames`
`RefinerySmokeOffsetFour` `RefinerySmokeOffsetOne` `RefinerySmokeOffsetThree`
`RefinerySmokeOffsetTwo` `RefinerySmokeParticleSystem` `RejoinTeamIfLimboed`
`Reload` `ReloadAmount` `RepairTurretIndex` `RepairTurretWeapon` `Repairable`
`Repel.Enemy.Range` `Repel.Enemy.Type` `RequiredHouses`
`RequiresStolenAlliedTech` `RequiresStolenSovietTech`
`RequiresStolenThirdTech` `ReselectIfLimboed` `ResourceDestination`
`ResourceGatherer` `RevealToAll` `SecretHouses` `SecretLab` `Selectable`
`SelfHealing` `SensorArray` `Sensors` `SensorsSight` `Sequence`
`ShockTurretIndex` `ShockTurretWeapon` `Sight` `SinkingSound` `Size`
`SizeLimit` `SlaveRegenRate` `SlaveReloadRate` `Slaved` `SlavesNumber`
`SniperTurretIndex` `SniperTurretWeapon` `Soylent` `SpawnRegenRate`
`SpawnReloadRate` `Spawned` `Spawner.AutoDeploy` `Spawner.AutoDeploy.Delay`
`Spawner.AutoDeploy.MinReady` `Spawns` `SpawnsNumber` `SpecialThreatValue`
`Speed` `SpeedType` `SpyEffect.GapVision` `SpyEffect.GapVision.Duration`
`SpyEffect.StolenMoney.Max` `SpyEffect.StolenMoney.Min`
`SpyEffect.StolenTechIndex` `SpySat` `Spyable` `Squad.EntrySelect`
`Squad.LoopLimit` `Squad.MaxActive` `Squad0.AnchorDeath.Behavior`
`Squad0.AnchorDeath.Heir` `Squad0.MaxActive` `Squad0.MemberSelection`
`Squad0.Members` `Squad0.Numbers` `Squad0.OrderEcho` `Squad0.Prerequisite`
`Squad0.Prerequisite.1` `Squad0.RegenRate` `Squad0.Weight`
`Squad1.AnchorDeath.Behavior` `Squad1.MaxActive` `Squad1.MemberSelection`
`Squad1.Members` `Squad1.Numbers` `Squad1.SpawnEvent`
`Squad1.SpawnEvent.Interval` `Squad1.Weight` `Squad2.Members` `Squad2.Numbers`
`Squad2.SpawnEvent` `Squad2.Stance` `Stage1` `Stage2` `Stage3` `Storage`
`StupidHunt` `SuperGapRadiusInCells` `SuperWeapon` `SuperWeapons`
`SuppressionThreshold` `Survivor.ElitePassengerChance`
`Survivor.RookiePassengerChance` `Survivor.VeteranPassengerChance`
`TargetCoordOffset` `TargetLaser` `TechLevel` `Teleporter`
`TerroristExplodeTurretIndex` `TerroristExplodeTurretWeapon`
`ThreatAvoidanceCoefficient` `ThreatPosed` `TiberiumProof` `TiltCrashJumpjet`
`ToProtect` `ToTile` `TogglePower` `TooBigToFitUnderBridge` `Trainable`
`TraitsRandomCount` `TurretRecoil` `TurretRotateSound` `TurretSpins`
`TurretTravel` `TypeImmune` `UIName` `UndeployDelay` `UndeploySound`
`UndeploysInto` `Underwater` `UnitAbsorb` `UnitLost.Suppress` `UnitReload`
`UnitRepair` `UnitsCostBonus` `UnitsGainSelfHeal` `UnloadingClass` `Unnatural`
`Unsellable` `Upgrades` `UseOwnName` `VHPScan` `VeteranAbilities`
`VirusTurretIndex` `VirusTurretWeapon` `VoiceAttack` `VoiceCapture`
`VoiceCrashing` `VoiceDeploy` `VoiceEnter` `VoiceFeedback` `VoiceHarvest`
`VoiceMove` `VoiceSecondaryWeaponAttack` `VoiceSelect`
`VoiceSelectDeactivated` `VoiceSelectEnslaved` `VoiceSpecialAttack` `WalkRate`
`Wall` `WaterBound` `Weapon.RangeUsesCellSpread` `WeaponStages`
`WeaponsFactory` `WeatherSystem.Amounts` `WeatherSystem.Rate`
`WeatherSystem.RequiresPower` `WeatherSystem.Types` `Weight` `WorkingSound`
`YuriBarracks` `YuriPrimeTurretIndex` `YuriPrimeTurretWeapon` `ZFudgeBridge`
`ZFudgeColumn` `ZFudgeTunnel`

## Runtime-capable keys your current ruleset actually uses (54 of 70)

`AlphaImage` `Ammo` `ElitePrimary` `EliteSecondary` `EliteWeapon1`
`EliteWeapon10` `EliteWeapon11` `EliteWeapon12` `EliteWeapon13`
`EliteWeapon14` `EliteWeapon15` `EliteWeapon16` `EliteWeapon17` `EliteWeapon2`
`EliteWeapon3` `EliteWeapon4` `EliteWeapon5` `EliteWeapon6` `EliteWeapon7`
`EliteWeapon8` `EliteWeapon9` `Gunner` `Image` `IsGattling` `Primary`
`Secondary` `Strength` `Turret` `Turret.RangeBands` `Turret.RangeIndices`
`TurretAnim` `TurretAnimIsVoxel` `TurretAnimX` `TurretAnimY`
`TurretAnimZAdjust` `TurretCount` `Weapon1` `Weapon10` `Weapon11` `Weapon12`
`Weapon13` `Weapon14` `Weapon15` `Weapon16` `Weapon17` `Weapon2` `Weapon3`
`Weapon4` `Weapon5` `Weapon6` `Weapon7` `Weapon8` `Weapon9` `WeaponCount`
