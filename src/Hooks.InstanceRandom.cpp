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
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <unordered_map>
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
            else if (!_stricmp(key, "Image"))
            {
                // Handled by the variant-clone path, not here.
            }
            else
            {
                // Cost/Armor etc. live on the shared TechnoTypeClass, so they
                // cannot differ between instances. Say so instead of silently
                // doing nothing. (Image is the exception — see VariantArt.)
                Debug::Log("[TraitExt]   (instance) %s: key '%s' is TYPE-level and "
                    "cannot vary per instance - use TraitsRandomScope=Type for it\n",
                    pType->ID, key);
            }
        }
    }
}

DEFINE_HOOK(0x6F9E50, TechnoClass_Update_InstanceRandom, 0x5)
{
    GET(TechnoClass*, pThis, ECX);

    // Logic tick is the safety net for the draw-time Type swap: gameplay must
    // never run against a clone type.
    RestorePending();

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

        const int chosen = idx[i];
        const TraitExt::TraitDef* pDef = pPool->Traits[chosen];
        Debug::Log("[TraitExt] (instance) %s @%p drew '%s'\n",
            pType->ID, pThis, pDef->Name.c_str());
        ApplyOneTrait(pThis, pDef);

        // If that trait carried a variant look, remember it for draw time.
        if (chosen < static_cast<int>(pPool->CloneIDs.size())
            && !pPool->CloneIDs[chosen].empty())
        {
            TraitExt::VariantArt::Assign(pThis, pPool->CloneIDs[chosen].c_str());
        }
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
namespace
{
    // unit -> the clone type it should be DRAWN as.
    std::unordered_map<void*, UnitTypeClass*> g_Variant;
    bool g_VariantEnabled = true;

    // Exactly one unit may be mid-swap at a time; draws are sequential, so the
    // next draw (or the logic tick) restores the previous one. Keeping the
    // window this small is what stops house counts / build limits / prereqs
    // from ever seeing a clone.
    UnitClass* g_Swapped = nullptr;
    UnitTypeClass* g_SwappedOriginal = nullptr;

    void RestorePending()
    {
        if (g_Swapped && g_SwappedOriginal)
            g_Swapped->Type = g_SwappedOriginal;
        g_Swapped = nullptr;
        g_SwappedOriginal = nullptr;
    }
}

namespace TraitExt
{
    namespace VariantArt
    {
        void Assign(::TechnoClass* pThis, const char* cloneID)
        {
            if (!pThis || !cloneID || !*cloneID)
                return;
            if (auto* const pClone = UnitTypeClass::Find(cloneID))
                g_Variant[pThis] = pClone;
        }

        void Forget(::TechnoClass* pThis)
        {
            if (g_Swapped == static_cast<void*>(pThis))
                RestorePending();
            g_Variant.erase(pThis);
        }

        bool Enabled() { return g_VariantEnabled; }
        void SetEnabled(bool on) { g_VariantEnabled = on; }
        bool Any() { return !g_Variant.empty(); }
    }

    void ApplyInstanceTraits(::TechnoClass* pThis,
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

// Per-unit variant art, rendering only.
//
// 0x73B140 = UnitClass::DrawObject (YRpp: "Draw() calls one of these"), the
// single dispatcher in front of DrawAsVXL/DrawAsSHP. ECX = UnitClass*. Boundary
// verified: 83 EC 38 / 53 / 55 is exactly 5 bytes. No framework hooks it, so
// this site is uncontended.
//
// Image lives on ObjectTypeClass (shared by the whole type), so a per-unit look
// needs a genuinely different type. We point Type at the synthesised clone only
// for the draw and restore it immediately afterwards, which is why gameplay
// never sees it. The restore happens at the NEXT draw and again on the logic
// tick, so a missed one self-corrects within a frame instead of persisting.
DEFINE_HOOK(0x73B140, UnitClass_DrawObject_VariantArt, 0x5)
{
    RestorePending();

    if (!g_VariantEnabled || g_Variant.empty())
        return 0;

    GET(UnitClass*, pThis, ECX);
    if (!pThis)
        return 0;

    const auto it = g_Variant.find(pThis);
    if (it == g_Variant.end() || !it->second || pThis->Type == it->second)
        return 0;

    g_Swapped = pThis;
    g_SwappedOriginal = pThis->Type;
    pThis->Type = it->second;
    return 0;
}

DEFINE_HOOK(0x6F4500, TechnoClass_DTOR_InstanceRandom, 0x5)
{
    GET(TechnoClass*, pThis, ECX);
    if (pThis)
    {
        g_Seen.erase(pThis);
        TraitExt::VariantArt::Forget(pThis);
    }
    return 0;
}
