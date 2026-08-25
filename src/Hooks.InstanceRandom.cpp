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

    void ApplyInstanceTrait(TechnoClass* pThis, const TraitExt::TraitDef* pDef)
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
        ApplyInstanceTrait(pThis, pDef);
    }

    return 0;
}

DEFINE_HOOK(0x6F4500, TechnoClass_DTOR_InstanceRandom, 0x6)
{
    GET(TechnoClass*, pThis, ECX);
    if (pThis)
        g_Seen.erase(pThis);
    return 0;
}
