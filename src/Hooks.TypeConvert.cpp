// VariantType - PERSISTENT type-pointer repoint, the mechanism that makes
// every type-level TechnoType tag reachable from a gate.
//
// The draw-time swap in Hooks.InstanceRandom.cpp points TechnoClass::Type at a
// variant clone for the width of one DrawIt call and puts it straight back.
// That narrow window is the ONLY reason gated traits reached art and weapons
// and nothing else: the engine reads practically every type field as
// pThis->Type->Field, dereferencing the instance's pointer AT the read site, so
// a pointer left repointed answers every one of those reads with the variant's
// value. No per-tag substitution, and - worth saying because it drove the
// design - NO NEW HOOKS: this is called from the gate evaluation we already run
// inside TechnoClass_Update.
//
// What does NOT follow the pointer is state cached into the instance when it was
// created. That list is not guesswork: Phobos ships this as the warhead
// Convert.From/Convert.To feature (TechnoExt::ConvertToType,
// Phobos/src/Ext/Techno/Body.cpp:345) and its vanilla path enumerates exactly
// which derived state has to be fixed up. This mirrors that list, and adds the
// reverse direction Phobos never needed - their conversion is a one-way warhead
// effect, ours is a gate that closes again.
//
// Deliberately NOT delegating to Ares/Antares' own ConvertTypeTo: Phobos binds
// it at a hardcoded module offset (0x43650 on Ares 3.0, 0x44130 on 3.0p1), so
// it breaks on any build whose layout differs. The vanilla path below needs
// nothing but YRpp.

#include "TraitEngine.h"

#include <TechnoClass.h>
#include <FootClass.h>
#include <UnitClass.h>
#include <InfantryClass.h>
#include <AircraftClass.h>
#include <HouseClass.h>
#include <LocomotionClass.h>
#include <TemporalClass.h>

#include <Utilities/Debug.h>

#include <unordered_map>
#include <string>
#include <cstring>

namespace TraitExt
{
    namespace VariantType
    {
        namespace
        {
            bool g_Enabled = true;
            bool g_Any = false;

            // The type the unit was built as. Recorded on the FIRST conversion
            // and never overwritten, so closing a gate restores what the unit
            // actually was rather than whatever another gate converted it to.
            std::unordered_map<::TechnoClass*, ::TechnoTypeClass*> g_OrigType;
            // Which clone each unit currently wears, so a re-evaluation that
            // lands on the same clone costs nothing.
            std::unordered_map<::TechnoClass*, ::TechnoTypeClass*> g_NowType;

            // Where the instance keeps its own type pointer. Each FootClass
            // subclass declares its own typed field; they are NOT a single
            // TechnoClass member, which is why this has to switch on WhatAmI.
            ::TechnoTypeClass** TypeSlot(::TechnoClass* pThis)
            {
                switch (pThis->WhatAmI())
                {
                case AbstractType::Infantry:
                    return reinterpret_cast<::TechnoTypeClass**>(
                        &static_cast<::InfantryClass*>(pThis)->Type);
                case AbstractType::Unit:
                    return reinterpret_cast<::TechnoTypeClass**>(
                        &static_cast<::UnitClass*>(pThis)->Type);
                case AbstractType::Aircraft:
                    return reinterpret_cast<::TechnoTypeClass**>(
                        &static_cast<::AircraftClass*>(pThis)->Type);
                default:
                    // BuildingClass is excluded on purpose, not forgotten. A
                    // building's foundation is written into map cells at
                    // Unlimbo, so repointing its type leaves the occupancy of
                    // the old footprint behind. Phobos refuses this too.
                    return nullptr;
                }
            }

            // The actual repoint plus every fix-up. Direction-agnostic: the
            // same work is needed converting to a clone and back again.
            bool Repoint(::TechnoClass* pThis, ::TechnoTypeClass* pTo, const char* why)
            {
                ::TechnoTypeClass** const slot = TypeSlot(pThis);
                if (!slot)
                    return false;

                ::TechnoTypeClass* const pFrom = *slot;
                if (!pFrom || !pTo || pFrom == pTo)
                    return false;

                // SANITY-CHECK THE TARGET BEFORE TOUCHING THE UNIT.
                //
                // This guard is here because its absence broke a GI. A clone
                // type that had not been filled in properly carried
                // Strength=0, the conversion went ahead, and the proportional
                // HP rescale produced "hp 31/31 -> 1/0" - a unit with no
                // health that also teleported, because a zeroed Locomotor
                // CLSID is not a locomotor. Every symptom was downstream of a
                // target type that was never viable, and nothing checked.
                //
                // Refuse instead. A gate that does nothing is a bug report; a
                // gate that hollows out the unit is a broken game.
                if (pTo->Strength <= 0)
                {
                    Debug::Log("[TraitExt] CONVERT REFUSED %s -> %s: the target type "
                        "has Strength=%d, so converting would leave the unit with no "
                        "health. The clone section is incomplete - check for a "
                        "'copied N key(s) literally' line for %s at load.\n",
                        pFrom->ID, pTo->ID, pTo->Strength, pTo->ID);
                    return false;
                }

                // Cross-category is impossible: the slot is typed, and the
                // engine's RTTI would disagree with the object's vtable.
                if (pFrom->WhatAmI() != pTo->WhatAmI())
                {
                    Debug::Log("[TraitExt] CONVERT refused %s -> %s: different "
                        "categories, a %s instance cannot wear a %s type\n",
                        pFrom->ID, pTo->ID, pFrom->ID, pTo->ID);
                    return false;
                }

                auto* const pFoot = static_cast<::FootClass*>(pThis);

                // A CLEG mid-freeze holds a pointer that the conversion would
                // leave pointing at stale state.
                if (auto* const pTemporal = pThis->TemporalImUsing)
                    if (pTemporal->Target)
                        pTemporal->LetGo();

                ::HouseClass* const pOwner = pThis->Owner;
                const int oldHealth = pThis->Health;
                const int oldMax = pFrom->Strength;

                // House counters are keyed BY TYPE, so the old type has to be
                // untracked before the pointer moves and the new one tracked
                // after. Skipping this is how a converted unit ends up counted
                // twice, or counted as the wrong type for build limits.
                if (!pThis->InLimbo)
                    pOwner->RegisterLoss(pThis, false);
                pOwner->RemoveTracking(pThis);

                *slot = pTo;

                // Current HP is absolute but maximum comes from the type, so
                // the pointer move alone would make a 50%-damaged unit read as
                // full or overfull. Proportional is the only sane mapping -
                // and it is lossy, which is why a flickering gate must not be
                // allowed to convert repeatedly (see the GateDwell floor).
                if (oldMax > 0)
                    pThis->SetHealthPercentage(
                        static_cast<double>(oldHealth) / static_cast<double>(oldMax));
                pThis->EstimatedHealth = pThis->Health;

                pOwner->AddTracking(pThis);
                if (!pThis->InLimbo)
                    pOwner->RegisterGain(pThis, false);
                pOwner->RecheckTechTree = true;

                // Ammo is per-instance; the new maximum may be smaller.
                const int oldAmmo = pThis->Ammo;
                if (oldAmmo > pTo->Ammo)
                {
                    pThis->Ammo = pTo->Ammo;
                    pThis->Mark(MarkType::Change);
                }

                // ROT is copied into the facing object, so it does not follow
                // the pointer. Aircraft steer with the secondary facing.
                if (pTo->WhatAmI() == AbstractType::AircraftType)
                    pThis->SecondaryFacing.SetROT(pTo->ROT);
                else
                    pThis->PrimaryFacing.SetROT(pTo->ROT);

                // The locomotor is a COM object instantiated at Unlimbo from
                // the type's CLSID, so a type whose Locomotor= differs needs a
                // new one or the unit keeps moving by the old rules. Ending
                // piggybacks first matters: a piggybacked locomotor that is
                // released underneath leaves the unit in a stuck state - the
                // same failure mode the Magnetron page documents when the
                // jumpjet imbue is torn down out of order.
                // A zeroed CLSID is not a locomotor, it is an unfilled field.
                // Replacing a working locomotor with one built from zeros is
                // how a GI ends up teleporting, so treat it as "no change
                // wanted" rather than as a request.
                static const CLSID kNoLoco {};
                CLSID nowLoco {};
                ILocomotion* const pILoco = pFoot->Locomotor;
                if (pTo->Locomotor == kNoLoco)
                {
                    Debug::Log("[TraitExt]   %s: target has no Locomotor set, keeping "
                        "the current one rather than building one from zeros\n",
                        pTo->ID);
                }
                else if (pILoco
                    && SUCCEEDED(static_cast<::LocomotionClass*>(pILoco)->GetClassID(&nowLoco))
                    && nowLoco != pTo->Locomotor)
                {
                    while (::LocomotionClass::End_Piggyback(pFoot->Locomotor))
                        ;
                    if (auto newLoco = ::LocomotionClass::CreateInstance(pTo->Locomotor))
                    {
                        newLoco->Link_To_Object(pFoot);
                        pFoot->Locomotor = std::move(newLoco);
                    }
                }

                Profile::CountConvert();
                Debug::Log("[TraitExt] CONVERT %s @%p: %s -> %s (%s) "
                    "hp %d/%d -> %d/%d\n",
                    pFrom->ID, pThis, pFrom->ID, pTo->ID, why,
                    oldHealth, oldMax, pThis->Health, pTo->Strength);
                return true;
            }
        }

        bool Enabled() { return g_Enabled; }
        void SetEnabled(bool on) { g_Enabled = on; }
        bool Any() { return g_Any; }
        void MarkUsed() { g_Any = true; }

        bool Supports(::TechnoClass* pThis)
        {
            return pThis && TypeSlot(pThis) != nullptr;
        }

        bool Convert(::TechnoClass* pThis, const char* cloneID)
        {
            if (!g_Enabled || !pThis || !cloneID || !*cloneID)
                return false;

            ::TechnoTypeClass* const pTo = ::TechnoTypeClass::Find(cloneID);
            if (!pTo)
            {
                Debug::Log("[TraitExt] CONVERT %p: clone type '%s' does not exist - "
                    "the INI synthesis did not run or was overwritten\n",
                    pThis, cloneID);
                return false;
            }

            ::TechnoTypeClass** const slot = TypeSlot(pThis);
            if (!slot)
            {
                Debug::Log("[TraitExt] CONVERT refused @%p: only vehicles, infantry "
                    "and aircraft can be converted (a building's foundation is "
                    "already written into the map cells)\n", pThis);
                return false;
            }

            // First conversion of this unit: remember what it was BUILT as.
            // Never overwrite - a second gate converting an already-converted
            // unit must not make its own clone the thing we restore to.
            if (g_OrigType.find(pThis) == g_OrigType.end())
                g_OrigType[pThis] = *slot;

            if (!Repoint(pThis, pTo, "gate opened"))
                return false;

            g_NowType[pThis] = pTo;
            return true;
        }

        bool Restore(::TechnoClass* pThis)
        {
            if (!pThis)
                return false;

            const auto it = g_OrigType.find(pThis);
            if (it == g_OrigType.end())
                return false;

            ::TechnoTypeClass* const pBack = it->second;
            Profile::CountRestore();
            const bool ok = Repoint(pThis, pBack, "gate closed");

            g_OrigType.erase(it);
            g_NowType.erase(pThis);
            return ok;
        }

        bool IsConverted(::TechnoClass* pThis)
        {
            return pThis && g_OrigType.find(pThis) != g_OrigType.end();
        }

        ::TechnoTypeClass* OriginalType(::TechnoClass* pThis)
        {
            const auto it = g_OrigType.find(pThis);
            return it == g_OrigType.end() ? nullptr : it->second;
        }

        void Forget(::TechnoClass* pThis)
        {
            // Called from the destructor hook. NOT a restore: the object is
            // going away, and touching house tracking for a dying unit would
            // double-count the loss the engine is already registering.
            g_OrigType.erase(pThis);
            g_NowType.erase(pThis);
        }

        void Clear()
        {
            g_OrigType.clear();
            g_NowType.clear();
        }
    }
}

// ---- Per-clone mode & key classification ----------------------------------
// A clone is 1:1 with the gated trait that created it, so the clone id is the
// natural handle for "does this trait need the full conversion or will the
// cheap draw swap do". Auto is the default BECAUSE Rex asked for the conversion
// to be invisible: write vanilla tags, and the engine picks the path that can
// actually deliver them.

namespace TraitExt
{
    namespace VariantType
    {
        namespace
        {
            std::unordered_map<std::string, Mode> g_Mode;
            std::unordered_map<std::string, bool> g_SawTypeKey;
            int g_DwellFloor = 15;      // frames; see WantsConvert
        }

        void SetMode(const std::string& cloneID, Mode m)
        {
            g_Mode[cloneID] = m;
            if (m == Mode::Convert)
                g_Any = true;
        }

        Mode ModeOf(const char* cloneID)
        {
            if (!cloneID || !*cloneID)
                return Mode::Auto;
            const auto it = g_Mode.find(cloneID);
            return it == g_Mode.end() ? Mode::Auto : it->second;
        }

        void MarkTypeKey(const std::string& cloneID)
        {
            g_SawTypeKey[cloneID] = true;
            g_Any = true;
        }

        bool WantsConvert(const char* cloneID)
        {
            if (!cloneID || !*cloneID)
                return false;
            switch (ModeOf(cloneID))
            {
            case Mode::Draw:    return false;
            case Mode::Convert: return true;
            case Mode::Auto:
            default:
                break;
            }
            const auto it = g_SawTypeKey.find(cloneID);
            return it != g_SawTypeKey.end() && it->second;
        }

        int DwellFloor() { return g_DwellFloor; }
        void SetDwellFloor(int frames) { g_DwellFloor = frames < 0 ? 0 : frames; }

        // Which mechanism can deliver this key to ONE unit. The three that
        // already worked are kept separate from Type because they are cheap:
        // a trait that only touches them never pays for a conversion.
        Reach ClassifyKey(const char* k)
        {
            if (!k || !*k)
                return Reach::Type;

            static const char* const kInstance[] = {
                "Health", "Strength", "Veterancy", "Ammo",
            };
            for (const char* s : kInstance)
                if (!_stricmp(k, s))
                    return Reach::Instance;

            if (TraitExt::IsWeaponKey(k))
                return Reach::Weapon;

            static const char* const kDraw[] = {
                "Image", "Voxel", "Remapable", "AlphaImage", "Palette",
                "Cameo", "AltCameo",
                "Turret", "TurretCount", "TurretOffset", "UseTurretShadow",
                "Turret.RangeBands", "Turret.RangeIndices",
                "TurretNotExportedOnGround", "TurretAnim", "TurretAnimIsVoxel",
                "TurretAnimX", "TurretAnimY", "TurretAnimZAdjust",
                "WeaponTurretIndex1", "WeaponTurretIndex2",
                "WeaponTurretIndex3", "WeaponTurretIndex4",
            };
            for (const char* s : kDraw)
                if (!_stricmp(k, s))
                    return Reach::Draw;

            // Asked of the HOUSE before any unit exists, so there is no
            // instance to answer for. Not a missing feature - a category
            // error. These stay load-time, where they work completely.
            static const char* const kBuild[] = {
                "Prerequisite", "Prerequisite.List", "Prerequisite.Lists",
                "Prerequisite.StolenTech", "Prerequisite.Negative",
                "PrerequisiteOverride", "TechLevel", "Owner",
                "ForbiddenHouses", "RequiredHouses", "BuildLimit",
                "AIBuildThis", "AIBuildCount", "Unbuildable",
            };
            for (const char* s : kBuild)
                if (!_stricmp(k, s))
                    return Reach::BuildScope;

            // Lives in artmd.ini, which TraitExt never reads. Cameo/AltCameo
            // are the deliberate exceptions and are handled above.
            static const char* const kArt[] = {
                "Sequence", "Facings", "WalkFrames", "FireUp", "Crawls",
                "Shadow", "StartWalkFrame", "StartFiringFrame", "Normalized",
            };
            for (const char* s : kArt)
                if (!_stricmp(k, s))
                    return Reach::ArtFile;

            // Everything else is a type field the engine reads through the
            // instance's type pointer, which conversion repoints.
            return Reach::Type;
        }

        const char* ReachName(Reach r)
        {
            switch (r)
            {
            case Reach::Instance:   return "per-instance field";
            case Reach::Weapon:     return "GetWeapon";
            case Reach::Draw:       return "draw swap";
            case Reach::Type:       return "type conversion";
            case Reach::BuildScope: return "house build list";
            case Reach::ArtFile:    return "artmd.ini";
            }
            return "?";
        }
    }
}
