#include "TraitEngine.h"

#include <Phobos.h>
#include <CCINIClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <TechnoTypeClass.h>
#include <FootClass.h>          // generic_cast needs it complete
#include <FileSystem.h>
#include <Utilities/Debug.h>

#include <ctime>

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
        // Trait defs must outlive ProcessINI because instance pools point at
        // them for the whole match.
        std::unordered_map<std::string, TraitDef> g_Traits;
        std::unordered_map<std::string, InstancePool> g_InstancePools;
    }

    namespace
    {
        std::vector<std::pair<std::string, std::string>> g_CameoRestore;
        bool g_CameoFixEnabled = true;
    }

    namespace CameoFix
    {
        void Remember(const std::string& targetID, const std::string& originalArt)
        {
            g_CameoRestore.emplace_back(targetID, originalArt);
        }

        bool Enabled() { return g_CameoFixEnabled; }
        void SetEnabled(bool on) { g_CameoFixEnabled = on; }

        void Apply()
        {
            if (!g_CameoFixEnabled || g_CameoRestore.empty())
                return;

            for (const auto& kv : g_CameoRestore)
            {
                TechnoTypeClass* const pType = TechnoTypeClass::Find(kv.first.c_str());
                if (!pType)
                    continue;

                // Resolve the cameo the ORIGINAL art section would have used:
                // an explicit Cameo= in that art section, else the <art>ICON
                // filename convention.
                char buffer[0x40];
                // INI_Art is a CCINIClass by value, not a pointer.
                CCINIClass::INI_Art.ReadString(kv.second.c_str(), "Cameo", "", buffer, sizeof(buffer));

                std::string cameo(buffer);
                while (!cameo.empty() && static_cast<unsigned char>(cameo.back()) <= ' ')
                    cameo.pop_back();
                size_t lead = 0;
                while (lead < cameo.size() && static_cast<unsigned char>(cameo[lead]) <= ' ')
                    ++lead;
                cameo = cameo.substr(lead);
                if (cameo.empty())
                    cameo = kv.second + "ICON";
                if (cameo.size() < 5 || _stricmp(cameo.c_str() + cameo.size() - 4, ".shp") != 0)
                    cameo += ".shp";

                if (SHPStruct* const pSHP = FileSystem::LoadSHPFile(cameo.c_str()))
                {
                    std::strncpy(pType->CameoFile, cameo.c_str(), sizeof(pType->CameoFile) - 1);
                    pType->CameoFile[sizeof(pType->CameoFile) - 1] = '\0';
                    pType->Cameo = pSHP;
                    Debug::Log("[TraitExt] cameo kept for %s: '%s' (original art '%s')\n",
                        kv.first.c_str(), cameo.c_str(), kv.second.c_str());
                }
                else
                {
                    Debug::Log("[TraitExt] WARN cameo '%s' for %s failed to load; "
                        "sidebar will show the redirected art's cameo instead\n",
                        cameo.c_str(), kv.first.c_str());
                }
            }
        }
    }

    namespace
    {
        std::unordered_map<std::string, std::vector<const TraitDef*>> g_SpyTraits;
    }

    namespace SpyTraits
    {
        const std::vector<const TraitDef*>* Find(const char* buildingTypeID)
        {
            if (!buildingTypeID || g_SpyTraits.empty())
                return nullptr;
            const auto it = g_SpyTraits.find(buildingTypeID);
            return (it == g_SpyTraits.end()) ? nullptr : &it->second;
        }

        bool Any() { return !g_SpyTraits.empty(); }
    }

    namespace InstanceRandom
    {
        const InstancePool* Find(const char* typeID)
        {
            if (!typeID || g_InstancePools.empty())
                return nullptr;
            const auto it = g_InstancePools.find(typeID);
            return (it == g_InstancePools.end()) ? nullptr : &it->second;
        }

        bool Any()
        {
            return !g_InstancePools.empty();
        }
    }

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
            if (v == "append" || v == "listadd")   return MergeMode::Append;
            if (v == "remove" || v == "listremove")return MergeMode::Remove;
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
            case MergeMode::Append:   return "Append";
            case MergeMode::Remove:   return "Remove";
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

        // splitmix32 finalizer — decorrelates nearby seeds. Matters because
        // per-match seeds (Scenario UniqueID) can be close together, and raw
        // xorshift32 on adjacent seeds produces visibly correlated first draws.
        // The CnCNet spawner rewrites spawn.ini per match with a Seed= that is
        // identical on every client — the one true per-match synced value that
        // already exists at rules-load time. (ScenarioClass::Random is NOT yet
        // seeded this early: its state was byte-identical across matches.)
        std::uint32_t ReadSpawnSeed()
        {
            FILE* fp = std::fopen("spawn.ini", "r");
            if (!fp)
                return 0;

            std::uint32_t seed = 0;
            char line[256];
            while (std::fgets(line, sizeof(line), fp))
            {
                const char* p = line;
                while (*p && static_cast<unsigned char>(*p) <= ' ')
                    ++p;
                if (std::strncmp(p, "Seed", 4) != 0)
                    continue;
                p += 4;
                while (*p && static_cast<unsigned char>(*p) <= ' ')
                    ++p;
                if (*p != '=')
                    continue;
                seed = static_cast<std::uint32_t>(std::strtoul(p + 1, nullptr, 10));
                break;
            }
            std::fclose(fp);
            return seed;
        }

        std::uint32_t MixSeed(std::uint32_t x)
        {
            x += 0x9E3779B9u;
            x ^= x >> 16; x *= 0x21F0AAADu;
            x ^= x >> 15; x *= 0x735A2D97u;
            x ^= x >> 15;
            return x;
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

                // List modes operate element-wise on CSV values, so a trait can
                // add a prerequisite without restating the whole list.
                if (c.Mode == MergeMode::Append || c.Mode == MergeMode::Remove)
                {
                    std::vector<std::string> items = SplitCSV(cur);
                    const std::vector<std::string> operand = SplitCSV(c.Value);

                    for (const auto& op : operand)
                    {
                        const auto it = std::find(items.begin(), items.end(), op);
                        if (c.Mode == MergeMode::Append)
                        {
                            if (it == items.end())
                                items.push_back(op);
                        }
                        else if (it != items.end())
                        {
                            items.erase(it);
                        }
                    }

                    std::string joined;
                    for (size_t i = 0; i < items.size(); ++i)
                    {
                        if (i)
                            joined += ',';
                        joined += items[i];
                    }
                    cur = joined;
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

        // Flatten a trait's Composes= chain into `out`, depth-first, with the
        // trait's own contribution appended last so its keys win. Cycles are
        // reported and cut (Phobos's own $Inherits has no such guard).
        void ExpandTrait(const std::string& name,
            const std::unordered_map<std::string, TraitDef>& traits,
            std::vector<const TraitDef*>& out,
            std::vector<std::string>& stack,
            const char* target)
        {
            if (std::find(stack.begin(), stack.end(), name) != stack.end())
            {
                Debug::Log("[TraitExt] WARN %s: trait cycle detected at '%s', cutting\n",
                    target, name.c_str());
                return;
            }

            const auto it = traits.find(name);
            if (it == traits.end())
            {
                Debug::Log("[TraitExt] WARN %s: unknown trait '%s' (not in [TraitTypes])\n",
                    target, name.c_str());
                return;
            }

            stack.push_back(name);
            for (const auto& child : it->second.Composes)
                ExpandTrait(child, traits, out, stack, target);
            stack.pop_back();

            if (std::find(out.begin(), out.end(), &it->second) == out.end())
                out.push_back(&it->second);
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

        g_Traits.clear();
        g_InstancePools.clear();
        g_SpyTraits.clear();
        g_CameoRestore.clear();
        // Default ON: a random-art trait almost never wants the cameo to follow.
        g_CameoFixEnabled = ReadKey(pINI, SectConfig, "KeepOriginalCameo", "yes")[0] != 'n'
            && ReadKey(pINI, SectConfig, "KeepOriginalCameo", "yes")[0] != 'N';
        std::unordered_map<std::string, TraitDef>& traits = g_Traits;
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
            def.Order = static_cast<int>(traits.size());
            def.Mode = ParseMode(ReadKey(pINI, name.c_str(), "Merge", "Override"),
                MergeMode::Override);

            def.Composes = SplitCSV(ReadKey(pINI, name.c_str(), "Traits"));
            def.AppliesTo = SplitCSV(ReadKey(pINI, name.c_str(), "AppliesTo"));

            const int keyCount = pINI->GetKeyCount(name.c_str());
            for (int i = 0; i < keyCount; ++i)
            {
                const char* keyName = pINI->GetKeyName(name.c_str(), i);
                // Reserved metadata keys — these configure the trait, they are
                // NOT values to fold into targets. Missing one here leaks it
                // into every target section (e.g. "E1.AppliesTo -> E1,GGI").
                if (!keyName
                    || !std::strcmp(keyName, "Merge")
                    || !std::strcmp(keyName, "Traits")
                    || !std::strcmp(keyName, "AppliesTo"))
                    continue;
                if (keyName[0] == '$')
                    continue; // leave $Inherits and friends to Phobos

                // "<Key>.Merge=" sets the mode for one key only.
                const std::string keyStr(keyName);
                const std::string suffix = ".Merge";
                if (keyStr.size() > suffix.size() &&
                    keyStr.compare(keyStr.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    def.KeyModes.emplace_back(keyStr.substr(0, keyStr.size() - suffix.size()),
                        ParseMode(ReadKey(pINI, name.c_str(), keyName), def.Mode));
                    continue;
                }

                def.Entries.emplace_back(keyName, ReadKey(pINI, name.c_str(), keyName));
            }

            Debug::Log("[TraitExt] trait '%s' Merge=%s keys=%d composes=%d\n",
                name.c_str(), ModeName(def.Mode), static_cast<int>(def.Entries.size()),
                static_cast<int>(def.Composes.size()));
            traits.emplace(name, std::move(def));
        }

        if (traits.empty())
            return;

        // Seed selection. A FIXED seed means the same draw every launch forever,
        // which makes "random" useless in practice. ScenarioClass::UniqueID is a
        // per-match salt that is identical on every client, so it gives real
        // per-match variety while staying sync-safe — but only once a scenario
        // exists. Log what is actually available so the timing is provable.
        ScenarioClass* pScen = ScenarioClass::Instance;

        // Per-match seed source. UniqueID turned out not to vary usefully, so
        // prefer the STATE of ScenarioClass::Random — the synced in-match RNG,
        // seeded from the game seed (spawn.ini "Seed="), hence identical on every
        // client and different every match. We only READ its state; drawing from
        // it would consume the synced stream and shift vanilla randomness.
        // Seed policy hinges on whether the match must stay in lockstep:
        //
        //  * MULTIPLAYER  -> spawn.ini "Seed=" only. The CnCNet client rewrites
        //    it per game and every client gets the same value, so all clients
        //    draw identically. Anything machine-local here would desync.
        //  * SKIRMISH / CAMPAIGN -> no lockstep requirement at all, so mix in a
        //    local clock. This is what finally makes the draw vary per match:
        //    spawn.ini is only rewritten when the CnCNet client launches the
        //    game, so on a direct launch its Seed is constant (observed
        //    identical across matches), and ScenarioClass::Random is not seeded
        //    this early either.
        const std::uint32_t spawnSeed = ReadSpawnSeed();
        const bool isMP = SessionClass::IsMultiplayer();

        std::uint32_t matchSalt = spawnSeed;
        const char* saltSource = "spawn.ini Seed (MP: identical on all clients)";

        if (!isMP)
        {
            const std::uint32_t localEntropy =
                static_cast<std::uint32_t>(GetTickCount())
                ^ (static_cast<std::uint32_t>(std::time(nullptr)) << 1);
            matchSalt = spawnSeed ^ localEntropy;
            saltSource = "spawn.ini Seed ^ local clock (offline: no sync needed)";
        }

        if (matchSalt == 0 && pScen)
        {
            matchSalt = static_cast<std::uint32_t>(pScen->UniqueID);
            saltSource = "Scenario UniqueID (last-resort fallback, may not vary)";
        }

        Debug::Log("[TraitExt] seed context: MP=%d spawnSeed=%u UniqueID=%d -> salt=%08X (%s)\n",
            isMP ? 1 : 0, spawnSeed, pScen ? pScen->UniqueID : 0, matchSalt, saltSource);

        std::uint32_t globalSeed = 0x5EED1234u;
        bool seedFromScenario = false;
        {
            const std::string seedStr = ReadKey(pINI, SectConfig, "RandomSeed");
            double parsed = 0.0;
            if (ParseNumber(seedStr, parsed) && static_cast<long long>(parsed) != 0)
            {
                globalSeed = static_cast<std::uint32_t>(static_cast<long long>(parsed));
            }
            else if (matchSalt != 0)
            {
                // RandomSeed=0 (or absent) => per-match seed when one exists.
                globalSeed = matchSalt;
                seedFromScenario = true;
            }
        }
        Debug::Log("[TraitExt] using seed %u (%s)\n", globalSeed,
            seedFromScenario ? "per-match" : "fixed from RandomSeed");

        // ---- 2. Targets ---------------------------------------------------
        // Default scope is the four TechnoType lists; [TraitExt] TargetLists=
        // adds more list sections (Warheads, SuperWeaponTypes, ...) and
        // [TraitTargets] names individual sections directly.
        std::vector<std::string> targets;

        std::vector<std::string> listSections;
        for (const char* list : TargetLists)
            listSections.emplace_back(list);
        for (const auto& extra : SplitCSV(ReadKey(pINI, SectConfig, "TargetLists")))
        {
            if (std::find(listSections.begin(), listSections.end(), extra) == listSections.end())
                listSections.push_back(extra);
        }

        for (const auto& list : listSections)
            ReadListSection(pINI, list.c_str(), targets);

        ReadListSection(pINI, "TraitTargets", targets);

        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

        int appliedTargets = 0;

        for (const auto& target : targets)
        {
            if (!pINI->GetSection(target.c_str()))
                continue;

            // Spy-triggered traits: registered now, forced on at infiltration.
            {
                const std::vector<std::string> spy =
                    SplitCSV(ReadKey(pINI, target.c_str(), "SpyTraits"));
                if (!spy.empty())
                {
                    std::vector<const TraitDef*> defs;
                    for (const auto& n : spy)
                    {
                        const auto it = traits.find(n);
                        if (it == traits.end())
                        {
                            Debug::Log("[TraitExt] WARN %s: unknown SpyTraits entry '%s'\n",
                                target.c_str(), n.c_str());
                            continue;
                        }
                        defs.push_back(&it->second);
                    }
                    if (!defs.empty())
                    {
                        g_SpyTraits[target] = defs;
                        Debug::Log("[TraitExt] %s: registered %d spy trait(s)\n",
                            target.c_str(), static_cast<int>(defs.size()));
                    }
                }
            }

            std::vector<std::string> wanted = SplitCSV(ReadKey(pINI, target.c_str(), "Traits"));

            // Inverse assignment: traits that name this target via AppliesTo=.
            // Applied before the target's own Traits= so the target-side list
            // stays the more specific (later-folding) one.
            {
                std::vector<std::pair<int, std::string>> inverse;
                for (const auto& kv : traits)
                {
                    const TraitDef& def = kv.second;
                    if (std::find(def.AppliesTo.begin(), def.AppliesTo.end(), target)
                        != def.AppliesTo.end())
                    {
                        inverse.emplace_back(def.Order, def.Name);
                    }
                }
                // Fold in [TraitTypes] declaration order. Sorting by NAME instead
                // silently reorders the fold and changes results: with
                // T_Absolute(=500) and T_Beefy(+200), alphabetical put Override
                // first and yielded 700 instead of the intended 500.
                std::sort(inverse.begin(), inverse.end());
                for (auto it = inverse.rbegin(); it != inverse.rend(); ++it)
                    wanted.insert(wanted.begin(), it->second);
            }

            // ---- 3. Random pool (deterministic; identical on every client) --
            // "TraitsRandomPool" is the clearer name (it sits next to
            // TraitsRandomCount, which is a min,max range rather than a list);
            // "TraitsRandom" stays accepted as an alias.
            std::string poolStr = ReadKey(pINI, target.c_str(), "TraitsRandomPool");
            if (poolStr.empty())
                poolStr = ReadKey(pINI, target.c_str(), "TraitsRandom");
            if (!poolStr.empty())
            {
                std::vector<std::string> pool = SplitCSV(poolStr);

                // Scope toggle. Type (default) = one draw at load shared by the
                // whole type for the match. Instance = each unit draws its own
                // at runtime.
                const std::string scope = ReadKey(pINI, target.c_str(), "TraitsRandomScope", "Type");
                const bool perInstance =
                    (!scope.empty() && (scope[0] == 'I' || scope[0] == 'i'));

                if (perInstance && !pool.empty())
                {
                    InstancePool ip;
                    const std::vector<std::string> cp =
                        SplitCSV(ReadKey(pINI, target.c_str(), "TraitsRandomCount", "1,1"));
                    double tmp = 0.0;
                    if (cp.size() >= 1 && ParseNumber(cp[0], tmp)) ip.CountMin = static_cast<int>(tmp);
                    ip.CountMax = ip.CountMin;
                    if (cp.size() >= 2 && ParseNumber(cp[1], tmp)) ip.CountMax = static_cast<int>(tmp);

                    for (const auto& n : pool)
                    {
                        const auto it = traits.find(n);
                        if (it == traits.end())
                        {
                            Debug::Log("[TraitExt] WARN %s: unknown trait '%s' in instance pool\n",
                                target.c_str(), n.c_str());
                            continue;
                        }
                        ip.Traits.push_back(&it->second);
                    }

                    if (!ip.Traits.empty())
                    {
                        const int poolN = static_cast<int>(ip.Traits.size());
                        ip.CountMin = (std::max)(0, (std::min)(ip.CountMin, poolN));
                        ip.CountMax = (std::min)(poolN, (std::max)(ip.CountMin, ip.CountMax));
                        g_InstancePools[target] = ip;
                        Debug::Log("[TraitExt] %s: registered PER-INSTANCE pool of %d (count %d..%d)\n",
                            target.c_str(), poolN, ip.CountMin, ip.CountMax);
                    }
                    pool.clear(); // handled at runtime, not at load
                }

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

                    // Clamp BOTH ends to the pool size. Clamping only the max
                    // lets min exceed it, which inverts the range and makes the
                    // span negative (undefined once cast for the modulo).
                    const int poolSize = static_cast<int>(pool.size());
                    lo = (std::max)(0, (std::min)(lo, poolSize));
                    hi = (std::min)(poolSize, (std::max)(lo, hi));

                    std::uint32_t state = MixSeed(globalSeed ^ HashString(target));
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
                seen.push_back(name);

                std::vector<std::string> stack;
                ExpandTrait(name, traits, resolved, stack, target.c_str());
            }

            // A composed trait can be pulled in indirectly; honour blocks on it.
            if (!blocked.empty())
            {
                resolved.erase(std::remove_if(resolved.begin(), resolved.end(),
                    [&blocked](const TraitDef* def)
                    {
                        return std::find(blocked.begin(), blocked.end(), def->Name) != blocked.end();
                    }), resolved.end());
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
                    // Mode precedence: value sigil > "<Key>.Merge" > trait Merge=.
                    MergeMode keyMode = def->Mode;
                    for (const auto& km : def->KeyModes)
                    {
                        if (km.first == entry.first)
                        {
                            keyMode = km.second;
                            break;
                        }
                    }

                    std::string value;
                    const MergeMode mode = ModeForValue(entry.second, keyMode, value);

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

                // A changed Image also moves the cameo, since the cameo is read
                // from the art section ImageFile names. Remember the art section
                // in use beforehand so the cameo can be put back.
                if (!_stricmp(key.c_str(), "Image"))
                    CameoFix::Remember(target, base.empty() ? target : base);
            }

            ++appliedTargets;
        }

        Debug::Log("[TraitExt] applied traits to %d target(s) from %d trait definition(s)\n",
            appliedTargets, static_cast<int>(traits.size()));

        // Image= cycle check. A -> B while B -> A makes the engine fail to
        // resolve art: the unit renders as nothing and the sidebar shows a
        // garbage cameo, with no indication of why. Traits make this easy to
        // create accidentally across two independent random pools, so name it.
        for (const auto& target : targets)
        {
            const std::string img = ReadKey(pINI, target.c_str(), "Image");
            if (img.empty() || img == target)
                continue;
            const std::string back = ReadKey(pINI, img.c_str(), "Image");
            if (!back.empty() && back == target)
            {
                Debug::Log("[TraitExt] WARN circular Image: [%s] Image=%s and [%s] Image=%s "
                    "— the engine cannot resolve this; expect an invisible unit and a wrong "
                    "cameo. Break the cycle so at most one side redirects.\n",
                    target.c_str(), img.c_str(), img.c_str(), back.c_str());
            }
        }
    }
}
