#!/usr/bin/env python3
"""Fail the build on an INI key that is parsed and then never used.

Written because of a real bug: RequireMission= parsed fine, resolved its mission
names to enum values, stored them on the ConditionalTrait -- and nothing ever
compared them. The gate registered as runtime-evaluated (correctly escaping the
static fold) and then never closed, applying the trait permanently. In game that
is indistinguishable from a different bug already fixed in the same file.

None of it is visible from outside: it compiles, it logs, the INI looks right,
and a staged test "for" the feature looks like evidence the feature exists. The
cheap catch is to walk the chain mechanically:

    INI key  ->  TraitDef field  ->  ConditionalTrait field  ->  compared

Checks:
  1. every reserved key parses into a TraitDef field that something reads
  2. every ConditionalTrait member assigned at load is read somewhere
  3. every reserved key is documented in docs/KEYS.md

Blind spot deliberately avoided: the first version of this sweep looked for
consumers only in Hooks.InstanceRandom.cpp and produced five false positives,
because proximity is consumed in NearMeets over in TraitEngine.cpp. Runtime code
is split across files, so consumers are searched for everywhere.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Keys read into a local at the parse site rather than into a TraitDef field.
# Listed explicitly so a NEW key cannot silently join them.
LOCAL_OK = {"Merge", "NearRange", "Weight", "ForceBodyFacing", "Cameo", "AltCameo"}


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    eng = read("src/TraitEngine.cpp")
    hdr = read("src/TraitEngine.h")
    docs = read("docs/KEYS.md")

    allsrc = "\n".join(
        read(os.path.join("src", n))
        for n in sorted(os.listdir(os.path.join(ROOT, "src")))
        if n.endswith((".cpp", ".h"))
    )

    problems = []

    m = re.search(r"kReservedTraitKeys\[\] = \{(.*?)\};", eng, re.S)
    if not m:
        print("check-keys: cannot find kReservedTraitKeys", file=sys.stderr)
        return 2
    reserved = set(re.findall(r'"([^"]+)"', m.group(1)))

    field_of = {}
    for mm in re.finditer(
        r'def\.(\w+)\s*=\s*(?:SplitCSV\()?ReadKey\(pINI, name\.c_str\(\), "(\w+)"', eng
    ):
        field_of[mm.group(2)] = mm.group(1)

    for key in sorted(reserved):
        fld = field_of.get(key)
        if fld is None:
            if key not in LOCAL_OK:
                problems.append(
                    "key '%s' is reserved but never parsed into a TraitDef field "
                    "(add it to LOCAL_OK if it is read into a local)" % key
                )
            continue
        uses = len(re.findall(r"(?:def|d|kv\.second|it->second)\.%s\b" % fld, allsrc))
        uses += len(re.findall(r"\bdef->%s\b" % fld, allsrc))
        if uses <= 1:
            problems.append(
                "key '%s' parses into TraitDef::%s and nothing reads it" % (key, fld)
            )

    cm = re.search(r"struct ConditionalTrait\s*\{(.*?)\n    \};", hdr, re.S)
    if not cm:
        print("check-keys: cannot find struct ConditionalTrait", file=sys.stderr)
        return 2

    members = re.findall(
        r"^\s*(?:std::vector<std::string>|std::vector<int>|int|bool|Whose|Power|"
        r"std::string|const TraitDef\*)\s+(\w+)\s*(?:=|;)",
        cm.group(1),
        re.M,
    )

    # The REGISTRATION span is where ct.* members are populated. It must be
    # excluded before searching for readers, or population masquerades as a
    # read: ct.Missions.push_back(...) is neither an assignment nor a compare,
    # so a naive search finds it and the check passes with NO runtime consumer.
    # Verified by fault injection - deleting the real mission check has to make
    # this script fail, and with the span included it did not.
    reg = re.search(r"ConditionalTrait ct;(.*?)Conditional::Register", eng, re.S)
    consumers = allsrc.replace(reg.group(1), "") if reg else allsrc

    for fld in members:
        if not re.search(r"\bct\.%s\s*=|\bct\.%s\.push_back" % (fld, fld), eng):
            continue  # never populated at load: a plain default, fine
        # (?!\s*=[^=]) excludes an assignment but keeps a COMPARISON. An earlier
        # version used (?!\s*=) and so read "ct.X ==" as an assignment,
        # reporting PowerMeets' own read as missing.
        if not re.search(r"\b(?:ct|c)\.%s\b(?!\s*=[^=])" % fld, consumers):
            problems.append(
                "ConditionalTrait::%s is assigned at load and never compared at "
                "runtime - that gate would never close" % fld
            )

    for key in sorted(reserved):
        if not re.search(r"`%s=" % re.escape(key), docs):
            problems.append("key '%s' is not documented in docs/KEYS.md" % key)

    if problems:
        print("check-keys: FAILED\n", file=sys.stderr)
        for p in problems:
            print("  * %s" % p, file=sys.stderr)
        print(
            "\n%d problem(s). A key that parses but is never consumed is invisible "
            "in game: it compiles, it logs, and the feature silently does nothing - "
            "or never stops doing it." % len(problems),
            file=sys.stderr,
        )
        return 1

    print(
        "check-keys: OK - %d reserved keys, %d ConditionalTrait members, all "
        "parsed, consumed and documented" % (len(reserved), len(members))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
