# Which TechnoType tags does trait inheritance support?

Checked against the live `rulesmd.ini` (**629 distinct TechnoType keys across
582 types**) and against `VariantType::ClassifyKey`, which is the code that
actually makes this decision at load - not a remembered tag list.

## Short version

**Everything, except 8 build-list keys.**

| | load-time trait | gated / per-unit trait |
|---|---|---|
| art, turret, weapons, HP/veterancy/ammo (54 keys) | ✓ | ✓ cheap draw swap |
| every other type tag (566 keys) | ✓ | ✓ type conversion |
| build-list keys (8) | ✓ | ✗ *not a unit question* |
| artmd.ini keys | ✗ | ✗ *different file* |

You do not pick a mechanism. `GateMode=Auto` is the default and the key
classification chooses: the cheap draw swap when it suffices, conversion when
the key needs the whole type. Write vanilla tags.

## How the two gated paths differ

A gate changes **one unit** mid-match, but a `TechnoTypeClass` is shared by
every unit of that type in every house. So a gate only works where the engine
asks the *instance*. The engine reads nearly every type field as
`pThis->Type->Field` - it dereferences the instance's pointer **at the read
site** - so the answer was never about which tags we support, but about how
long we leave that pointer repointed.

| | what it does | reaches |
|---|---|---|
| **draw swap** | repoints `Type` for the width of one `DrawIt` call, then restores | art, turret, weapons, the four real per-instance fields |
| **conversion** | leaves `Type` repointed; the unit *is* the variant until the gate closes | every type-level tag |

Conversion costs what the draw swap does not: HP is rescaled **proportionally**
to the new maximum (lossy, so round trips drift), ammo is clamped if the new
maximum is smaller, the locomotor is rebuilt when `Locomotor=` differs, and the
unit is re-registered with its house with a tech-tree recheck. Converting gates
therefore get a 15-frame `GateDwell` floor whether the author set one or not -
a unit idling on a radius boundary would otherwise flip every tick.

Not guesswork: Phobos ships the same mechanism as the warhead
`Convert.From`/`Convert.To` feature (`TechnoExt::ConvertToType`), and its
vanilla path is where this fix-up list comes from. TraitExt adds the reverse
direction, which Phobos never needed - their conversion is a one-way warhead
effect, a gate closes again.

## What a gate genuinely cannot do

**Buildings.** A building's foundation is written into map cells at Unlimbo, so
repointing its type leaves the old footprint occupied. Vehicles, infantry and
aircraft convert, and only within the same category - a `UnitClass` instance
cannot wear an `InfantryTypeClass`. Phobos refuses this too.

**Build-list keys (8).** Asked of the HOUSE while deciding what can be built,
before the unit exists, so there is no single unit for them to answer about.
This is a category error rather than a missing feature, and TraitExt now says
so by name instead of reporting a blanket "no effect":

`AIBuildThis` `BuildLimit` `ForbiddenHouses` `Owner` `Prerequisite`
`PrerequisiteOverride` `RequiredHouses` `TechLevel`

They work **completely** on a load-time trait, which is where they belong.

**artmd.ini keys.** TraitExt reads the rules INI. `Cameo=`/`AltCameo=` are the
two deliberate exceptions, because a trait that changes what a unit is should
change its icon.

## Caveats worth knowing before testing

* **Variant ART is vehicles + infantry only.** Aircraft and buildings have no
  draw path that reads `Type`. Aircraft still *convert*, so an aircraft can
  gain gated speed or `Landable=` - it just cannot change how it looks.
* **`Primary`/`Secondary` and `WeaponN` are mutually exclusive selectors.** A
  type reads its `WeaponN` list when that list is engaged (`WeaponCount>0`,
  `Gunner=yes`, `TurretCount>1`) and `Primary`/`Secondary` otherwise - never
  both, so one silently overwrites the other. TraitExt warns rather than
  rewriting: picking the wrong one is an authoring mistake, not ours to guess.
* **A type with no `Primary` has no targeting at all** (e.g. `LCRF`). A gated
  weapon does not give it the ability to acquire a target.
* **Rollback is uneven.** `Veterancy` is restored when a gate closes;
  `Health`/`Strength` and `Ammo` are not, because gameplay moves them in
  between and restoring would undo real damage.
* **`Strength` means two things.** At load it sets the type's maximum HP; as a
  gate payload it sets current HP on that one unit.
* **`Image=` on a rules section names an ART section**, resolved exactly one
  hop. `[APOC] Image=MTNK` wears art `MTNK`; it does not become the Grizzly,
  whose rules id also happens to be `MTNK`.

## Reference: the cheap-path keys (54)

Delivered by the draw swap, `GetWeapon` or a real instance field, so a trait
made only of these never pays for a conversion:

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

## Reference: keys that need conversion (566 in this ruleset)

Supported on both paths - load-time, and from a gate via conversion:

`#BuildTimeMultiplier` `#Inherits` `:SecretBuilding` `AIBasePlanningSide`
`Academy.Cap.Infantry` `Academy.InfantryVeterancy` `Academy.Stacks`
`Academy.VehicleVeterancy` `Accelerates` `AccelerationFactor` `ActivateSound`
`Adjacent` `AdjacentType` `Agent` `AggressiveStance.Always` `AirRangeBonus`
`AircraftCostBonus` `AirportBound` `AirstrikeRechargeTime` `AirstrikeTeam`
`AirstrikeTeamType` `AllowedToStartInMultiplayer` `AlternateArcticArt`
`AlwaysAvailable` `AmbientSound` `AntiAirValue` `AntiArmorValue`
`AntiInfantryValue` `Armor` `Assaulter` `Attachment0.FLH`
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
`BrainBlastTurretWeapon` `BridgeRepairHut` `BuildCat` `BuildLimit.Scope`
`BuildLimit.Scope.Max` `BuildLimit.Scope.RefundOnSell` `BuildOffAnyBuilding`
`BuildSpeed.Containers` `BuildSpeed.Stack.Max` `BuildTimeMultiplier`
`BuildingsCostBonus` `Bunker` `Bunker.DeployCaptureRange`
`Bunker.DeployToEnter` `BunkerDamageMultiplier` `BunkerROFMultMultiplier`
`Bunkerable` `BunkerableAnyway` `C4` `CanApproachTarget` `CanBeOccupied`
`CanC4` `CanDisguise` `CanOccupyFire` `CanPassiveAquire` `CanRetaliate`
`Capturable` `CaptureEvaEvent` `CarriesCrate` `Carryall` `Category`
`ChargedAnimTime` `ChronoInSound` `ChronoOutSound` `ChronoTurretIndex`
`ChronoTurretWeapon` `Civilian` `ClickRepairable` `Cloakable` `CloakingSpeed`
`Clone.Escalate.Global.Count` `Clone.Escalate.Global.Index`
`Clone.Escalate[0]` `Clone.Escalate[1]` `Clone.Escalate[2]` `Cloning`
`Cloning.ConsideredBuilt` `Cloning.ConsideredBuilt.Weight` `Cloning.Mult`
`Cloning.Mult.Blacklist` `Cloning.Mult.Slots` `CloseRange`
`ConcentricRadialIndicator` `ConsideredAircraft` `ConstructionYard` `Cost`
`CowTurretIndex` `CowTurretWeapon` `Crashable` `CrashingSound` `CrateBeneath`
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
`FlakTurretWeapon` `FlightLevel` `FlyBack` `FlyBy` `Fraidycat` `FreeUnit`
`GDIBarracks` `Gap.Pattern` `Gap.Pattern.Plume.Direction`
`Gap.Pattern.Plume.Length` `Gap.Pattern.Plume.Spacing`
`Gap.Pattern.Plume.Speed` `Gap.Pattern.Plume.Spread` `Gap.Pattern.Shift`
`Gap.Temporary` `GapGenerator` `GapRadiusInCells` `Gate` `GateCloseDelay`
`Grinding` `GuardRange` `GuardianTurretIndex` `GuardianTurretWeapon`
`HalfDamageSmokeLocation1` `HarvestRate` `Harvester` `HasRadialIndicator`
`HasStupidGuardMode` `HasTurretTooltips` `HealthBar.Hide` `Height` `Helipad`
`Host.Delay` `Host.Nums` `Host.OnlyBuilt` `Host.TriggeredTimes` `Host.Types`
`HoverAttack` `IFVMode` `IdleRate` `Immune` `ImmuneToPoison`
`ImmuneToPsionicWeapons` `ImmuneToPsionics` `ImmuneToRadiation`
`ImmuneToVeins` `ImpactLandSound` `InfantryAbsorb` `InfantryCostBonus`
`InfantryGainSelfHeal` `Infiltrate` `Infiltrate.Effects.Source` `InitialAmmo`
`InitialPayload.Nums` `InitialPayload.Types` `InitiateTurretIndex`
`InitiateTurretWeapon` `Insignificant` `Intel.Shrouds` `InvisibleInGame`
`IsBase` `IsBaseDefense` `IsChargeTurret` `IsImmuneToRadiation`
`IsSelectableCombatant` `IsSimpleDeployer` `IsTilter` `Ivan` `JumpJet`
`JumpJetAccel` `JumpJetTurn` `JumpJetTurnRate` `JumpjetClimb` `JumpjetCrash`
`JumpjetDeviation` `JumpjetHeight` `JumpjetNoWobbles` `JumpjetSpeed`
`JumpjetWobbles` `KeepAlive` `LandTargeting` `Landable` `LeadershipRating`
`LeaveRubble` `LeaveTransportSound` `LeaveWaterSound` `LegalTarget`
`LightBlueTint` `LightGreenTint` `LightIntensity` `LightRedTint`
`LightVisibility` `Locomotor` `MachineGunTurretIndex` `MachineGunTurretWeapon`
`MaxDebris` `MaxNumberOccupants` `Maxdebris` `MinDebris` `Mirage.Count`
`Mirage.Decoys` `Mirage.DefaultDisguises` `Mirage.Disguise` `Mirage.Distance`
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
`Overpowerable` `Parasiteable` `Passengers` `PermaDisguise` `PhysicalSize`
`Pip` `PipScale` `PipsDrawForAll` `PistolTurretIndex` `PistolTurretWeapon`
`PitchAngle` `PitchSpeed` `PixelSelectionBracketDelta` `PlaceAnywhere`
`Points` `Power` `Powered` `PoweredBy` `PoweredSpecial` `PoweredUnit`
`PowersUnit` `Prerequisite.Containers` `Prerequisite.StolenTechs`
`PreventAttackMove` `ProduceCashAmount` `ProduceCashDelay`
`ProduceCashStartup` `PsychicDetectionRadius` `ROT` `RadCannonTurretIndex`
`RadCannonTurretWeapon` `Radar` `RadarInvisible` `RadarVisible`
`RadialFireSegments` `RateDown` `RateUp` `Rebate.Containers` `Refinery`
`RefinerySmokeFrames` `RefinerySmokeOffsetFour` `RefinerySmokeOffsetOne`
`RefinerySmokeOffsetThree` `RefinerySmokeOffsetTwo`
`RefinerySmokeParticleSystem` `RejoinTeamIfLimboed` `Reload` `ReloadAmount`
`RepairTurretIndex` `RepairTurretWeapon` `Repairable` `Repel.Enemy.Range`
`Repel.Enemy.Type` `RequiresStolenAlliedTech` `RequiresStolenSovietTech`
`RequiresStolenThirdTech` `ReselectIfLimboed` `ResourceDestination`
`ResourceGatherer` `RevealToAll` `SecretHouses` `SecretLab` `Selectable`
`SelfHealing` `SensorArray` `Sensors` `SensorsSight` `ShockTurretIndex`
`ShockTurretWeapon` `Sight` `SinkingSound` `Size` `SizeLimit` `SlaveRegenRate`
`SlaveReloadRate` `Slaved` `SlavesNumber` `SniperTurretIndex`
`SniperTurretWeapon` `Soylent` `SpawnRegenRate` `SpawnReloadRate` `Spawned`
`Spawner.AutoDeploy` `Spawner.AutoDeploy.Delay` `Spawner.AutoDeploy.MinReady`
`Spawns` `SpawnsNumber` `SpecialThreatValue` `Speed` `SpeedType`
`SpyEffect.GapVision` `SpyEffect.GapVision.Duration`
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
`TargetCoordOffset` `TargetLaser` `Teleporter` `TerroristExplodeTurretIndex`
`TerroristExplodeTurretWeapon` `ThreatAvoidanceCoefficient` `ThreatPosed`
`TiberiumProof` `TiltCrashJumpjet` `ToProtect` `ToTile` `TogglePower`
`TooBigToFitUnderBridge` `Trainable` `TraitsRandomCount` `TurretRecoil`
`TurretRotateSound` `TurretSpins` `TurretTravel` `TypeImmune` `UIName`
`UndeployDelay` `UndeploySound` `UndeploysInto` `Underwater` `UnitAbsorb`
`UnitLost.Suppress` `UnitReload` `UnitRepair` `UnitsCostBonus`
`UnitsGainSelfHeal` `UnloadingClass` `Unnatural` `Unsellable` `Upgrades`
`UseOwnName` `VHPScan` `VeteranAbilities` `VirusTurretIndex`
`VirusTurretWeapon` `VoiceAttack` `VoiceCapture` `VoiceCrashing` `VoiceDeploy`
`VoiceEnter` `VoiceFeedback` `VoiceHarvest` `VoiceMove`
`VoiceSecondaryWeaponAttack` `VoiceSelect` `VoiceSelectDeactivated`
`VoiceSelectEnslaved` `VoiceSpecialAttack` `WalkRate` `Wall` `WaterBound`
`Weapon.RangeUsesCellSpread` `WeaponStages` `WeaponsFactory`
`WeatherSystem.Amounts` `WeatherSystem.Rate` `WeatherSystem.RequiresPower`
`WeatherSystem.Types` `Weight` `WorkingSound` `YuriBarracks`
`YuriPrimeTurretIndex` `YuriPrimeTurretWeapon` `ZFudgeBridge` `ZFudgeColumn`
`ZFudgeTunnel`
