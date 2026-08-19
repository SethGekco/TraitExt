// P2 groundwork: the trait/random injection seam.
//
// 0x668BF0 = RulesClass::Read_File(CCINIClass*) ENTRY (YRpp identity; the
// framework name "RulesClass_Addition" is misleading). Binary-verified
// (encyclopedia/Rules-Load.md): RulesClass::Init calls this 3x for the initial
// rulesmd.ini, and it fires again for the game-mode INI and the map INI — so
// this ONE cooperative hook sees EVERY rules pass. Anything written into pINI
// here is visible to the vanilla section readers that follow, and to Phobos's
// lazy $Inherits resolution (0x528BAC) on first read of a section.
//
// Phase 1 (this file): proof of life — log each pass so the in-game debug.log
// shows the seam firing and chaining cleanly beside Phobos's return-0 hook at
// the same address.
//
// SEED CAVEAT for later logical randoms: the three Init passes run at program
// init, before the scenario's synced seed — Logical per-type rolls must happen
// only in scenario-load passes (or at LoadTypesFromINI time). Cosmetic rolls
// may happen in any pass.

#include "TraitExt.h"

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

    const bool isRulesINI = (pINI == &CCINIClass::INI_Rules.get());
    Debug::Log("[TraitExt] Read_File pass %d: pRules=%p pINI=%p%s\n",
        TraitInjection::PassCount, pRules, pINI,
        isRulesINI ? " (INI_Rules)" : "");

    // TODO(P2): resolve [TraitTypes], roll random pools (cosmetic only until
    // seed timing is verified), and write chosen $Inherits/keys into pINI here.

    return 0;
}
