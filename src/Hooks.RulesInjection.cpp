// The trait resolution seam.
//
// 0x668BF0 = RulesClass::Read_File(CCINIClass*) ENTRY (YRpp identity; the
// framework name "RulesClass_Addition" is misleading). Binary-verified
// (encyclopedia/Rules-Load.md): RulesClass::Init calls this 3x for the initial
// rulesmd.ini, and it fires again for the game-mode INI and the map INI — so
// this ONE cooperative hook sees EVERY rules pass. Anything written into pINI
// here is visible to the vanilla section readers that follow, and to Phobos's
// lazy $Inherits resolution (0x528BAC) on first read of a section.
//
// Because it fires repeatedly, the engine guards itself with a per-INI applied
// flag — folding Add/Multiply more than once would compound silently.
//
// SEED CAVEAT for later per-instance logical randoms: the three Init passes run
// at program init, before the scenario's synced seed exists. Load-time random
// here is therefore DETERMINISTIC (seeded from [TraitExt] RandomSeed + the
// section name), which is sync-safe by construction: every client reads the same
// INI and draws the same picks.

#include "TraitExt.h"
#include "TraitEngine.h"

#include <Phobos.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>
#include <RulesClass.h>
#include <CCINIClass.h>

namespace TraitInjection
{
    int PassCount = 0;
}

DEFINE_HOOK(0x668BF0, RulesClass_ReadFile_TraitInjection, 0x5)
{
    GET(RulesClass*, pRules, ECX);
    GET_STACK(CCINIClass*, pINI, 0x4);

    ++TraitInjection::PassCount;

    const bool isRulesINI = (pINI == CCINIClass::INI_Rules);
    Debug::Log("[TraitExt] Read_File pass %d: pRules=%p pINI=%p%s\n",
        TraitInjection::PassCount, pRules, pINI,
        isRulesINI ? " (INI_Rules)" : "");

    TraitExt::Engine::ProcessINI(pINI);

    return 0;
}

// 0x679CAF = RulesData::LoadAfterTypeData epilogue (verified: 5F / 5E / B0 01 /
// 5B is exactly 5 bytes, matching the size all frameworks declare here). Types
// exist by now, so this is where a cameo clobbered by an Image= redirect can be
// put back. Cooperative return 0 — Antares and Phobos also sit on this address.
DEFINE_HOOK(0x679CAF, RulesData_LoadAfterTypeData_TraitCameoFix, 0x5)
{
    TraitExt::CameoFix::Apply();
    return 0;
}
