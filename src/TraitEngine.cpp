#include "TraitEngine.h"

#include <Phobos.h>
#include <CCINIClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <TechnoTypeClass.h>
#include <TechnoClass.h>        // proximity gates walk TechnoClass::Array
#include <HouseClass.h>         // ... and ask it who owns what
#include <Fundamentals.h>       // Unsorted::CurrentFrame / LeptonsPerCell
#include <FootClass.h>          // generic_cast needs it complete
#include <FileSystem.h>
#include <CCFileClass.h>
#include <FileFormats/VXL.h>
#include <FileFormats/HVA.h>
#include <Memory.h>
#include <Utilities/Debug.h>

#include <ctime>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <iterator>             // std::size
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

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
        bool g_CameoApplied = false;
    }

    namespace
    {
        std::vector<std::pair<std::string, std::string>> g_MixedTurrets;
        bool g_MixedTurretEnabled = true;
        bool g_MixedTurretApplied = false;

        // Load "<name>.VXL" + "<name>.HVA" into a VoxelStruct the caller owns.
        // Modelled on Antares' own turret loader so the pair is prepared the
        // same way (notably the HVA scale from the VXL tailer).
        bool LoadVoxelPair(VoxelStruct& out, const char* pName)
        {
            char filename[0x40];
            out.VXL = nullptr;
            out.HVA = nullptr;

            std::snprintf(filename, sizeof(filename), "%s.VXL", pName);
            VoxLib* pVXL = nullptr;
            {
                CCFileClass* const pFile = GameCreate<CCFileClass>(filename);
                if (!pFile)
                    return false;
                if (!pFile->Exists())
                {
                    GameDelete(pFile);
                    return false;
                }

                pVXL = static_cast<VoxLib*>(YRMemory::AllocateChecked(sizeof(VoxLib)));
                std::memset(pVXL, 0, sizeof(VoxLib));
                if (!pVXL->ReadFile(pFile, false))
                    pVXL->Initialized = true;
                GameDelete(pFile);
            }

            std::snprintf(filename, sizeof(filename), "%s.HVA", pName);
            MotLib* pHVA = nullptr;
            if (CCFileClass* const pFile = GameCreate<CCFileClass>(filename))
            {
                if (pFile->Exists())
                {
                    pHVA = static_cast<MotLib*>(YRMemory::AllocateChecked(sizeof(MotLib)));
                    std::memset(pHVA, 0, sizeof(MotLib));
                    if (!pHVA->ReadFile(pFile))
                        pHVA->LoadedFailed = 1;
                }
                GameDelete(pFile);
            }

            if (pHVA && !pVXL->Initialized && !pHVA->LoadedFailed)
            {
                auto const& tailer = pVXL->TailerData[pVXL->HeaderData->limb_number];
                pHVA->Scale(tailer.HVAMultiplier);
                out.VXL = pVXL;
                out.HVA = pHVA;
                return true;
            }
            return false;
        }
    }

    namespace
    {
        std::unordered_map<void*, TechnoTypeClass*> g_VariantWeapon;
        std::unordered_set<std::string> g_WeaponClones;
        bool g_VariantWeaponEnabled = true;
    }

    bool IsWeaponKey(const char* k)
    {
        static const char* const kWeaponKeys[] = {
            "Primary", "Secondary", "ElitePrimary", "EliteSecondary",
            "Weapon1", "Weapon2", "Weapon3", "Weapon4",
            "EliteWeapon1", "EliteWeapon2", "EliteWeapon3", "EliteWeapon4",
        };
        for (const char* w : kWeaponKeys)
            if (!_stricmp(k, w))
                return true;
        return false;
    }

    namespace VariantWeapon
    {
        void MarkClone(const std::string& cloneID)
        {
            if (g_WeaponClones.insert(cloneID).second)
                Debug::Log("[TraitExt]   %s: carries its own weapon(s)\n", cloneID.c_str());
        }
        bool CloneHasWeapon(const std::string& cloneID)
        {
            return g_WeaponClones.count(cloneID) != 0;
        }
        void Assign(::TechnoClass* pThis, const char* cloneID)
        {
            if (!pThis || !cloneID || !*cloneID)
                return;
            if (!g_WeaponClones.count(cloneID))
                return;     // this variant defines no weapon: keep the base one

            TechnoTypeClass* const p = TechnoTypeClass::Find(cloneID);
            if (!p)
            {
                static std::unordered_set<std::string> s_warned;
                if (s_warned.insert(cloneID).second)
                    Debug::Log("[TraitExt] WARN weapon variant '%s' is not a TechnoType\n",
                        cloneID);
                return;
            }
            g_VariantWeapon[pThis] = p;
        }
        void Forget(::TechnoClass* pThis) { g_VariantWeapon.erase(pThis); }
        bool Any() { return !g_VariantWeapon.empty(); }
        bool Enabled() { return g_VariantWeaponEnabled; }
        void SetEnabled(bool on) { g_VariantWeaponEnabled = on; }

        TechnoTypeClass* For(::TechnoClass* pThis)
        {
            const auto it = g_VariantWeapon.find(pThis);
            return it == g_VariantWeapon.end() ? nullptr : it->second;
        }
    }

    namespace MixedTurret
    {
        void Remember(const std::string& cloneID, const std::string& turretArt)
        {
            g_MixedTurrets.emplace_back(cloneID, turretArt);
        }

        bool Enabled() { return g_MixedTurretEnabled; }
        void SetEnabled(bool on) { g_MixedTurretEnabled = on; }

        void Apply()
        {
            if (!g_MixedTurretEnabled || g_MixedTurrets.empty() || g_MixedTurretApplied)
                return;
            g_MixedTurretApplied = true;

            for (const auto& kv : g_MixedTurrets)
            {
                TechnoTypeClass* const pType = TechnoTypeClass::Find(kv.first.c_str());
                if (!pType)
                    continue;

                char name[0x40];
                int loaded = 0;

                // Slot 0 covers the single-turret case; the indexed slots cover
                // multi-turret donors (Prism-style). Missing files are normal.
                std::snprintf(name, sizeof(name), "%sTUR", kv.second.c_str());
                if (LoadVoxelPair(pType->TurretVoxel, name))
                    ++loaded;
                if (LoadVoxelPair(pType->ChargerTurrets[0], name))
                    ++loaded;

                std::snprintf(name, sizeof(name), "%sBARL", kv.second.c_str());
                if (LoadVoxelPair(pType->BarrelVoxel, name))
                    ++loaded;
                if (LoadVoxelPair(pType->ChargerBarrels[0], name))
                    ++loaded;

                for (int i = 1; i < 4; ++i)
                {
                    std::snprintf(name, sizeof(name), "%sTUR%d", kv.second.c_str(), i);
                    if (LoadVoxelPair(pType->ChargerTurrets[i], name))
                        ++loaded;
                    std::snprintf(name, sizeof(name), "%sBARL%d", kv.second.c_str(), i);
                    if (LoadVoxelPair(pType->ChargerBarrels[i], name))
                        ++loaded;
                }

                Debug::Log("[TraitExt] mixed turret: %s now wears '%s' turret art (%d voxel pair(s))\n",
                    kv.first.c_str(), kv.second.c_str(), loaded);
            }
        }
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

            // 0x679CAF fires once per rules pass, so without this guard the
            // whole restore re-runs several times per game start, reloading and
            // leaking an SHP for every affected type each time. Reset by
            // ProcessINI so a later match still gets its cameos restored.
            if (g_CameoApplied)
                return;
            g_CameoApplied = true;

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

    namespace
    {
        std::unordered_map<std::string, std::vector<ConditionalTrait>> g_Conditional;

        // Every type ID any proximity gate watches for, and the per-frame
        // snapshot of where those objects currently are.
        std::unordered_set<std::string> g_NearWatch;
        std::unordered_map<std::string, std::vector<TechnoClass*>> g_NearIndex;
        int g_NearIndexFrame = -1;
    }

    namespace Conditional
    {
        const std::vector<ConditionalTrait>* Find(const char* typeID)
        {
            if (!typeID || g_Conditional.empty())
                return nullptr;
            const auto it = g_Conditional.find(typeID);
            return (it == g_Conditional.end()) ? nullptr : &it->second;
        }
        bool Any() { return !g_Conditional.empty(); }
        void Register(const std::string& targetID, const ConditionalTrait& ct)
        {
            g_Conditional[targetID].push_back(ct);
            for (const auto& id : ct.NearTypes)
                g_NearWatch.insert(id);
        }
        void Clear() { g_Conditional.clear(); g_NearWatch.clear(); g_NearIndex.clear(); }

        bool AnyNear() { return !g_NearWatch.empty(); }

        // The index is a snapshot of every watched object's position, rebuilt on
        // the same cadence the conditional check itself runs on. Without it each
        // unit would walk the whole TechnoClass::Array, which is units x technos
        // of work; with it the walk happens once and each unit only scans the
        // handful of objects it actually cares about.
        //
        // SYNC (encyclopedia/Logic-Frame-Update.md): this feeds gameplay, so the
        // rebuild iterates TechnoClass::Array in INDEX order — never a hash map
        // — and distances are integer leptons. Pointer-ordered iteration here is
        // the classic desync that only shows up after minutes of play.
        static void RebuildNearIndex()
        {
            const int now = Unsorted::CurrentFrame;
            if (now == g_NearIndexFrame)
                return;
            g_NearIndexFrame = now;

            for (auto& kv : g_NearIndex)
                kv.second.clear();

            for (int i = 0; i < TechnoClass::Array.Count; ++i)
            {
                TechnoClass* const pOther = TechnoClass::Array.GetItem(i);
                if (!pOther || !pOther->IsAlive || pOther->InLimbo)
                    continue;

                TechnoTypeClass* const pType = pOther->GetTechnoType();
                if (!pType || !g_NearWatch.count(pType->ID))
                    continue;

                g_NearIndex[pType->ID].push_back(pOther);
            }
        }

        bool NearMeets(::TechnoClass* pThis, const ConditionalTrait& ct)
        {
            if (ct.NearTypes.empty())
                return true;        // no proximity gate on this trait
            if (!pThis)
                return false;

            RebuildNearIndex();

            HouseClass* const pOwner = pThis->Owner;
            const int reach = ct.NearRange * Unsorted::LeptonsPerCell;

            for (const auto& id : ct.NearTypes)
            {
                const auto it = g_NearIndex.find(id);
                if (it == g_NearIndex.end())
                    continue;

                for (TechnoClass* const pOther : it->second)
                {
                    if (pOther == pThis)
                        continue;   // a unit must not satisfy its own gate

                    HouseClass* const pTheirs = pOther->Owner;
                    switch (ct.NearOwner)
                    {
                    case ConditionalTrait::Whose::Owner:
                        if (pTheirs != pOwner) continue;
                        break;
                    case ConditionalTrait::Whose::Ally:
                        if (!pOwner || !pOwner->IsAlliedWith(pTheirs)) continue;
                        break;
                    case ConditionalTrait::Whose::Enemy:
                        if (!pOwner || pOwner->IsAlliedWith(pTheirs)) continue;
                        break;
                    case ConditionalTrait::Whose::Any:
                        break;
                    }

                    if (pThis->DistanceFrom(pOther) <= reach)
                        return true;
                }
            }
            return false;
        }
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

    namespace
    {
        // Everything a synthesised clone type needs beyond $Inherits+Image.
        // Shared by the random-pool path and the prerequisite-gated path: it
        // used to live only in the former, which is why a gated Battle
        // Fortress FV changed its image but kept its old turret and weapon.
        void FurnishClone(CCINIClass* pINI, const std::string& cloneID,
            const TraitDef& def, const std::string& artDonor,
            const std::string& target)
        {
            // Turret presence must agree with the borrowed art.
            // The clone inherits the TARGET's Turret= (e.g. the
            // Grizzly's "yes"), but the art it now wears may have
            // no turret voxel at all (TNKD is Turret=no, Mirage
            // has none), which is why bodies changed while
            // turrets vanished. Adopt the art donor's turret
            // settings unless the trait states them itself.
            // Turret rendering is driven by a FAMILY of tags, not
            // just Turret=. The Prism Tank, for example, is
            // TurretCount=4 with WeaponCount=1 and picks a turret
            // voxel by RANGE (Turret.RangeBands /
            // Turret.RangeIndices) — copying only Turret and
            // TurretCount left the selector unset, so the body
            // drew and the turret did not. Inherit the whole
            // family from the art donor so type and art agree.
            static const char* const kTurretKeys[] = {
                "Turret", "TurretCount", "TurretOffset", "UseTurretShadow",
                "Turret.RangeBands", "Turret.RangeIndices",
                "TurretNotExportedOnGround", "TurretAnim", "TurretAnimIsVoxel",
                "TurretAnimX", "TurretAnimY", "TurretAnimZAdjust",
                "WeaponTurretIndex1", "WeaponTurretIndex2",
                "WeaponTurretIndex3", "WeaponTurretIndex4",
            };

            int copied = 0;
            for (const char* ak : kTurretKeys)
            {
                bool traitSpecifies = false;
                for (const auto& te : def.Entries)
                {
                    if (!_stricmp(te.first.c_str(), ak)) { traitSpecifies = true; break; }
                }
                if (traitSpecifies)
                    continue;   // author's own value wins

                const std::string donor = ReadKey(pINI, artDonor.c_str(), ak);
                if (!donor.empty())
                {
                    pINI->WriteString(cloneID.c_str(), ak, donor.c_str());
                    ++copied;
                }
                else if (!_stricmp(ak, "Turret"))
                {
                    // Donor has no turret at all: say so
                    // explicitly rather than inheriting the
                    // target's "yes" and hunting a voxel that
                    // does not exist.
                    pINI->WriteString(cloneID.c_str(), ak, "no");
                }
            }
            Debug::Log("[TraitExt]   %s: inherited %d turret tag(s) from '%s'\n",
                cloneID.c_str(), copied, artDonor.c_str());

            // Mixed turrets: resolve the turret donor's ART name
            // the same way the body image is resolved, then have
            // the clone load that turret art for itself.
            if (!def.TurretFrom.empty())
            {
                std::string tart = def.TurretFrom;
                for (int hops = 0; hops < 8; ++hops)
                {
                    const std::string nx = ReadKey(pINI, tart.c_str(), "Image");
                    if (nx.empty() || nx == tart) break;
                    tart = nx;
                }
                MixedTurret::Remember(cloneID, tart);
                Debug::Log("[TraitExt]   %s: turret art from '%s' (resolved '%s')\n",
                    cloneID.c_str(), def.TurretFrom.c_str(), tart.c_str());
            }

            // Any other TYPE-level key on the trait belongs on
            // the clone — that is what makes "change the body
            // but keep the turret" (or vice versa) expressible.
            for (const auto& te : def.Entries)
            {
                const char* k = te.first.c_str();
                if (!_stricmp(k, "Image")
                    || !_stricmp(k, "Health") || !_stricmp(k, "Strength")
                    || !_stricmp(k, "Veterancy") || !_stricmp(k, "Ammo"))
                    continue;   // instance-level, applied per unit

                // TraitExt's own key, not a game key — don't
                // write it into the clone section.
                if (!_stricmp(k, "ForceBodyFacing"))
                {
                    const bool on = !te.second.empty()
                        && (te.second[0] == 'y' || te.second[0] == 'Y'
                            || te.second[0] == 't' || te.second[0] == 'T'
                            || te.second[0] == '1');
                    VariantArt::SetForceBodyFacing(cloneID, on);
                    if (on)
                        Debug::Log("[TraitExt]   %s: must aim with its hull "
                            "(ForceBodyFacing)\n", cloneID.c_str());
                    continue;
                }

                // The clone is swapped in ONLY for the draw, so
                // only keys the renderer reads can have any
                // effect. Cost/Armor/Strength/Prerequisite and
                // friends belong to the real buildable type —
                // writing them here would look like it works and
                // silently do nothing (Rex hit exactly this with
                // Cost). Name the limit instead.
                static const char* const kDrawKeys[] = {
                    "Turret", "TurretCount", "TurretOffset", "UseTurretShadow",
                    "Turret.RangeBands", "Turret.RangeIndices",
                    "TurretNotExportedOnGround", "TurretAnim", "TurretAnimIsVoxel",
                    "TurretAnimX", "TurretAnimY", "TurretAnimZAdjust",
                    "WeaponTurretIndex1", "WeaponTurretIndex2",
                    "WeaponTurretIndex3", "WeaponTurretIndex4",
                    "Voxel", "Remapable", "AlphaImage", "Palette",
                };

                bool drawRelevant = false;
                for (const char* dk : kDrawKeys)
                {
                    if (!_stricmp(k, dk)) { drawRelevant = true; break; }
                }

                // Weapons ARE answerable per unit, because the
                // engine asks the instance (TechnoClass::
                // GetWeapon) rather than the type. Let them onto
                // the clone and flag it.
                if (!drawRelevant && IsWeaponKey(k))
                {
                    VariantWeapon::MarkClone(cloneID);
                    pINI->WriteString(cloneID.c_str(), k, te.second.c_str());
                    continue;
                }

                if (!drawRelevant)
                {
                    Debug::Log("[TraitExt]   WARN %s: '%s' has NO EFFECT here — the "
                        "variant type is swapped in only for DRAWING and for the "
                        "weapon lookup, so every other gameplay key still comes "
                        "from [%s]. Apply it as a plain (non-variant) trait.\n",
                        cloneID.c_str(), k, target.c_str());
                    continue;
                }

                pINI->WriteString(cloneID.c_str(), k, te.second.c_str());
            }
        }
    }

    namespace
    {
        // ---- InheritFrom: copy a whole TechnoType into a trait --------------
        //
        // The point of the feature: name a real (or dummy) unit and take what it
        // defines, instead of hand-writing every key. Expanded at LOAD into the
        // trait's own Entries, so the existing fold, clone and gate machinery
        // all work unchanged - there is no new runtime path.
        //
        // Two things make this more than a key-copy loop:
        //
        // 1. IDENTITY IS NOT AN ATTRIBUTE. A unit section carries its name,
        //    price and buildability alongside its stats. Copying those makes the
        //    target a duplicate of the donor rather than a variant of itself, so
        //    they are excluded unless asked for by name.
        //
        // 2. TAGS COME IN FAMILIES. Half a family is worse than none: copying
        //    Turret= without Turret.RangeBands= drew a body with no turret, and
        //    inheriting Primary= onto a Gunner/WeaponCount unit did nothing at
        //    all because that unit answers from its Weapon1..N list. Both cost a
        //    debugging round in this project. So a family moves together, and
        //    where the donor lacks the family the target's copy is explicitly
        //    switched OFF rather than left to contradict the new values.

        struct KeyFamily { const char* Name; const char* const* Keys; int Count; };

        const char* const kFamArt[] = {
            "Image", "Voxel", "Remapable", "AlphaImage", "Palette",
            "Cameo", "AltCameo", "Anim", "FireUp", "WaterImage",
        };
        const char* const kFamTurret[] = {
            "Turret", "TurretCount", "TurretOffset", "UseTurretShadow",
            "Turret.RangeBands", "Turret.RangeIndices",
            "TurretNotExportedOnGround", "TurretAnim", "TurretAnimIsVoxel",
            "TurretAnimX", "TurretAnimY", "TurretAnimZAdjust",
            "WeaponTurretIndex1", "WeaponTurretIndex2",
            "WeaponTurretIndex3", "WeaponTurretIndex4",
        };
        const char* const kFamWeapons[] = {
            "Primary", "Secondary", "ElitePrimary", "EliteSecondary",
            "Gunner", "WeaponCount", "IsGattling", "OpportunityFire",
            "Weapon1", "Weapon2", "Weapon3", "Weapon4",
            "EliteWeapon1", "EliteWeapon2", "EliteWeapon3", "EliteWeapon4",
        };
        const char* const kFamArmor[] = {
            "Armor", "Strength", "SelfHealing", "ImmuneToRadiation",
            "ImmuneToPsionics", "ImmuneToVeins", "Explodes",
        };
        const char* const kFamMovement[] = {
            "Speed", "SpeedType", "MovementZone", "Locomotor", "ROT",
            "TurnRate", "Crusher", "Crushable", "OmniCrusher",
            "JumpJet", "Accelerates", "MovementRestrictedTo",
        };
        // Excluded by default - these say WHICH UNIT THIS IS, not what it can do.
        const char* const kFamIdentity[] = {
            "UIName", "Name", "AIBasePlanningSide", "DeploysInto",
            "UndeploysInto", "Category",
        };
        const char* const kFamEconomy[] = {
            "Cost", "Soylent", "Points", "TechLevel", "Prerequisite",
            "Owner", "ForbiddenHouses", "RequiredHouses", "BuildLimit",
            "AIBuildThis", "Nominal",
        };

        const KeyFamily kFamilies[] = {
            { "Art",      kFamArt,      static_cast<int>(std::size(kFamArt))      },
            { "Turret",   kFamTurret,   static_cast<int>(std::size(kFamTurret))   },
            { "Weapons",  kFamWeapons,  static_cast<int>(std::size(kFamWeapons))  },
            { "Armor",    kFamArmor,    static_cast<int>(std::size(kFamArmor))    },
            { "Movement", kFamMovement, static_cast<int>(std::size(kFamMovement)) },
            { "Identity", kFamIdentity, static_cast<int>(std::size(kFamIdentity)) },
            { "Economy",  kFamEconomy,  static_cast<int>(std::size(kFamEconomy))  },
        };

        const KeyFamily* FindFamily(const char* name)
        {
            for (const auto& f : kFamilies)
                if (!_stricmp(f.Name, name))
                    return &f;
            return nullptr;
        }

        bool FamilyHasKey(const KeyFamily& f, const char* key)
        {
            for (int i = 0; i < f.Count; ++i)
                if (!_stricmp(f.Keys[i], key))
                    return true;
            return false;
        }

        // Does this filter list (of key names AND/OR family names) name the key?
        bool ListNamesKey(const std::vector<std::string>& list, const char* key)
        {
            for (const auto& entry : list)
            {
                if (!_stricmp(entry.c_str(), key))
                    return true;
                if (const KeyFamily* const f = FindFamily(entry.c_str()))
                    if (FamilyHasKey(*f, key))
                        return true;
            }
            return false;
        }

        bool IsExcludedByDefault(const char* key)
        {
            // By name, not index — reordering kFamilies must not silently
            // change which keys are protected.
            const KeyFamily* const id = FindFamily("Identity");
            const KeyFamily* const ec = FindFamily("Economy");
            return (id && FamilyHasKey(*id, key)) || (ec && FamilyHasKey(*ec, key));
        }

        // Resolve the ART SECTION a type actually draws from: its own Image= if
        // it redirects, else its own ID. [SREF] does NOT contain "Image=SREF",
        // so "look like a Prism Tank" has to be synthesised rather than copied.
        std::string ResolveArtName(CCINIClass* pINI, const std::string& id)
        {
            std::string art = id;
            for (int hops = 0; hops < 8; ++hops)
            {
                const std::string nx = ReadKey(pINI, art.c_str(), "Image");
                if (nx.empty() || nx == art)
                    break;
                art = nx;
            }
            return art;
        }

        void ExpandInheritFrom(CCINIClass* pINI,
            std::unordered_map<std::string, TraitDef>& traits)
        {
            for (auto& kv : traits)
            {
                TraitDef& def = kv.second;
                if (def.InheritFrom.empty())
                    continue;

                // The trait's own keys must win, so collect them first and skip
                // anything the author already stated.
                std::unordered_set<std::string> own;
                for (const auto& e : def.Entries)
                {
                    std::string k = e.first;
                    for (auto& c : k) c = static_cast<char>(std::tolower(c));
                    own.insert(k);
                }

                std::vector<std::pair<std::string, std::string>> copied;

                for (const auto& donor : def.InheritFrom)
                {
                    if (!pINI->GetSection(donor.c_str()))
                    {
                        Debug::Log("[TraitExt] WARN trait '%s': InheritFrom names '%s', "
                            "which is not a section in the rules\n",
                            def.Name.c_str(), donor.c_str());
                        continue;
                    }

                    int took = 0, skipped = 0;
                    const int n = pINI->GetKeyCount(donor.c_str());
                    for (int i = 0; i < n; ++i)
                    {
                        const char* const key = pINI->GetKeyName(donor.c_str(), i);
                        if (!key || !*key || key[0] == '$')
                            continue;   // leave $Inherits to Phobos

                        std::string lower(key);
                        for (auto& c : lower) c = static_cast<char>(std::tolower(c));
                        if (own.count(lower))
                            continue;   // author stated it themselves

                        // InheritOnly, when present, is the whole allow-list.
                        if (!def.InheritOnly.empty())
                        {
                            if (!ListNamesKey(def.InheritOnly, key))
                                continue;
                        }
                        else if (IsExcludedByDefault(key))
                        {
                            ++skipped;
                            continue;
                        }
                        if (ListNamesKey(def.InheritExcept, key))
                        {
                            ++skipped;
                            continue;
                        }

                        copied.emplace_back(key, ReadKey(pINI, donor.c_str(), key));
                        own.insert(lower);
                        ++took;
                    }

                    // ART: the donor's look is its section name, not a key, so it
                    // is never picked up by the loop above. Synthesise it unless
                    // Art was filtered out or the author set Image themselves.
                    const bool wantArt = def.InheritOnly.empty()
                        ? !ListNamesKey(def.InheritExcept, "Image")
                        : ListNamesKey(def.InheritOnly, "Image");
                    if (wantArt && !own.count("image"))
                    {
                        const std::string art = ResolveArtName(pINI, donor);
                        copied.emplace_back("Image", art);
                        own.insert("image");
                        ++took;
                        Debug::Log("[TraitExt]   %s: look from '%s' (art '%s')\n",
                            def.Name.c_str(), donor.c_str(), art.c_str());
                    }

                    Debug::Log("[TraitExt] trait '%s': inherited %d key(s) from '%s' "
                        "(%d skipped as identity/economy)\n",
                        def.Name.c_str(), took, donor.c_str(), skipped);
                }

                if (copied.empty())
                    continue;

                // ---- family coherence ------------------------------------
                // A donor that does not use a family must TURN THE TARGET'S OFF,
                // or the target keeps its own setting and silently contradicts
                // the values we just copied. Both of these cost a debugging
                // round before being understood.
                auto haveKey = [&](const char* k) -> bool
                {
                    for (const auto& c : copied)
                        if (!_stricmp(c.first.c_str(), k))
                            return true;
                    return false;
                };

                const std::string& firstDonor = def.InheritFrom.front();

                // Weapons: a WeaponCount/Gunner target answers from Weapon1..N
                // and never reads Primary. Inheriting Primary= from a plain tank
                // onto an IFV therefore does nothing at all unless the list is
                // switched off too.
                if (haveKey("Primary") && !haveKey("WeaponCount")
                    && !own.count("weaponcount"))
                {
                    const std::string wc = ReadKey(pINI, firstDonor.c_str(), "WeaponCount");
                    if (wc.empty())
                    {
                        copied.emplace_back("WeaponCount", "0");
                        copied.emplace_back("Gunner", "no");
                        Debug::Log("[TraitExt]   %s: donor has no WeaponCount, so the "
                            "target's Weapon1..N list is switched off - otherwise the "
                            "inherited Primary= would never be read\n", def.Name.c_str());
                    }
                }

                // Turret: same shape. A donor with no turret must say so, or the
                // target keeps Turret=yes and hunts a voxel that does not exist.
                if (!haveKey("Turret") && !own.count("turret"))
                {
                    const std::string t = ReadKey(pINI, firstDonor.c_str(), "Turret");
                    if (t.empty())
                    {
                        copied.emplace_back("Turret", "no");
                        Debug::Log("[TraitExt]   %s: donor has no turret, saying so "
                            "explicitly\n", def.Name.c_str());
                    }
                }

                // Inherited keys go BEFORE the author's own, so the author wins.
                copied.insert(copied.end(), def.Entries.begin(), def.Entries.end());
                def.Entries.swap(copied);
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
        g_CameoApplied = false;
        // Kill switch: the draw-time Type swap is the riskiest thing here, so
        // it can be disabled from INI without a rebuild.
        VariantArt::SetEnabled(ReadKey(pINI, SectConfig, "VariantArt", "yes")[0] != 'n');
        MixedTurret::SetEnabled(ReadKey(pINI, SectConfig, "MixedTurrets", "yes")[0] != 'n');
        g_MixedTurrets.clear();
        g_WeaponClones.clear();
        VariantWeapon::SetEnabled(ReadKey(pINI, SectConfig, "VariantWeapons", "yes")[0] != 'n');
        Conditional::Clear();
        g_MixedTurretApplied = false;
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
            def.RandomPoolFor = SplitCSV(ReadKey(pINI, name.c_str(), "RandomPoolFor"));
            def.RandomScope = ReadKey(pINI, name.c_str(), "RandomScope");
            def.TurretFrom = ReadKey(pINI, name.c_str(), "TurretFrom");
            def.RerollInterval = ReadKey(pINI, name.c_str(), "RerollInterval");
            def.Requirement = SplitCSV(ReadKey(pINI, name.c_str(), "Requirement"));
            def.InheritFrom = SplitCSV(ReadKey(pINI, name.c_str(), "InheritFrom"));
            def.InheritOnly = SplitCSV(ReadKey(pINI, name.c_str(), "InheritOnly"));
            def.InheritExcept = SplitCSV(ReadKey(pINI, name.c_str(), "InheritExcept"));
            def.NearTypes = SplitCSV(ReadKey(pINI, name.c_str(), "NearTypes"));
            def.NearOwner = ReadKey(pINI, name.c_str(), "NearOwner");
            {
                const std::string r = ReadKey(pINI, name.c_str(), "NearRange");
                def.NearRange = r.empty() ? 0 : std::atoi(r.c_str());
                if (!def.NearTypes.empty() && def.NearRange <= 0)
                {
                    // Defaulting silently would look like a gate that never opens.
                    def.NearRange = 5;
                    Debug::Log("[TraitExt] WARN trait '%s': NearTypes needs NearRange "
                        "(in cells); assuming %d\n", name.c_str(), def.NearRange);
                }
            }

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
                    || !std::strcmp(keyName, "AppliesTo")
                    || !std::strcmp(keyName, "RandomPoolFor")
                    || !std::strcmp(keyName, "RandomScope")
                    || !std::strcmp(keyName, "TurretFrom")
                    || !std::strcmp(keyName, "RerollInterval")
                    || !std::strcmp(keyName, "Requirement")
                    || !std::strcmp(keyName, "InheritFrom")
                    || !std::strcmp(keyName, "InheritOnly")
                    || !std::strcmp(keyName, "InheritExcept")
                    || !std::strcmp(keyName, "NearTypes")
                    || !std::strcmp(keyName, "NearRange")
                    || !std::strcmp(keyName, "NearOwner"))
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

        // Resolve InheritFrom before ANY folding: it only rewrites each trait's
        // own Entries, so every downstream path (fold, clone, gate) sees a
        // trait that looks hand-written.
        ExpandInheritFrom(pINI, traits);

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

        // Remember which list each target came from, so a synthesised variant
        // type can be registered in that same list (or it never loads).
        std::unordered_map<std::string, std::string> targetList;
        for (const auto& list : listSections)
        {
            std::vector<std::string> got;
            ReadListSection(pINI, list.c_str(), got);
            for (const auto& g : got)
            {
                targets.push_back(g);
                targetList.emplace(g, list);
            }
        }

        ReadListSection(pINI, "TraitTargets", targets);

        // A trait naming a section in AppliesTo= is a clear statement of intent,
        // so treat that section as a target even if it is not in any type list.
        // This is what lets traits reach sections the lists never cover —
        // weapons, warheads, projectiles — since YR has no master list for them
        // (they exist only as sections referenced by name).
        // Variant art swaps the Type pointer inside UnitClass::DrawAsVXL /
        // DrawAsSHP and resolves clones through UnitTypeClass - both are
        // VEHICLE paths. Infantry, aircraft and buildings draw elsewhere, so a
        // clone for them would be built and then never used. Say so at load.
        auto artCapable = [&](const std::string& tgt) -> bool
        {
            const auto lit = targetList.find(tgt);
            return lit != targetList.end() && !_stricmp(lit->second.c_str(), "VehicleTypes");
        };

        // Conditional traits are NOT folded statically — they must be judged
        // per unit against its owner's buildings, or one player's Battle Lab
        // would upgrade everybody's units.
        int condClones = 0;
        for (const auto& kv : traits)
        {
            const TraitDef& def = kv.second;
            // A proximity gate is a conditional too, and may stand alone: a
            // trait can be "while near a Battle Fortress" with no house-wide
            // prerequisite at all.
            if (def.Requirement.empty() && def.NearTypes.empty())
                continue;

            for (const auto& want : def.AppliesTo)
            {
                if (!pINI->GetSection(want.c_str()))
                    continue;

                ConditionalTrait ct;
                ct.Def = &def;
                ct.Requirement = def.Requirement;
                ct.NearTypes = def.NearTypes;
                ct.NearRange = def.NearRange;
                if (!_stricmp(def.NearOwner.c_str(), "Ally"))
                    ct.NearOwner = ConditionalTrait::Whose::Ally;
                else if (!_stricmp(def.NearOwner.c_str(), "Enemy"))
                    ct.NearOwner = ConditionalTrait::Whose::Enemy;
                else if (!_stricmp(def.NearOwner.c_str(), "Any"))
                    ct.NearOwner = ConditionalTrait::Whose::Any;

                // If it changes the look, it needs a clone type just like a
                // random variant does.
                for (const auto& e : def.Entries)
                {
                    if (_stricmp(e.first.c_str(), "Image") != 0)
                        continue;

                    if (!artCapable(want))
                    {
                        Debug::Log("[TraitExt] WARN %s: trait '%s' sets Image, but variant art "
                            "is VEHICLE-ONLY (the draw swap hooks UnitClass). %s is not a "
                            "VehicleType, so its look cannot change - the trait's other keys "
                            "still apply.\n", want.c_str(), def.Name.c_str(), want.c_str());
                        break;
                    }

                    std::string art = e.second;
                    for (int hops = 0; hops < 8; ++hops)
                    {
                        const std::string nx = ReadKey(pINI, art.c_str(), "Image");
                        if (nx.empty() || nx == art) break;
                        art = nx;
                    }

                    char buf[24];
                    std::snprintf(buf, sizeof(buf), "%.14s$C%d", want.c_str(), condClones++);
                    ct.CloneID = buf;

                    pINI->WriteString(ct.CloneID.c_str(), "$Inherits", want.c_str());
                    pINI->WriteString(ct.CloneID.c_str(), "Image", art.c_str());

                    // Same furnishing as a random variant: turret tags that
                    // match the borrowed art, plus the trait's own draw and
                    // weapon keys. Without this the unlock changed the image
                    // and nothing else.
                    FurnishClone(pINI, ct.CloneID, def, e.second, want);

                    const auto lit = targetList.find(want);
                    if (lit != targetList.end())
                    {
                        const int n2 = pINI->GetKeyCount(lit->second.c_str());
                        char idx[16];
                        std::snprintf(idx, sizeof(idx), "%d", n2);
                        pINI->WriteString(lit->second.c_str(), idx, ct.CloneID.c_str());
                    }
                    break;
                }

                Conditional::Register(want, ct);
                // Requirement may now be empty (a proximity-only gate), so this
                // can no longer index it blindly.
                Debug::Log("[TraitExt] %s: conditional trait '%s' (needs %s%s%s)%s\n",
                    want.c_str(), def.Name.c_str(),
                    def.Requirement.empty() ? "-" : def.Requirement[0].c_str(),
                    def.NearTypes.empty() ? "" : ", near ",
                    def.NearTypes.empty() ? "" : def.NearTypes[0].c_str(),
                    ct.CloneID.empty() ? "" : " [has variant art]");
            }
        }

        for (const auto& kv : traits)
        {
            if (!kv.second.Requirement.empty())
                continue;       // handled above, at runtime
            for (const auto& want : kv.second.AppliesTo)
            {
                if (std::find(targets.begin(), targets.end(), want) != targets.end())
                    continue;
                if (!pINI->GetSection(want.c_str()))
                {
                    Debug::Log("[TraitExt] WARN trait '%s' AppliesTo '%s', which is not a "
                        "section in this INI\n", kv.second.Name.c_str(), want.c_str());
                    continue;
                }
                targets.push_back(want);
                Debug::Log("[TraitExt] '%s' added as a target via AppliesTo (trait '%s')\n",
                    want.c_str(), kv.second.Name.c_str());
            }
        }

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
                    if (!def.Requirement.empty())
                        continue;   // gated: applied per unit at runtime, not folded here
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

            // Merge in traits that nominated themselves via RandomPoolFor=,
            // in [TraitTypes] declaration order so the pool is stable.
            std::string inverseScope;
            {
                std::vector<std::pair<int, std::string>> joined;
                for (const auto& kv : traits)
                {
                    const TraitDef& def = kv.second;
                    if (std::find(def.RandomPoolFor.begin(), def.RandomPoolFor.end(), target)
                        == def.RandomPoolFor.end())
                        continue;
                    joined.emplace_back(def.Order, def.Name);
                    if (!def.RandomScope.empty())
                    {
                        if (inverseScope.empty())
                            inverseScope = def.RandomScope;
                        else if (_stricmp(inverseScope.c_str(), def.RandomScope.c_str()) != 0)
                        {
                            Debug::Log("[TraitExt] WARN %s: conflicting RandomScope in pool "
                                "('%s' vs '%s'); using '%s'\n", target.c_str(),
                                inverseScope.c_str(), def.RandomScope.c_str(), inverseScope.c_str());
                        }
                    }
                }
                std::sort(joined.begin(), joined.end());
                for (const auto& j : joined)
                {
                    if (!poolStr.empty())
                        poolStr += ',';
                    poolStr += j.second;
                }
            }
            if (!poolStr.empty())
            {
                std::vector<std::string> pool = SplitCSV(poolStr);

                // Scope toggle. Type (default) = one draw at load shared by the
                // whole type for the match. Instance = each unit draws its own
                // at runtime.
                std::string scope = ReadKey(pINI, target.c_str(), "TraitsRandomScope", "");
                if (scope.empty())
                    scope = inverseScope;      // trait-side RandomScope=
                if (scope.empty())
                    scope = "Type";            // default
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

                    // "N" or "min,max" — a range keeps a group of units from
                    // morphing in lockstep.
                    auto readInterval = [&](const std::string& spec)
                    {
                        if (spec.empty())
                            return;
                        const std::vector<std::string> parts = SplitCSV(spec);
                        double v = 0.0;
                        if (parts.size() >= 1 && ParseNumber(parts[0], v))
                        {
                            ip.RerollMin = static_cast<int>(v);
                            ip.RerollMax = ip.RerollMin;
                        }
                        if (parts.size() >= 2 && ParseNumber(parts[1], v))
                            ip.RerollMax = static_cast<int>(v);
                        if (ip.RerollMax < ip.RerollMin)
                            ip.RerollMax = ip.RerollMin;
                    };

                    readInterval(ReadKey(pINI, target.c_str(), "TraitsRandomInterval"));
                    for (const auto& n2 : pool)
                    {
                        const auto t2 = traits.find(n2);
                        if (t2 != traits.end() && !t2->second.RerollInterval.empty())
                            readInterval(t2->second.RerollInterval);
                    }
                    if (ip.RerollMin > 0)
                        Debug::Log("[TraitExt] %s: look re-rolls every %d..%d frames\n",
                            target.c_str(), ip.RerollMin, ip.RerollMax);

                    int cloneIdx = 0;
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

                        // If this trait sets Image, synthesise a clone type so
                        // the look really can differ per unit. The clone
                        // $Inherits the target, so it matches in every other
                        // respect, and is registered in the same type list.
                        std::string cloneID;
                        for (const auto& e : it->second.Entries)
                        {
                            if (_stricmp(e.first.c_str(), "Image") != 0)
                                continue;

                            if (!artCapable(target))
                            {
                                Debug::Log("[TraitExt] WARN %s: trait '%s' sets Image, but "
                                    "variant art is VEHICLE-ONLY (the draw swap hooks "
                                    "UnitClass). %s is not a VehicleType, so its look cannot "
                                    "vary per unit - other keys still apply.\n",
                                    target.c_str(), n.c_str(), target.c_str());
                                break;
                            }

                            std::string art = e.second;
                            for (int hops = 0; hops < 8; ++hops)
                            {
                                const std::string nx = ReadKey(pINI, art.c_str(), "Image");
                                if (nx.empty() || nx == art) break;
                                art = nx;
                            }

                            char buf[24];
                            std::snprintf(buf, sizeof(buf), "%.16s$%d", target.c_str(), cloneIdx++);
                            cloneID = buf;

                            pINI->WriteString(cloneID.c_str(), "$Inherits", target.c_str());
                            pINI->WriteString(cloneID.c_str(), "Image", art.c_str());

                            FurnishClone(pINI, cloneID, it->second, e.second, target);

                            const auto lit = targetList.find(target);
                            if (lit == targetList.end())
                            {
                                Debug::Log("[TraitExt] WARN %s: no source list known, variant "
                                    "'%s' cannot be registered as a type\n",
                                    target.c_str(), cloneID.c_str());
                            }
                            else
                            {
                                const int n2 = pINI->GetKeyCount(lit->second.c_str());
                                char idx[16];
                                std::snprintf(idx, sizeof(idx), "%d", n2);
                                pINI->WriteString(lit->second.c_str(), idx, cloneID.c_str());

                                Debug::Log("[TraitExt] %s: variant '%s' (Image=%s) for '%s' "
                                    "-> [%s] %s=%s\n",
                                    target.c_str(), cloneID.c_str(), art.c_str(), n.c_str(),
                                    lit->second.c_str(), idx, cloneID.c_str());
                            }
                            break;
                        }
                        ip.CloneIDs.push_back(cloneID);
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
                std::string result = FoldKey(base, byKey[key], target.c_str(), key.c_str());

                // Image= must name an ART SECTION, not a unit ID. Many units
                // redirect their own art (MGTK -> RTNK, APOC -> MTNK), so a
                // trait saying Image=MGTK would point at a section that does not
                // exist and the unit renders as nothing with no cameo. Follow
                // the redirect so "look like a Mirage Tank" does what it says.
                if (!_stricmp(key.c_str(), "Image") && !result.empty())
                {
                    std::string art = result;
                    for (int hops = 0; hops < 8; ++hops)
                    {
                        const std::string next = ReadKey(pINI, art.c_str(), "Image");
                        if (next.empty() || next == art)
                            break;
                        art = next;
                    }
                    if (art != result)
                    {
                        Debug::Log("[TraitExt]   %s.Image '%s' redirects its own art -> using '%s'\n",
                            target.c_str(), result.c_str(), art.c_str());
                        result = art;
                    }
                }

                if (result == base)
                    continue;
                pINI->WriteString(target.c_str(), key.c_str(), result.c_str());
                Debug::Log("[TraitExt]   %s.%s: '%s' -> '%s'\n",
                    target.c_str(), key.c_str(), base.c_str(), result.c_str());

                // Setting Primary= on a unit that answers from the WeaponN list
                // writes cleanly and then does nothing, which reads in the log
                // as a successful change. The IFV is the worst case: Gunner=yes
                // with WeaponCount=17, so it picks by PASSENGER and Primary= is
                // never consulted. Say so rather than let the log look like it
                // worked.
                if (!_stricmp(key.c_str(), "Primary") || !_stricmp(key.c_str(), "Secondary"))
                {
                    const std::string wc = ReadKey(pINI, target.c_str(), "WeaponCount");
                    if (!wc.empty() && std::atoi(wc.c_str()) > 0)
                    {
                        const std::string g = ReadKey(pINI, target.c_str(), "Gunner");
                        const bool gunner = !g.empty()
                            && (g[0] == 'y' || g[0] == 'Y' || g[0] == 't'
                                || g[0] == 'T' || g[0] == '1');
                        Debug::Log("[TraitExt]   WARN %s: '%s' was written, but this type "
                            "is WeaponCount=%s%s, so the engine answers from its "
                            "Weapon1..N list and IGNORES %s. Set Weapon1= (the "
                            "default/empty slot) instead.\n",
                            target.c_str(), key.c_str(), wc.c_str(),
                            gunner ? " with Gunner=yes (weapon chosen by PASSENGER)" : "",
                            key.c_str());
                    }
                }

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
