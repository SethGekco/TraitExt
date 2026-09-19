#pragma once

#include <string>
#include <vector>
#include <utility>

class CCINIClass;
// Both must be at GLOBAL scope: declaring them inside namespace TraitExt would
// create distinct TraitExt::TechnoClass / TraitExt::TechnoTypeClass types.
class TechnoClass;
class TechnoTypeClass;

namespace TraitExt
{
    // Per-key fold modes. A Trait declares its default via Merge=; every key in
    // that Trait folds with that mode unless the key carries a sigil (+ or *).
    enum class MergeMode
    {
        Override,   // absolute: discards prior contributors (default)
        Keep,       // first-wins: applies only if nothing set the key yet
        Add,        // numeric +=
        Multiply,   // numeric *=
        Min,        // numeric clamp low
        Max,        // numeric clamp high
        Average,    // mean of all Average-mode contributors for that key
        Append,     // CSV list: add items not already present
        Remove      // CSV list: drop the listed items
    };

    struct TraitDef
    {
        std::string Name;
        // Index in [TraitTypes]. Inverse (AppliesTo=) assignment folds in this
        // order — declaration order is meaningful to the author, alphabetical
        // order is not, and fold order changes the result.
        int Order = 0;
        MergeMode Mode = MergeMode::Override;
        // Traits this trait is composed of; expanded before its own entries so
        // the trait's own keys act as the more specific override.
        std::vector<std::string> Composes;
        // Inverse assignment: the trait names its own targets, so a trait can be
        // applied without editing the target section at all.
        std::vector<std::string> AppliesTo;
        // Inverse random-pool membership: "this trait is one option in these
        // targets' random pools". Same motivation as AppliesTo — keeps all
        // trait config in one place instead of scattered across unit sections.
        std::vector<std::string> RandomPoolFor;
        // Optional scope for the pools this trait joins ("Type" or "Instance").
        std::string RandomScope;
        // Donor TechnoTypes to copy wholesale. This is the "apply another unit
        // to this one" verb: instead of hand-writing every key, name a real (or
        // dummy) type and take what it defines. Expanded into Entries at load,
        // BEFORE the trait's own keys, so a hand-written key still wins.
        std::vector<std::string> InheritFrom;
        // Narrow the copy. Both accept key names and FAMILY names (Art, Turret,
        // Weapons, Armor, Movement, Economy, Identity).
        std::vector<std::string> InheritOnly;
        std::vector<std::string> InheritExcept;
        // Per-trait override of [TraitExt] InheritFamilyCoherence. Empty = use
        // the global. "no" hands the whole result to the data: nothing is
        // switched off on your behalf.
        std::string InheritCoherence;
        // Mixed turrets: take the turret (and barrel) art from a DIFFERENT unit
        // than the body. Resolved to an art name at load.
        std::string TurretFrom;
        // Per-trait way to set the pool's re-roll cadence (frames).
        std::string RerollInterval;
        // Prerequisite gate: the unit's OWNER must have all of these present
        // for the trait to apply. Checked per unit, because type data is shared
        // by every house — a type-level unlock would arm the enemy too.
        std::vector<std::string> Requirement;
        // Proximity gate: the trait applies only while one of these types is
        // standing within NearRange CELLS of the unit. Like Requirement it is
        // judged per unit at runtime — but unlike Requirement it depends on
        // where the unit currently is, so it opens and closes as it drives.
        std::vector<std::string> NearTypes;
        int NearRange = 0;
        // Whose objects count: Owner (default), Ally, Enemy or Any.
        std::string NearOwner;
        // Author order is preserved: fold order is declaration order.
        std::vector<std::pair<std::string, std::string>> Entries;
        // Per-key mode overrides from "<Key>.Merge=" inside the trait section.
        std::vector<std::pair<std::string, MergeMode>> KeyModes;
    };

    // Load-time trait resolution. Runs at the RulesClass::Read_File entry seam
    // (0x668BF0), before the vanilla section readers, so every value written
    // here is what the game parses. Non-numeric keys always Override; numeric
    // folds need an INI-visible base value (see docs/TESTING.md).
    namespace Engine
    {
        void ProcessINI(CCINIClass* pINI);
    }

    // ---- Per-instance random (TraitsRandomScope=Instance) -------------------
    // Type scope resolves once at load and every unit of the type shares the
    // draw for the whole match. Instance scope defers the draw to each unit's
    // first logic tick, so units of the same type differ from one another.
    //
    // Only INSTANCE-level fields can vary this way (Health, Veterancy, Ammo) —
    // Image/Cost/Armor live on the shared TechnoTypeClass and are therefore
    // Type-scope only. The engine warns rather than silently doing nothing.
    struct InstancePool
    {
        std::vector<const TraitDef*> Traits;
        // Parallel to Traits. Image is TYPE-level, so a per-unit look needs a
        // real second type: for any pooled trait that sets Image we synthesise
        // a clone type ($Inherits the target, differing only in Image) and swap
        // the unit's Type pointer for the duration of its draw. Empty when the
        // trait sets no Image.
        std::vector<std::string> CloneIDs;
        int CountMin = 1;
        int CountMax = 1;
        // >0: re-roll the unit's LOOK on this cadence, so a unit visibly shifts
        // between variants mid-match. A RANGE, re-drawn after each morph, so a
        // group of units does not pulse in lockstep. Appearance only — see the
        // note in Hooks.InstanceRandom.cpp for why stats are excluded.
        int RerollMin = 0;
        int RerollMax = 0;
    };

    // Per-unit cosmetic variant art. Rendering only: the Type pointer is swapped
    // just around UnitClass::DrawObject and restored, so house counts, build
    // limits and prerequisites never observe the clone.
    namespace VariantArt
    {
        // A variant may require the unit to aim with its BODY rather than a
        // turret — the point of a turretless-looking variant like the Tank
        // Destroyer, which in vanilla has to rotate to fire.
        void SetForceBodyFacing(const std::string& cloneID, bool on);
        bool NeedsBodyFacing(::TechnoClass* pThis);

        void Assign(::TechnoClass* pThis, const char* cloneID);
        void Forget(::TechnoClass* pThis);
        bool Enabled();
        void SetEnabled(bool on);
        bool Any();
    }

    // ---- Cameo preservation --------------------------------------------
    // In YR the cameo is read from the art section named by ImageFile (see
    // Antares Ext/TechnoType/Body.cpp: CameoPCX.Read(&INI_Art, pThis->ImageFile,
    // ...)). So changing Image= inevitably drags the cameo along with it, which
    // is almost never what a random-art trait wants. When enabled (the default),
    // TraitExt restores each affected type's cameo to the one its ORIGINAL art
    // would have used, after types finish loading.
    // Mixed turrets. YR derives BOTH body and turret art from one ImageFile
    // ("<art>.vxl" / "<art>TUR.vxl"), and Antares builds the same names, so no
    // data key can separate them. Instead the synthesised clone LOADS ITS OWN
    // turret voxels from the donor's art name — the clone owns them, so there is
    // no pointer aliasing and nothing can be freed twice.
    // Per-unit WEAPONS. The weapon is fetched through TechnoClass::GetWeapon,
    // which takes the INSTANCE — so unlike Cost or Armor it can be answered
    // per unit by handing back the variant clone's weapon instead.
    bool IsWeaponKey(const char* key);

    namespace VariantWeapon
    {
        void MarkClone(const std::string& cloneID);   // this clone overrides weapons
        bool CloneHasWeapon(const std::string& cloneID);
        void Assign(::TechnoClass* pThis, const char* cloneID);
        void Forget(::TechnoClass* pThis);
        bool Any();
        bool Enabled();
        void SetEnabled(bool on);
        ::TechnoTypeClass* For(::TechnoClass* pThis);
    }

    namespace MixedTurret
    {
        void Remember(const std::string& cloneID, const std::string& turretArt);
        void Apply();       // once types (and their art) exist
        bool Enabled();
        void SetEnabled(bool on);
    }

    namespace CameoFix
    {
        // targetID -> the art section it used before a trait changed Image.
        void Remember(const std::string& targetID, const std::string& originalArt);
        void Apply();   // called once types are loaded
        bool Enabled();
        void SetEnabled(bool on);
    }

    // ---- Spy-triggered traits (forced inheritance on infiltration) ---------
    // A building type may declare SpyTraits=; when a spy infiltrates one of
    // those buildings, the traits are forced onto THAT BUILDING INSTANCE.
    // Instance scope is deliberate: BuildingTypeClass is shared by every house,
    // so type-level application would buff the victim's whole faction (and
    // everyone else's) rather than the infiltrated structure.
    // Prerequisite-gated traits. Registered at load, evaluated per unit at
    // runtime so each house is judged on its own buildings, and re-checked so a
    // unit updates when the building goes up or is destroyed.
    struct ConditionalTrait
    {
        enum class Whose { Owner, Ally, Enemy, Any };

        const TraitDef* Def = nullptr;
        std::vector<std::string> Requirement;
        std::vector<std::string> NearTypes;
        int NearRange = 0;                  // cells; 0 disables the gate
        Whose NearOwner = Whose::Owner;
        std::string CloneID;        // empty unless the trait changes appearance
    };

    namespace Conditional
    {
        const std::vector<ConditionalTrait>* Find(const char* typeID);
        bool Any();
        void Register(const std::string& targetID, const ConditionalTrait& ct);
        void Clear();

        // Is something this trait watches for currently standing close enough?
        // Answered from an index rebuilt at most once per check cadence, so the
        // cost is one walk of TechnoClass::Array rather than one per unit.
        bool NearMeets(::TechnoClass* pThis, const ConditionalTrait& ct);
        bool AnyNear();     // true if any registered trait uses a proximity gate
    }

    namespace SpyTraits
    {
        const std::vector<const TraitDef*>* Find(const char* buildingTypeID);
        bool Any();
    }

    // Applies instance-capable keys (Health/Strength, Veterancy, Ammo) to one
    // object. Shared by per-instance random and by spy infiltration.
    void ApplyInstanceTraits(::TechnoClass* pThis,
        const std::vector<const TraitDef*>& traits, const char* reason);

    namespace InstanceRandom
    {
        // Registered at load; looked up by TechnoType ID at runtime.
        const InstancePool* Find(const char* typeID);
        bool Any();
    }
}
