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
#include <cmath>
#include <ScenarioClass.h>
#include <Fundamentals.h>   // Unsorted::CurrentFrame

#include <unordered_set>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace
{
    std::unordered_set<void*> g_Seen;
    // unit -> frame at which its LOOK should be re-rolled again.
    std::unordered_map<void*, int> g_NextReroll;
    // unit -> frame of its next prerequisite re-check, and which gated traits
    // are currently satisfied for it.
    std::unordered_map<void*, int> g_NextCondCheck;
    std::unordered_map<void*, unsigned> g_CondActive;

    // Cosmetic re-roll RNG. Deliberately LOCAL, not ScenarioClass::Random:
    // appearance is unsynced by design, and drawing from the synced generator
    // here would perturb the stream every N frames for purely visual reasons.
    std::uint32_t g_CosmeticRng = 0x1234567u;
    std::uint32_t CosmeticRand()
    {
        g_CosmeticRng ^= g_CosmeticRng << 13;
        g_CosmeticRng ^= g_CosmeticRng >> 17;
        g_CosmeticRng ^= g_CosmeticRng << 5;
        return g_CosmeticRng;
    }

    // Defined further down with the variant-art state; declared here because
    // the logic-tick hook below is the safety net that undoes a draw swap.
    void RestorePending();

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

    // One line per type+key for the whole session. These run on the per-unit
    // logic tick, so an unguarded Debug::Log here floods the log.
    void WarnOnce(const char* typeID, const char* key, const char* why)
    {
        static std::unordered_set<std::string> s_seen;
        std::string k(typeID ? typeID : "?"); k += '|'; k += key;
        if (s_seen.insert(k).second)
            Debug::Log("[TraitExt]   (instance) %s: key '%s' %s\n",
                typeID ? typeID : "?", key, why);
    }

    // Does this unit's OWNER have everything the gate asks for? Judged per
    // unit, never per type: types are shared by every house, so a type-level
    // unlock would arm the enemy from your own tech.
    bool OwnerMeets(TechnoClass* pThis, const std::vector<std::string>& req)
    {
        HouseClass* const pOwner = pThis ? pThis->Owner : nullptr;
        if (!pOwner)
            return false;

        for (const auto& id : req)
        {
            TechnoTypeClass* const pNeed = TechnoTypeClass::Find(id.c_str());
            if (!pNeed || pOwner->CountOwnedAndPresent(pNeed) <= 0)
                return false;
        }
        return true;
    }

    // Power is a HOUSE property, so this is house-wide by nature: every unit of
    // the owner flips together when the base browns out. That is what makes a
    // "powered unit" read correctly - it is the base losing power, not the unit.
    bool PowerMeets(TechnoClass* pThis, const TraitExt::ConditionalTrait& ct)
    {
        if (ct.NeedPower == TraitExt::ConditionalTrait::Power::Ignore)
            return true;

        HouseClass* const pOwner = pThis ? pThis->Owner : nullptr;
        if (!pOwner)
            return false;

        return ct.NeedPower == TraitExt::ConditionalTrait::Power::Full
            ? pOwner->HasFullPower()
            : pOwner->HasLowPower();
    }

    // The owner must have NONE of these. Requirement= cannot express "while you
    // lack X", and that is the natural way to write a penalty or a fallback form.
    bool OwnerLacks(TechnoClass* pThis, const std::vector<std::string>& forbid)
    {
        if (forbid.empty())
            return true;
        HouseClass* const pOwner = pThis ? pThis->Owner : nullptr;
        if (!pOwner)
            return false;

        for (const auto& id : forbid)
        {
            TechnoTypeClass* const pNo = TechnoTypeClass::Find(id.c_str());
            if (pNo && pOwner->CountOwnedAndPresent(pNo) > 0)
                return false;
        }
        return true;
    }

    // Gates on the unit's OWN state rather than its owner's. Health is a percent
    // so one threshold reads the same on a 300hp tank and a 2000hp one.
    bool SelfMeets(TechnoClass* pThis, const TraitExt::ConditionalTrait& ct)
    {
        TechnoTypeClass* const pType = pThis ? pThis->GetTechnoType() : nullptr;
        if (!pType)
            return false;

        if (ct.HealthBelowPct > 0)
        {
            if (pType->Strength <= 0)
                return false;
            const int pct = pThis->Health * 100 / pType->Strength;
            if (pct > ct.HealthBelowPct)
                return false;
        }

        if (ct.MinVeterancy >= 0)
        {
            const int rank = pThis->Veterancy.IsElite() ? 2
                : pThis->Veterancy.IsVeteran() ? 1 : 0;
            if (rank < ct.MinVeterancy)
                return false;
        }
        return true;
    }

    void ApplyOneTrait(TechnoClass* pThis, const TraitExt::TraitDef* pDef, bool hasClone)
    {
        TechnoTypeClass* const pType = pThis->GetTechnoType();
        if (!pType)
            return;

        for (const auto& entry : pDef->Entries)
        {
            const char* key = entry.first.c_str();

            // TraitExt's own config keys, consumed at load. Not stats, and not
            // game keys — skip them here or they warn as "not numeric".
            if (!_stricmp(key, "ForceBodyFacing"))
                continue;

            double value = 0.0;
            if (!ParseDouble(entry.second.c_str(), value))
            {
                // A variant clone already carries this key, so there is nothing
                // to report - and reporting it per unit per frame buried the log.
                if (!hasClone)
                    WarnOnce(pType->ID, key, "is not numeric; instance scope supports "
                        "Health/Strength, Veterancy, Ammo only");
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
                if (!hasClone)
                    WarnOnce(pType->ID, key, "is TYPE-level and cannot vary per instance "
                        "- use TraitsRandomScope=Type for it");
            }
        }
    }
}

namespace
{
    // Re-checked on a cadence rather than every frame: prerequisites change
    // when a building finishes or dies, not continuously, and this runs for
    // every unit alive.
    void EvaluateConditionals(TechnoClass* pThis)
    {
        if (!TraitExt::Conditional::Any() || !pThis)
            return;

        TechnoTypeClass* const pType = pThis->GetTechnoType();
        if (!pType)
            return;

        const auto* pList = TraitExt::Conditional::Find(pType->ID);
        if (!pList)
            return;

        const int now = Unsorted::CurrentFrame;
        const auto nit = g_NextCondCheck.find(pThis);
        if (nit != g_NextCondCheck.end() && now < nit->second)
            return;
        g_NextCondCheck[pThis] = now + 15;   // ~1s

        unsigned mask = 0;
        for (size_t i = 0; i < pList->size() && i < 32; ++i)
        {
            // Both gates must hold: "I have a Battle Lab" AND "I am standing
            // next to one of these". Either half may be absent, in which case
            // it does not constrain.
            const TraitExt::ConditionalTrait& c = (*pList)[i];
            if (OwnerMeets(pThis, c.Requirement)
                && OwnerLacks(pThis, c.RequireNot)
                && TraitExt::Conditional::NearMeets(pThis, c)
                && PowerMeets(pThis, c)
                && SelfMeets(pThis, c))
                mask |= (1u << i);
        }

        const unsigned was = g_CondActive.count(pThis) ? g_CondActive[pThis] : 0u;
        if (mask == was)
            return;
        g_CondActive[pThis] = mask;

        // Appearance follows the gate both ways; stats are applied when the
        // gate opens and deliberately NOT rolled back when it closes, because
        // un-applying a fold is not generally possible.
        bool tookLook = false;
        for (size_t i = 0; i < pList->size() && i < 32; ++i)
        {
            const bool on = (mask & (1u << i)) != 0;
            const bool before = (was & (1u << i)) != 0;
            if (on == before)
                continue;

            const TraitExt::ConditionalTrait& ct = (*pList)[i];
            if (on)
            {
                Debug::Log("[TraitExt] (unlock) %s @%p: '%s' satisfied\n",
                    pType->ID, pThis, ct.Def->Name.c_str());
                ApplyOneTrait(pThis, ct.Def, !ct.CloneID.empty());
                if (!ct.CloneID.empty())
                {
                    TraitExt::VariantArt::Assign(pThis, ct.CloneID.c_str());
                    TraitExt::VariantWeapon::Assign(pThis, ct.CloneID.c_str());
                    tookLook = true;
                }
            }
            else
            {
                Debug::Log("[TraitExt] (unlock) %s @%p: '%s' no longer satisfied\n",
                    pType->ID, pThis, ct.Def->Name.c_str());
            }
        }

        // Gate closed and nothing else claims the look: back to the base type.
        // The weapon must follow the SAME condition — forgetting it
        // unconditionally here threw away the variant weapon that was just
        // assigned a few lines above, so an unlocked unit changed its image
        // and kept firing its old primary.
        if (!tookLook && mask == 0)
        {
            TraitExt::VariantArt::Forget(pThis);
            TraitExt::VariantWeapon::Forget(pThis);
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

    // Periodic LOOK re-roll, if the pool asked for one. Appearance only: a
    // unit that silently changed armour or veterancy mid-fight would be a
    // gameplay surprise, and those are synced state. This is purely what the
    // draw hook reads, so an unsynced local RNG is correct here.
    if (g_Seen.count(pThis))
    {
        TechnoTypeClass* const pT = pThis->GetTechnoType();
        const TraitExt::InstancePool* const pP =
            pT ? TraitExt::InstanceRandom::Find(pT->ID) : nullptr;

        if (pP && pP->RerollMin > 0 && !pP->CloneIDs.empty())
        {
            // Draw a fresh delay each time, so units that spawned together do
            // not morph in lockstep.
            auto nextDelay = [&]() -> int
            {
                const int span = pP->RerollMax - pP->RerollMin + 1;
                return pP->RerollMin + (span > 1
                    ? static_cast<int>(CosmeticRand() % static_cast<std::uint32_t>(span))
                    : 0);
            };

            const int now = Unsorted::CurrentFrame;
            const auto nit = g_NextReroll.find(pThis);
            if (nit == g_NextReroll.end())
            {
                g_NextReroll[pThis] = now + nextDelay();
            }
            else if (now >= nit->second)
            {
                nit->second = now + nextDelay();

                // Only pick among pooled traits that actually have a look.
                const int n = static_cast<int>(pP->CloneIDs.size());
                for (int tries = 0; tries < n; ++tries)
                {
                    const int pick = static_cast<int>(CosmeticRand() % static_cast<std::uint32_t>(n));
                    if (!pP->CloneIDs[pick].empty())
                    {
                        // Morphing must move the WEAPON too, or a unit that
                        // looks like a Prism Tank keeps firing shells.
                        TraitExt::VariantArt::Assign(pThis, pP->CloneIDs[pick].c_str());
                        TraitExt::VariantWeapon::Assign(pThis, pP->CloneIDs[pick].c_str());
                        break;
                    }
                }
            }
        }
        EvaluateConditionals(pThis);
        return 0;
    }

    TechnoTypeClass* const pType = pThis->GetTechnoType();
    if (!pType)
        return 0;

    EvaluateConditionals(pThis);

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

        // A variant clone carries this trait's TYPE-level keys (Image, Cost,
        // Armor...), so the per-instance pass must not complain about them.
        const bool hasClone = chosen < static_cast<int>(pPool->CloneIDs.size())
            && !pPool->CloneIDs[chosen].empty();

        Debug::Log("[TraitExt] (instance) %s @%p drew '%s'\n",
            pType->ID, pThis, pDef->Name.c_str());
        ApplyOneTrait(pThis, pDef, hasClone);

        if (hasClone)
        {
            // BOTH, or a weapons-only variant is drawn and never armed. This is
            // the unit's FIRST draw; the morph re-roll and the gated path
            // already did both, so this was the one path still art-only.
            TraitExt::VariantArt::Assign(pThis, pPool->CloneIDs[chosen].c_str());
            TraitExt::VariantWeapon::Assign(pThis, pPool->CloneIDs[chosen].c_str());
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
    // clone type -> must aim with the body instead of a turret.
    std::unordered_map<void*, bool> g_BodyFacing;
    std::unordered_set<std::string> g_BodyFacingIDs;
    bool g_VariantEnabled = true;

    // Exactly one unit may be mid-swap at a time; draws are sequential, so the
    // next draw (or the logic tick) restores the previous one. Keeping the
    // window this small is what stops house counts / build limits / prereqs
    // from ever seeing a clone.
    UnitClass* g_Swapped = nullptr;
    UnitTypeClass* g_SwappedOriginal = nullptr;
    int g_SwappedTurretNumber = 0;
    bool g_SwappedTurretFixed = false;

    void RestorePending()
    {
        if (g_Swapped && g_SwappedOriginal)
        {
            g_Swapped->Type = g_SwappedOriginal;
            if (g_SwappedTurretFixed)
                g_Swapped->CurrentTurretNumber = g_SwappedTurretNumber;
        }
        g_Swapped = nullptr;
        g_SwappedOriginal = nullptr;
        g_SwappedTurretFixed = false;
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

            auto* const pClone = UnitTypeClass::Find(cloneID);
            if (pClone)
            {
                g_Variant[pThis] = pClone;
            }
            else
            {
                // Logged once per clone id: means the synthesised section never
                // became a real type (not registered in a list the engine
                // reads, or written too late to be parsed).
                static std::unordered_set<std::string> s_warned;
                if (s_warned.insert(cloneID).second)
                {
                    Debug::Log("[TraitExt] WARN variant type '%s' does not exist as a "
                        "UnitType - the clone section was not turned into a type\n", cloneID);
                }
            }
        }

        void Forget(::TechnoClass* pThis)
        {
            if (g_Swapped == static_cast<void*>(pThis))
                RestorePending();
            g_Variant.erase(pThis);
        }

        void SetForceBodyFacing(const std::string& cloneID, bool on)
        {
            if (on)
                g_BodyFacingIDs.insert(cloneID);
            else
                g_BodyFacingIDs.erase(cloneID);
        }

        bool NeedsBodyFacing(::TechnoClass* pThis)
        {
            if (!pThis || g_BodyFacingIDs.empty())
                return false;
            const auto it = g_Variant.find(pThis);
            if (it == g_Variant.end() || !it->second)
                return false;
            return g_BodyFacingIDs.count(it->second->ID) != 0;
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
            ApplyOneTrait(pThis, pDef, false);
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
// Hooked at UnitClass::DrawAsVXL (0x73B470) and DrawAsSHP (0x73C5F0) — YRpp:
// "main drawing functions - Draw() calls one of these". These are the top-level
// per-type draws; hooking the lower-level UnitClass::DrawObject (0x73B140)
// instead was verified in game to be TOO LATE: the swap happened but the art had
// already been resolved, so the unit still rendered as its base type.
//
// Boundaries verified — both start with `81 EC imm32` (6 bytes), so the steal
// MUST be 0x6; a 5-byte steal would split that instruction. Neither address is
// hooked by any framework. ECX = UnitClass*.
//
// Image lives on ObjectTypeClass (shared by the whole type), so a per-unit look
// needs a genuinely different type. Type points at the synthesised clone only
// for the draw and is restored right after, which is why gameplay never sees it.
// The restore runs at the next draw and again on the logic tick, so a missed one
// self-corrects within a frame instead of persisting.
namespace
{
    void SwapForDraw(UnitClass* pThis)
    {
        RestorePending();

        if (!g_VariantEnabled || g_Variant.empty() || !pThis)
            return;

        const auto it = g_Variant.find(pThis);
        if (it == g_Variant.end() || !it->second || pThis->Type == it->second)
            return;

        static bool s_loggedFirst = false;
        if (!s_loggedFirst)
        {
            s_loggedFirst = true;
            Debug::Log("[TraitExt] variant art ACTIVE: first draw swap %s -> %s\n",
                pThis->Type ? pThis->Type->ID : "?", it->second->ID);
        }

        // NEVER swap in a type with no loaded voxel. DrawAsVXL divides through
        // [HVA+8], so a null HVA is not a missing-art no-op - it is an integer
        // divide fault that takes the game down on the unit's first frame.
        // A variant clone whose Image resolves to nothing (or to its own
        // synthesised ID, which is never a real art section) lands here.
        // Refusing the swap costs the variant its look and keeps the match
        // alive, which is the right trade every time.
        if (!it->second->MainVoxel.HVA)
        {
            static std::unordered_set<const void*> s_warned;
            if (s_warned.insert(it->second).second)
                Debug::Log("[TraitExt] WARN variant '%s' has no loaded voxel art "
                    "(Image= names a section with no .vxl/.hva) - refusing the draw "
                    "swap; the unit keeps its own look. Drawing it would divide by a "
                    "null HVA and crash.\n", it->second->ID);
            return;
        }

        g_Swapped = pThis;
        g_SwappedOriginal = pThis->Type;
        pThis->Type = it->second;

        // CurrentTurretNumber is a PER-INSTANCE index into the type's turret
        // voxels, and it was set while this unit was still its base type. Borrow
        // a multi-turret art donor (the Prism Tank is TurretCount=4) and that
        // index is meaningless or -1, which silently kills the turret draw on
        // BOTH paths: Phobos returns early for idx<0, and Antares indexes
        // ChargerTurrets[-1]. PayloadExt documents the same failure
        // ("invisible until the unit first fires"), so clamp it into range for
        // the borrowed type and put the original back afterwards.
        const int turrets = it->second->TurretCount;
        if (turrets > 0
            && (pThis->CurrentTurretNumber < 0 || pThis->CurrentTurretNumber >= turrets))
        {
            g_SwappedTurretNumber = pThis->CurrentTurretNumber;
            g_SwappedTurretFixed = true;
            pThis->CurrentTurretNumber = 0;

            static bool s_loggedTurret = false;
            if (!s_loggedTurret)
            {
                s_loggedTurret = true;
                Debug::Log("[TraitExt] variant turret index repaired: %d -> 0 "
                    "(borrowed type %s has TurretCount=%d)\n",
                    g_SwappedTurretNumber, it->second->ID, turrets);
            }
        }
    }
}

DEFINE_HOOK(0x73B470, UnitClass_DrawAsVXL_VariantArt, 0x6)
{
    GET(UnitClass*, pThis, ECX);
    SwapForDraw(pThis);
    return 0;
}

DEFINE_HOOK(0x73C5F0, UnitClass_DrawAsSHP_VariantArt, 0x6)
{
    GET(UnitClass*, pThis, ECX);
    SwapForDraw(pThis);
    return 0;
}

// ForceBodyFacing — make a variant aim with its hull instead of a turret.
//
// A variant only LOOKS turretless: gameplay still sees the base type (we swap
// Type for the draw only), so a Tank-Destroyer-looking Grizzly still fires in
// any direction. This restores the expected behaviour — rotate the hull first,
// the way TNKD does and MTNK does not.
//
// 0x740FD0 = UnitClass::GetFireError, UNHOOKED by any framework. Boundary
// verified: 83 EC 0C / 53 / 55 is exactly 5 bytes. ECX = this, stack +0x4 =
// target.
//
// Returning a value needs a flow-replacing jump, and every epilogue in this
// function overwrites EAX with its own constant. Hooking the ENTRY sidesteps
// that: nothing has been pushed yet, so ESP already points at the return
// address and a bare `ret 0xc` at 0x74102A returns correctly with no stack
// fixup at all.
//
// The turret facing (SecondaryFacing) is already aimed at the target, so it
// doubles as "the direction we want" without recomputing target geometry.
DEFINE_HOOK(0x740FD0, UnitClass_GetFireError_ForceBodyFacing, 0x5)
{
    enum { RetFacing = 0x74102A };   // bare `ret 0xc`

    GET(UnitClass*, pThis, ECX);
    GET_STACK(AbstractClass*, pTarget, 0x4);

    if (!pThis || !pTarget || !TraitExt::VariantArt::NeedsBodyFacing(pThis))
        return 0;

    // Aim at the TARGET, not at the turret.
    //
    // Comparing the hull against SecondaryFacing looked right but failed for the
    // first orders after a unit had been idle: a parked tank has hull and turret
    // already aligned, so the difference was ~0 and the check passed even though
    // the target was off to one side. Once the turret began tracking, the two
    // diverged and it started working — exactly the "only the first few orders"
    // symptom. The target's own direction has no such warm-up.
    const CoordStruct here = pThis->GetCoords();
    const CoordStruct there = pTarget->GetCoords();

    const double dx = static_cast<double>(there.X - here.X);
    const double dy = static_cast<double>(there.Y - here.Y);
    if (dx == 0.0 && dy == 0.0)
        return 0;

    // Game Y grows southward, hence the negation before atan2.
    const DirStruct want { std::atan2(-dy, dx) };

    const int body = static_cast<int>(pThis->PrimaryFacing.Current().Raw);
    const int aim = static_cast<int>(want.Raw);

    int diff = body - aim;
    if (diff < 0) diff = -diff;
    if (diff > 32768) diff = 65536 - diff;      // wrap the short way round

    // ~11 degrees of slack, matching how forgiving vanilla hull aiming feels.
    if (diff <= 2048)
        return 0;

    // Point the hull at the target so it actually turns, then report FACING so
    // the engine holds fire until it has.
    pThis->PrimaryFacing.SetDesired(want);

    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        Debug::Log("[TraitExt] ForceBodyFacing active: holding fire while hull turns "
            "(body=%d aim=%d diff=%d)\n", body, aim, diff);
    }

    R->EAX(static_cast<int>(FireError::FACING));
    return RetFacing;
}

// Per-unit WEAPONS.
//
// 0x70E140 = TechnoClass::GetWeapon(int). Crucially this takes the INSTANCE
// (ECX), not the type — which is exactly why weapons can vary per unit while
// Cost and Armor cannot: those are only ever asked of the shared type.
//
// Boundary verified: 56 / 57 / 8B 7C 24 0C is exactly 6 bytes. In the RELEASE
// channel nothing hooks it (only Phobos PR#1168, which is not in this build).
//
// Returning a value needs a flow-replacing jump, and the epilogues pop first,
// so we hook the ENTRY: nothing is pushed yet, ESP already points at the return
// address, and 0x70E151 is a bare `ret 4`. Same trick as GetFireError.
//
// SYNC: the variant that selects the weapon was drawn from ScenarioClass::Random
// (the synced generator), so every client resolves the same weapon for the same
// unit. This is game logic, not cosmetics — it must not use the local RNG.
DEFINE_HOOK(0x70E140, TechnoClass_GetWeapon_Variant, 0x6)
{
    enum { RetWeapon = 0x70E151 };   // bare `ret 4`

    // Proof-of-life BEFORE any gating. "variant weapon ACTIVE" never appearing
    // could mean the map is empty, the lookup missed, the slot was empty — or
    // that the game simply does not route weapon selection through this
    // address, in which case the whole seam is wrong. Those need different
    // fixes, and only an ungated entry log tells them apart.
    {
        static bool s_reached = false;
        if (!s_reached)
        {
            s_reached = true;
            Debug::Log("[TraitExt] GetWeapon hook REACHED (0x70E140 is on a live path)\n");
        }
    }

    if (!TraitExt::VariantWeapon::Enabled())
        return 0;

    if (!TraitExt::VariantWeapon::Any())
    {
        static bool s_empty = false;
        if (!s_empty)
        {
            s_empty = true;
            Debug::Log("[TraitExt] GetWeapon: no unit has a variant weapon assigned yet\n");
        }
        return 0;
    }

    GET(TechnoClass*, pThis, ECX);
    GET_STACK(int, index, 0x4);

    if (!pThis || index < 0 || index >= TechnoTypeClass::MaxWeapons)
        return 0;

    TechnoTypeClass* const pVariant = TraitExt::VariantWeapon::For(pThis);
    if (!pVariant)
        return 0;

    // Keyed PER VARIANT TYPE, not a single global bool. A one-shot global
    // probe only ever reports the first type that happens to hit it: MTNK
    // matched first and made every IFV match invisible, so the log could not
    // answer "did the FV variant engage" at all.
    {
        static std::unordered_set<const void*> s_matched;
        if (s_matched.insert(pVariant).second)
            Debug::Log("[TraitExt] GetWeapon: '%s' matched a variant, slot %d\n",
                pVariant->ID, index);
    }

    // ForceWeapon: does this variant's own gun beat the slot the engine asked
    // for? It matters because a Gunner unit picks its slot by PASSENGER — so an
    // IFV carrying a variant weapon reverts to the passenger's gun the instant
    // anyone boards, since that slot is one the clone merely INHERITED rather
    // than one the trait declared. With ForceWeapon=yes we answer from the
    // variant's own declared slot instead; with it off, the passenger wins and
    // the vanilla gunner behaviour is untouched.
    int use = index;
    if (TraitExt::VariantWeapon::Force(pVariant->ID)
        && !TraitExt::VariantWeapon::SlotDeclared(pVariant->ID, index))
    {
        const int own = TraitExt::VariantWeapon::FirstDeclaredSlot(pVariant->ID);
        if (own >= 0 && own != index)
        {
            use = own;
            static std::unordered_set<const void*> s_forced;
            if (s_forced.insert(pVariant).second)
                Debug::Log("[TraitExt] GetWeapon: '%s' ForceWeapon=yes, answering slot "
                    "%d from its own declared slot %d\n", pVariant->ID, index, own);
        }
    }

    // Mirror vanilla's elite selection, just off the variant's arrays.
    WeaponStruct* const pWeapon = pThis->Veterancy.IsElite()
        ? &pVariant->EliteWeapon[use]
        : &pVariant->Weapon[use];

    if (!pWeapon->WeaponType)
    {
        // The lookup succeeds and then the slot is empty, which is where this
        // has been dying. Dump what the variant type ACTUALLY parsed, once per
        // type, rather than reasoning about what it should contain.
        static std::unordered_set<const void*> s_dumped;
        if (s_dumped.insert(pVariant).second)
        {
            Debug::Log("[TraitExt] GetWeapon: variant '%s' slot %d is EMPTY "
                "(asked for %d, elite=%d). WeaponCount=%d TurretCount=%d\n",
                pVariant->ID, use, index, pThis->Veterancy.IsElite() ? 1 : 0,
                pVariant->WeaponCount, pVariant->TurretCount);
            for (int i = 0; i < 4; ++i)
            {
                Debug::Log("[TraitExt]     %s: Weapon[%d]=%s EliteWeapon[%d]=%s\n",
                    pVariant->ID, i,
                    pVariant->Weapon[i].WeaponType
                        ? pVariant->Weapon[i].WeaponType->ID : "(null)",
                    i,
                    pVariant->EliteWeapon[i].WeaponType
                        ? pVariant->EliteWeapon[i].WeaponType->ID : "(null)");
            }
            // And the base, for comparison: if the BASE has it and the clone
            // does not, the clone's own keys never parsed.
            if (TechnoTypeClass* const pBase = pThis->GetTechnoType())
                Debug::Log("[TraitExt]     base %s: Weapon[0]=%s WeaponCount=%d\n",
                    pBase->ID,
                    pBase->Weapon[0].WeaponType
                        ? pBase->Weapon[0].WeaponType->ID : "(null)",
                    pBase->WeaponCount);
        }
        return 0;   // variant leaves this slot empty: fall back to vanilla
    }

    {
        static std::unordered_set<const void*> s_active;
        if (s_active.insert(pVariant).second)
            Debug::Log("[TraitExt] variant weapon ACTIVE: slot %d from '%s' -> %s\n",
                index, pVariant->ID, pWeapon->WeaponType->ID);
    }

    R->EAX(pWeapon);
    return RetWeapon;
}

DEFINE_HOOK(0x6F4500, TechnoClass_DTOR_InstanceRandom, 0x5)
{
    GET(TechnoClass*, pThis, ECX);
    if (pThis)
    {
        g_Seen.erase(pThis);
        g_NextReroll.erase(pThis);
        g_NextCondCheck.erase(pThis);
        g_CondActive.erase(pThis);
        TraitExt::VariantArt::Forget(pThis);
        TraitExt::VariantWeapon::Forget(pThis);
    }
    return 0;
}
