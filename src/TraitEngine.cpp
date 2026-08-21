#include "TraitEngine.h"

#include <Phobos.h>
#include <CCINIClass.h>
#include <Utilities/Debug.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace TraitExt
{
    namespace
    {
        constexpr const char* SectTraitTypes = "TraitTypes";
        constexpr const char* SectConfig = "TraitExt";
        constexpr const char* KeyApplied = "$Applied";

        // Sections whose listed entries are candidate trait targets.
        const char* const TargetLists[] = {
            "VehicleTypes", "InfantryTypes", "BuildingTypes", "AircraftTypes"
        };

        std::string Trim(const std::string& s)
        {
            size_t b = 0, e = s.size();
            while (b < e && static_cast<unsigned char>(s[b]) <= ' ') ++b;
            while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') --e;
            return s.substr(b, e - b);
        }

        std::string ReadKey(CCINIClass* pINI, const char* section, const char* key,
            const char* def = "")
        {
            char buffer[0x400];
            // Routes through Phobos's lazy $Inherits fallback (0x528BAC), so a
            // target's inherited value is visible here as its base.
            pINI->ReadString(section, key, def, buffer, sizeof(buffer));
            return Trim(std::string(buffer));
        }

        std::vector<std::string> SplitCSV(const std::string& s)
        {
            std::vector<std::string> out;
            size_t start = 0;
            while (start <= s.size())
            {
                const size_t comma = s.find(',', start);
                const size_t end = (comma == std::string::npos) ? s.size() : comma;
                std::string item = Trim(s.substr(start, end - start));
                if (!item.empty())
                    out.push_back(item);
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
            return out;
        }

        bool ParseNumber(const std::string& s, double& out)
        {
            if (s.empty())
                return false;
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            if (!end || end == s.c_str())
                return false;
            while (*end && static_cast<unsigned char>(*end) <= ' ')
                ++end;
            if (*end)
                return false;
            out = v;
            return true;
        }

        std::string FormatNumber(double v)
        {
            char buffer[64];
            if (std::fabs(v - std::floor(v + 0.5)) < 1e-9 && std::fabs(v) < 1e15)
            {
                std::snprintf(buffer, sizeof(buffer), "%lld",
                    static_cast<long long>(std::floor(v + 0.5)));
                return std::string(buffer);
            }
            std::snprintf(buffer, sizeof(buffer), "%.6f", v);
            std::string s(buffer);
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
            return s;
        }

        MergeMode ParseMode(const std::string& s, MergeMode fallback)
        {
            std::string v;
            v.reserve(s.size());
            for (char c : s)
                v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            if (v == "override" || v == "replace") return MergeMode::Override;
            if (v == "keep" || v == "first")       return MergeMode::Keep;
            if (v == "add" || v == "addition")     return MergeMode::Add;
            if (v == "multiply" || v == "mult")    return MergeMode::Multiply;
            if (v == "min")                        return MergeMode::Min;
            if (v == "max")                        return MergeMode::Max;
            if (v == "average" || v == "mean")     return MergeMode::Average;
            return fallback;
        }

        const char* ModeName(MergeMode m)
        {
            switch (m)
            {
            case MergeMode::Keep:     return "Keep";
            case MergeMode::Add:      return "Add";
            case MergeMode::Multiply: return "Multiply";
            case MergeMode::Min:      return "Min";
            case MergeMode::Max:      return "Max";
            case MergeMode::Average:  return "Average";
            default:                  return "Override";
            }
        }

        // A value may carry a per-key sigil that overrides the Trait's Merge=.
        // "+300" => Add, "*0.75" => Multiply. A bare "-5" stays a plain negative
        // number (Override/whatever the trait mode is), because "-" is ambiguous
        // with negative literals.
        MergeMode ModeForValue(const std::string& raw, MergeMode traitMode, std::string& valueOut)
        {
            if (!raw.empty() && (raw[0] == '+' || raw[0] == '*'))
            {
                const std::string rest = Trim(raw.substr(1));
                double probe = 0.0;
                if (ParseNumber(rest, probe))
                {
                    valueOut = rest;
                    return (raw[0] == '+') ? MergeMode::Add : MergeMode::Multiply;
                }
            }
            valueOut = raw;
            return traitMode;
        }

        std::uint32_t HashString(const std::string& s)
        {
            std::uint32_t h = 2166136261u;
            for (char c : s)
            {
                h ^= static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
                h *= 16777619u;
            }
            return h;
        }

        std::uint32_t NextRand(std::uint32_t& state)
        {
            // xorshift32 — deterministic and identical on every client.
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }

        struct Contribution
        {
            MergeMode Mode;
            std::string Value;
            const TraitDef* Source;
        };

        // Fold an ordered contributor list for one key onto a base value.
        std::string FoldKey(const std::string& base, const std::vector<Contribution>& contribs,
            const char* target, const char* key)
        {
            std::string cur = base;

            // Average reduces over the whole set, not pairwise, so precompute it
            // and apply at the position of the first Average contributor.
            double avgSum = 0.0;
            int avgCount = 0;
            for (const auto& c : contribs)
            {
                double v = 0.0;
                if (c.Mode == MergeMode::Average && ParseNumber(c.Value, v))
                {
                    avgSum += v;
                    ++avgCount;
                }
            }
            bool avgApplied = false;

            for (const auto& c : contribs)
            {
                if (c.Mode == MergeMode::Average)
                {
                    if (!avgApplied && avgCount > 0)
                    {
                        cur = FormatNumber(avgSum / avgCount);
                        avgApplied = true;
                    }
                    continue;
                }

                if (c.Mode == MergeMode::Override)
                {
                    cur = c.Value;
                    continue;
                }

                if (c.Mode == MergeMode::Keep)
                {
                    if (cur.empty())
                        cur = c.Value;
                    continue;
                }

                // Numeric modes need both sides numeric; anything else Overrides,
                // which is what makes "Radar=yes" behave sanely under Merge=Add.
                double lhs = 0.0, rhs = 0.0;
                const bool lhsNum = ParseNumber(cur, lhs);
                const bool rhsNum = ParseNumber(c.Value, rhs);

                if (!rhsNum)
                {
                    cur = c.Value;
                    continue;
                }

                if (!lhsNum)
                {
                    if (!cur.empty())
                    {
                        Debug::Log("[TraitExt]   WARN %s.%s: base '%s' is not numeric; "
                            "trait '%s' (%s) overrides instead of folding\n",
                            target, key, cur.c_str(), c.Source->Name.c_str(), ModeName(c.Mode));
                    }
                    else
                    {
                        Debug::Log("[TraitExt]   WARN %s.%s: no INI base value; trait '%s' (%s) "
                            "applied as Override (engine defaults are not INI-visible)\n",
                            target, key, c.Source->Name.c_str(), ModeName(c.Mode));
                    }
                    cur = c.Value;
                    continue;
                }

                switch (c.Mode)
                {
                case MergeMode::Add:      lhs += rhs; break;
                case MergeMode::Multiply: lhs *= rhs; break;
                case MergeMode::Min:      lhs = (std::min)(lhs, rhs); break;
                case MergeMode::Max:      lhs = (std::max)(lhs, rhs); break;
                default: break;
                }
                cur = FormatNumber(lhs);
            }

            return cur;
        }

        void ReadListSection(CCINIClass* pINI, const char* section, std::vector<std::string>& out)
        {
            const int count = pINI->GetKeyCount(section);
            for (int i = 0; i < count; ++i)
            {
                const char* keyName = pINI->GetKeyName(section, i);
                if (!keyName)
                    continue;
                const std::string value = ReadKey(pINI, section, keyName);
                if (!value.empty())
                    out.push_back(value);
            }
        }
    }

    void Engine::ProcessINI(CCINIClass* pINI)
    {
        if (!pINI)
            return;

        if (!pINI->GetSection(SectTraitTypes))
            return; // nothing to do for this INI

        // Read_File fires once per rules pass (rulesmd x3 via Init, game mode,
        // map). Folding Add/Multiply more than once would compound, so guard.
        if (!ReadKey(pINI, SectConfig, KeyApplied).empty())
            return;
        pINI->WriteString(SectConfig, KeyApplied, "1");

        // ---- 1. Trait registry -------------------------------------------
        std::vector<std::string> traitNames;
        ReadListSection(pINI, SectTraitTypes, traitNames);

        std::unordered_map<std::string, TraitDef> traits;
        for (const auto& name : traitNames)
        {
            if (!pINI->GetSection(name.c_str()))
            {
                Debug::Log("[TraitExt] WARN trait '%s' listed in [TraitTypes] has no section\n",
                    name.c_str());
                continue;
            }

            TraitDef def;
            def.Name = name;
            def.Mode = ParseMode(ReadKey(pINI, name.c_str(), "Merge", "Override"),
                MergeMode::Override);

            const int keyCount = pINI->GetKeyCount(name.c_str());
            for (int i = 0; i < keyCount; ++i)
            {
                const char* keyName = pINI->GetKeyName(name.c_str(), i);
                if (!keyName || !std::strcmp(keyName, "Merge"))
                    continue;
                if (keyName[0] == '$')
                    continue; // leave $Inherits and friends to Phobos
                def.Entries.emplace_back(keyName, ReadKey(pINI, name.c_str(), keyName));
            }

            Debug::Log("[TraitExt] trait '%s' Merge=%s keys=%d\n",
                name.c_str(), ModeName(def.Mode), static_cast<int>(def.Entries.size()));
            traits.emplace(name, std::move(def));
        }

        if (traits.empty())
            return;

        std::uint32_t globalSeed = 0x5EED1234u;
        {
            const std::string seedStr = ReadKey(pINI, SectConfig, "RandomSeed");
            double parsed = 0.0;
            if (ParseNumber(seedStr, parsed))
                globalSeed = static_cast<std::uint32_t>(static_cast<long long>(parsed));
        }

        // ---- 2. Targets ---------------------------------------------------
        std::vector<std::string> targets;
        for (const char* list : TargetLists)
            ReadListSection(pINI, list, targets);

        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

        int appliedTargets = 0;

        for (const auto& target : targets)
        {
            if (!pINI->GetSection(target.c_str()))
                continue;

            std::vector<std::string> wanted = SplitCSV(ReadKey(pINI, target.c_str(), "Traits"));

            // ---- 3. Random pool (deterministic; identical on every client) --
            const std::string poolStr = ReadKey(pINI, target.c_str(), "TraitsRandom");
            if (!poolStr.empty())
            {
                std::vector<std::string> pool = SplitCSV(poolStr);
                if (!pool.empty())
                {
                    int lo = 1, hi = 1;
                    const std::vector<std::string> countParts =
                        SplitCSV(ReadKey(pINI, target.c_str(), "TraitsRandomCount", "1,1"));
                    double tmp = 0.0;
                    if (countParts.size() >= 1 && ParseNumber(countParts[0], tmp))
                        lo = static_cast<int>(tmp);
                    hi = lo;
                    if (countParts.size() >= 2 && ParseNumber(countParts[1], tmp))
                        hi = static_cast<int>(tmp);

                    lo = (std::max)(0, lo);
                    hi = (std::max)(lo, hi);
                    hi = (std::min)(hi, static_cast<int>(pool.size()));

                    std::uint32_t state = globalSeed ^ HashString(target);
                    if (!state)
                        state = 0x9E3779B9u;

                    const int span = hi - lo + 1;
                    const int pick = lo + static_cast<int>(NextRand(state) % static_cast<std::uint32_t>(span));

                    // Partial Fisher-Yates: draw `pick` distinct entries.
                    for (int i = 0; i < pick && i < static_cast<int>(pool.size()); ++i)
                    {
                        const int remaining = static_cast<int>(pool.size()) - i;
                        const int j = i + static_cast<int>(NextRand(state) % static_cast<std::uint32_t>(remaining));
                        std::swap(pool[i], pool[j]);
                        wanted.push_back(pool[i]);
                    }

                    Debug::Log("[TraitExt] %s: random pool %d/%d picked\n",
                        target.c_str(), pick, static_cast<int>(pool.size()));
                }
            }

            if (wanted.empty())
                continue;

            // ---- 4. Blocks --------------------------------------------------
            const std::vector<std::string> blocked =
                SplitCSV(ReadKey(pINI, target.c_str(), "BlockTraits"));

            std::vector<const TraitDef*> resolved;
            std::vector<std::string> seen;
            for (const auto& name : wanted)
            {
                if (std::find(blocked.begin(), blocked.end(), name) != blocked.end())
                {
                    Debug::Log("[TraitExt] %s: trait '%s' blocked\n", target.c_str(), name.c_str());
                    continue;
                }
                if (std::find(seen.begin(), seen.end(), name) != seen.end())
                {
                    Debug::Log("[TraitExt] %s: trait '%s' listed twice, ignoring repeat\n",
                        target.c_str(), name.c_str());
                    continue;
                }
                const auto it = traits.find(name);
                if (it == traits.end())
                {
                    Debug::Log("[TraitExt] WARN %s: unknown trait '%s' (not in [TraitTypes])\n",
                        target.c_str(), name.c_str());
                    continue;
                }
                seen.push_back(name);
                resolved.push_back(&it->second);
            }

            if (resolved.empty())
                continue;

            // ---- 5. Collect contributions per key, preserving order ---------
            std::vector<std::string> keyOrder;
            std::unordered_map<std::string, std::vector<Contribution>> byKey;

            for (const TraitDef* def : resolved)
            {
                for (const auto& entry : def->Entries)
                {
                    std::string value;
                    const MergeMode mode = ModeForValue(entry.second, def->Mode, value);

                    auto it = byKey.find(entry.first);
                    if (it == byKey.end())
                    {
                        keyOrder.push_back(entry.first);
                        it = byKey.emplace(entry.first, std::vector<Contribution>()).first;
                    }
                    it->second.push_back(Contribution{ mode, value, def });
                }
            }

            // ---- 6. Fold and write -----------------------------------------
            for (const auto& key : keyOrder)
            {
                const std::string base = ReadKey(pINI, target.c_str(), key.c_str());
                const std::string result = FoldKey(base, byKey[key], target.c_str(), key.c_str());
                if (result == base)
                    continue;
                pINI->WriteString(target.c_str(), key.c_str(), result.c_str());
                Debug::Log("[TraitExt]   %s.%s: '%s' -> '%s'\n",
                    target.c_str(), key.c_str(), base.c_str(), result.c_str());
            }

            ++appliedTargets;
        }

        Debug::Log("[TraitExt] applied traits to %d target(s) from %d trait definition(s)\n",
            appliedTargets, static_cast<int>(traits.size()));
    }
}
