#pragma once

#include <string>
#include <vector>
#include <utility>

class CCINIClass;

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
        Average     // mean of all Average-mode contributors for that key
    };

    struct TraitDef
    {
        std::string Name;
        MergeMode Mode = MergeMode::Override;
        // Author order is preserved: fold order is declaration order.
        std::vector<std::pair<std::string, std::string>> Entries;
    };

    // Load-time trait resolution. Runs at the RulesClass::Read_File entry seam
    // (0x668BF0), before the vanilla section readers, so every value written
    // here is what the game parses. Non-numeric keys always Override; numeric
    // folds need an INI-visible base value (see docs/TESTING.md).
    namespace Engine
    {
        void ProcessINI(CCINIClass* pINI);
    }
}
