#include "pch.h"
// cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /I <scratch-pch> tests\nr_depth_copy_ledger_smoke.cpp
#include <cstdio>
#include <cstring>

#if __has_include("../OptiScaler/native/DepthCopyLedger.h")
#include "../OptiScaler/native/DepthCopyLedger.h"
using native::DepthCopyLedger;
using native::DepthCopySlots;
constinit DepthCopyLedger constantLedger;
constinit DepthCopySlots constantSlots;
namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}
bool Is(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
constexpr uint64_t A = 0xA000, B = 0xB000, C = 0xC000, Pick = 0xD000, Other = 0xE000;
struct Finder
{
    DepthCopySlots slots;
    int Copy(uint64_t list, uint64_t vertices, uint64_t pick = Pick, bool reversed = true)
    {
        const int slot = slots.Take(list, [](int) { return true; });
        if (slot >= 0)
            slots.Recorded(slot, list, pick, vertices, reversed, slots.HandsOut());
        return slot;
    }
    void Close(uint64_t list)
    {
        if (slots.MayHold(list))
            slots.Closed(list);
    }
};
void Lists()
{
    Finder f;
    Check(!constantLedger.Find(A, Pick).valid && constantLedger.Frame() == 0 && constantSlots.HandsOut() &&
              constantSlots.Used() == 0 && !constantSlots.MayHold(A),
          "constant initialization, empty ledger");
    Check(!f.slots.Find(0, Pick).valid && !f.slots.MayHold(0), "null list");
    Check(f.Copy(A, 100) == 0 && f.Copy(B, 900) == 1 && f.Copy(A, 500) == 2 && f.Copy(A, 300) == 3,
          "each copy takes a distinct slot");
    auto a = f.slots.Find(A, Pick);
    Check(a.valid && a.slot == 2 && a.entry.vertices == 500 && a.entry.ordinal == 1 && a.copiesInList == 3 &&
              a.entry.reversed && a.entry.pick == Pick && a.entry.list == A && !a.acrossPresent,
          "interleaved lists keep their own biggest stretch and request id");
    Check(f.slots.Find(B, Pick).slot == 1 && !f.slots.Find(C, Pick).valid, "other list cannot borrow a copy");
    Check(f.Copy(A, 500) == 4 && f.slots.Find(A, Pick).slot == 4, "tie goes to the later copy");
    Check(Is(f.slots.Find(A, Other).why, "the list's copies are of a depth buffer no longer picked"),
          "a changed pick cannot read the old buffer");
    Check(f.Copy(A, 1, Other, false) == 5 && f.slots.Find(A, Other).entry.ordinal == 4 &&
              !f.slots.Find(A, Other).entry.reversed,
          "ordinal follows the list, not the pick");
    f.Close(A);
    Check(!f.slots.MayHold(A) && !f.slots.Find(A, Pick).valid && f.slots.Find(B, Pick).valid,
          "Close forgets only that list, including a reset without execute");
    Check(f.Copy(A, 10) == 6 && f.slots.Find(A, Pick).entry.ordinal == 0, "new recording starts at ordinal zero");
}
void Frames()
{
    Finder f;
    f.slots.NewFrame(7);
    f.Copy(A, 400);
    f.Copy(A, 100);
    f.slots.NewFrame(8);
    auto a = f.slots.Find(A, Pick);
    Check(a.valid && a.slot == 0 && a.entry.frame == 7 && a.acrossPresent && a.copiesInList == 2,
          "single-sample copy stays valid across a present");
    Check(f.Copy(B, 200) == 0, "next frame reuses slot zero");
    a = f.slots.Find(A, Pick);
    Check(a.valid && a.slot == 1 && a.entry.ordinal == 1 && a.acrossPresent && a.copiesInList == 1,
          "slot replacement forgets the displaced copy, not the other slot");
    f.Copy(C, 1);
    Check(!f.slots.Find(A, Pick).valid && !f.slots.MayHold(A), "both displaced slots leave no copy");
    f.slots.Remade(true);
    Check(f.slots.Used() == 0 && !f.slots.MayHold(B) && !f.slots.Find(C, Pick).valid,
          "new device or geometry forgets every slot");
    Check(f.Copy(A, 10) == 0 && f.slots.Find(A, Pick).entry.frame == 8, "remake preserves the frame counter");
}
void Exhaustion()
{
    Finder f;
    Check(f.slots.Take(A, [](int) { return false; }) == -1 && f.slots.Used() == 0, "failed allocation takes no slot");
    Check(f.slots.Take(A, [](int) { return true; }) == 0, "take an unrecorded slot");
    f.slots.GiveBack(0);
    f.slots.GiveBack(5);
    Check(f.slots.Used() == 0 && !f.slots.MayHold(A), "failed recording gives back only the last slot");
    for (int i = 0; i < DepthCopySlots::kSlots; ++i)
        Check(f.Copy(i % 2 ? B : A, 100 + i) == i, "eight slots in order");
    Check(f.Copy(C, 99999) == -1 && f.slots.MayHold(C) &&
              Is(f.slots.Find(C, Pick).why, "the frame's copy slots ran out") && f.slots.Find(A, Pick).slot == 6 &&
              f.slots.Find(B, Pick).slot == 7 && f.slots.Find(A, Pick).copiesInList == 4,
          "ninth copy cannot overwrite any reader's slot");
    f.slots.NewFrame(1);
    Check(f.slots.MayHold(C), "skip survives a present while its list stays open");
    f.Close(C);
    Check(!f.slots.MayHold(C) && Is(f.slots.Find(C, Pick).why, "no copy in this list this frame"),
          "Close forgets a skip");
}
void Pending()
{
    DepthCopyLedger ledger;
    ledger.Record(0, A, Pick, 400, false, false);
    Check(!ledger.Find(A, Pick).valid && Is(ledger.Find(A, Pick).why, "resolve pending") &&
              ledger.Find(A, Pick).copiesInList == 1,
          "unresolved multisample copy is not readable");
    ledger.NewFrame(1);
    Check(Is(ledger.Find(A, Pick).why, "the list's copy was taken before the last present"),
          "pending copy keeps its frame test");
    Finder f;
    f.slots.Remade(false);
    for (int i = 0; i <= DepthCopySlots::kSlots; ++i)
        f.Copy(A, 1);
    Check(!f.slots.HandsOut() && !f.slots.MayHold(A) && !f.slots.Find(A, Pick).valid &&
              f.slots.Ledger().At(0).list == 0,
          "present-time resolve enters no ledger or skip: Close need not lock");
    f.slots.Remade(true);
    Check(f.Copy(A, 1) == 0 && f.slots.Find(A, Pick).valid, "single-sample slots hand copies out again");
}
} // namespace
int main()
{
    Lists();
    Frames();
    Exhaustion();
    Pending();
    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
#else
int main()
{
    std::printf("FAIL depth copy ledger is not available\n");
    return 1;
}
#endif
