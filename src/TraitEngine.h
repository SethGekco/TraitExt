#pragma once

#include <string>
#include <vector>
#include <utility>

class CCINIClass;
class TechnoClass;   // must be at GLOBAL scope: declaring it inside namespace
                     // TraitExt would create a distinct TraitExt::TechnoClass

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
        int CountMin = 1;
        int CountMax = 1;
    };

    // ---- Cameo preservation --------------------------------------------
    // In YR the cameo is read from the art section named by ImageFile (see
    // Antares Ext/TechnoType/Body.cpp: CameoPCX.Read(&INI_Art, pThis->ImageFile,
    // ...)). So changing Image= inevitably drags the cameo along with it, which
    // is almost never what a random-art trait wants. When enabled (the default),
    // TraitExt restores each affected type's cameo to the one its ORIGINAL art
    // would have used, after types finish loading.
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
