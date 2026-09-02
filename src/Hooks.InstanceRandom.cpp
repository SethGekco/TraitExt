// Per-instance random (TraitsRandomScope=Instance).
//
// Hook choice per the Hook Encyclopedia (encyclopedia/Techno-Instance-Lifecycle.md):
//   0x6F9E50 TechnoClass_Update — the per-instance LOGIC tick, ECX=TechnoClass*.
//   The page documents it as frame-synchronous and explicitly safe for synced
//   game state and for drawing from ScenarioClass::Instance->Random, and records
//   in-game verification from a standalone Syringe DLL. It also warns that the
//   tick is per-instance and re-runs every frame, so one-shot work needs a guard
//   — hence the seen-set below.
//   0x6F4500 TechnoClass_DTOR — used to drop the guard entry, because instance
//   pointers get recycled and a stale entry would silently skip a new unit.
//
// SYNC: Health/Veterancy/Ammo are game LOGIC, so the draw must be identical on
// every client. We therefore use the game's own synced RNG
// (ScenarioClass::Instance->Random) rather than any local generator — the exact
// mistake that causes the classic shared-RNG desync. All clients tick the same
// units in the same order, so the draw sequence matches.

#include "TraitExt.h"
#include "TraitEngine.h"

#include <Phobos.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <FootClass.h>          // generic_cast in TechnoClass.h needs it complete
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <ScenarioClass.h>

#include <unordered_set>
#include <cstdlib>
#include <cstring>

namespace
{
    std::unordered_set<void*> g_Seen;

    bool ParseDouble(const char* s, double& out)
    {
        if (!s || !*s)
            return false;
        char* end = nullptr;
        const double v = std::strtod(s, &end);
        if (!end || end == s)
            return false;
        out = v;
        return true;
    }

    void ApplyOneTrait(TechnoClass* pThis, const TraitExt::TraitDef* pDef)
    {
        TechnoTypeClass* const pType = pThis->GetTechnoType();
        if (!pType)
            return;

        for (const auto& entry : pDef->Entries)
        {
            const char* key = entry.first.c_str();
            double value = 0.0;
            if (!ParseDouble(entry.second.c_str(), value))
            {
                Debug::Log("[TraitExt]   (instance) %s: key '%s' is not numeric; "
                    "instance scope supports Health/Strength, Veterancy, Ammo only\n",
                    pType->ID, key);
                continue;
            }

            if (!_stricmp(key, "Health") || !_stricmp(key, "Strength"))
            {
                int hp = static_cast<int>(value);
                if (hp < 1) hp = 1;
                if (hp > pType->Strength) hp = pType->Strength;
                pThis->Health = hp;
            }
            else if (!_stricmp(key, "Veterancy"))
            {
                pThis->Veterancy.Veterancy = static_cast<float>(value);
            }
            else if (!_stricmp(key, "Ammo"))
            {
                int ammo = static_cast<int>(value);
                if (ammo < 0) ammo = 0;
                if (ammo > pType->Ammo) ammo = pType->Ammo;
                pThis->Ammo = ammo;
            }
            else
            {
                // Image/Cost/Armor etc. live on the shared TechnoTypeClass, so
                // they cannot differ between instances. Say so instead of
                // silently doing nothing.
                Debug::Log("[TraitExt]   (instance) %s: key '%s' is TYPE-level and "
                    "cannot vary per instance — use TraitsRandomScope=Type for it\n",
                    pType->ID, key);
            }
        }
    }
}

DEFINE_HOOK(0x6F9E50, TechnoClass_Update_InstanceRandom, 0x5)
{
    GET(TechnoClass*, pThis, ECX);

    if (!pThis || !TraitExt::InstanceRandom::Any())
        return 0;

    // Per-instance one-shot guard (the tick re-runs every frame).
    if (g_Seen.count(pThis))
        return 0;

    TechnoTypeClass* const pType = pThis->GetTechnoType();
    if (!pType)
        return 0;

    const TraitExt::InstancePool* pPool = TraitExt::InstanceRandom::Find(pType->ID);
    if (!pPool)
        return 0;

    g_Seen.insert(pThis);

    ScenarioClass* const pScen = ScenarioClass::Instance;
    if (!pScen)
        return 0;

    const int poolN = static_cast<int>(pPool->Traits.size());
    const int count = (pPool->CountMax > pPool->CountMin)
        ? pScen->Random.RandomRanged(pPool->CountMin, pPool->CountMax)
        : pPool->CountMin;

    // Partial Fisher-Yates over indices, drawing from the SYNCED generator.
    std::vector<int> idx(poolN);
    for (int i = 0; i < poolN; ++i)
        idx[i] = i;

    for (int i = 0; i < count && i < poolN; ++i)
    {
        const int j = pScen->Random.RandomRanged(i, poolN - 1);
        const int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;

        const TraitExt::TraitDef* pDef = pPool->Traits[idx[i]];
        Debug::Log("[TraitExt] (instance) %s @%p drew '%s'\n",
            pType->ID, pThis, pDef->Name.c_str());
        ApplyOneTrait(pThis, pDef);
    }

    return 0;
}

// STOLEN SIZE MUST BE 0x5. The prologue is
//   6F4500 51      push ecx
//   6F4501 53      push ebx
//   6F4502 56      push esi
//   6F4503 8B F1   mov esi,ecx     <- cumulative exactly 5
//   6F4505 33 DB   xor ebx,ebx
// Declaring 0x6 splits `33 DB` and leaves a dangling `DB` byte, corrupting the
// instruction stream and sending execution to a wild address. That is what
// caused three reproducible C0000005 crashes at 0x09C00126 (an address in no
// module at all). All four frameworks declare 0x5 here for this reason.
namespace TraitExt
{
    void ApplyInstanceTraits(TechnoClass* pThis,
        const std::vector<const TraitDef*>& traits, const char* reason)
    {
        if (!pThis)
            return;
        TechnoTypeClass* const pType = pThis->GetTechnoType();
        for (const TraitDef* pDef : traits)
        {
            Debug::Log("[TraitExt] (%s) %s @%p applying '%s'\n",
                reason, pType ? pType->ID : "?", pThis, pDef->Name.c_str());
            ApplyOneTrait(pThis, pDef);
        }
    }
}

// Spy infiltration -> force traits onto the infiltrated BUILDING INSTANCE.
//
// ENCYCLOPEDIA-CHECKED (encyclopedia/Spy-Infiltration.md, registry/hooks.csv):
// 0x4571E0 is BuildingClass::Infiltrate. ECX = the entered BuildingClass*,
// [ESP+0x4] = the infiltrator's HouseClass*. Antares/Ares wrap the whole
// function and return 0x4575A2 when their dispatch consumes the event; Syringe
// still runs every registered handler, so an observer here is live regardless
// of load order. Three other consumers already sit on this address (Antares,
// IntelExt, AcademyExt).
//
//   *** THIS HANDLER MUST ALWAYS RETURN 0. ***
//
// Returning a jump target would contend with Antares for control of the site
// and let load order decide whose spy effects run at all. Stolen size is 0x5,
// matching every other consumer — a different size at a shared address can
// overlap a neighbouring patch and corrupt its JMP displacement.
//
// Only instance-level keys are applied, so buildability is untouched and the
// sidebar repaint the page warns about is not required here.
DEFINE_HOOK(0x4571E0, BuildingClass_Infiltrate_TraitExt, 0x5)
{
    GET(BuildingClass*, pVictim, ECX);
    GET_STACK(HouseClass*, pEnterer, 0x4);

    if (!pVictim || !pEnterer || !TraitExt::SpyTraits::Any())
        return 0;

    BuildingTypeClass* const pType = pVictim->Type;
    if (!pType)
        return 0;

    if (const auto* pList = TraitExt::SpyTraits::Find(pType->ID))
        TraitExt::ApplyInstanceTraits(pVictim, *pList, "spy");

    return 0;
}

DEFINE_HOOK(0x6F4500, TechnoClass_DTOR_InstanceRandom, 0x5)
{
    GET(TechnoClass*, pThis, ECX);
    if (pThis)
        g_Seen.erase(pThis);
    return 0;
}
