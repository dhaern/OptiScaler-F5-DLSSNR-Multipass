#pragma once

#include <atomic>
#include <cstdint>

namespace native
{
// One direct queue orders each slot's barriers and readers. An open list may keep its single-sample copy across a
// present, until another copy takes that slot. Close ends the recording, including Reset without execution.
// Calls require the finder's lock except HandsOut and MayHold; neither needs TLS or dynamically initialized statics.
class DepthCopyLedger
{
  public:
    static constexpr int kSlots = 8;
    struct Entry
    {
        uint64_t list = 0, frame = 0;
        uint32_t ordinal = 0;
        uint64_t vertices = 0, pick = 0;
        bool reversed = false, singleSample = false;
    };
    struct Found
    {
        bool valid = false;
        const char* why = "";
        int slot = -1;
        Entry entry;
        uint32_t copiesInList = 0;
        bool acrossPresent = false;
    };
    void NewFrame(uint64_t frame) { _frame = frame; }
    uint64_t Frame() const { return _frame; }
    uint32_t Record(int slot, uint64_t list, uint64_t pick, uint64_t vertices, bool reversed, bool singleSample)
    {
        if (slot < 0 || slot >= kSlots || list == 0)
            return 0;
        uint32_t ordinal = 0;
        for (int i = 0; i < kSlots; ++i)
            if (i != slot && _entries[i].list == list && _entries[i].ordinal + 1 > ordinal)
                ordinal = _entries[i].ordinal + 1;
        _entries[slot] = Entry { list, _frame, ordinal, vertices, pick, reversed, singleSample };
        _lists[slot].store(list, std::memory_order_relaxed);
        return ordinal;
    }
    void NoteSkipped(uint64_t list)
    {
        if (list == 0)
            return;
        int at = -1;
        for (int i = 0; i < kSlots && at < 0; ++i)
            if (_skipped[i].load(std::memory_order_relaxed) == list)
                at = i;
        for (int i = 0; i < kSlots && at < 0; ++i)
            if (_skipped[i].load(std::memory_order_relaxed) == 0)
                at = i;
        if (at < 0)
        {
            at = _skippedNext;
            _skippedNext = (_skippedNext + 1) % kSlots;
        }
        _skipped[at].store(list, std::memory_order_relaxed);
    }
    void Forget(uint64_t list)
    {
        for (int i = 0; i < kSlots; ++i)
        {
            if (_entries[i].list == list)
            {
                _entries[i] = Entry {};
                _lists[i].store(0, std::memory_order_relaxed);
            }
            if (_skipped[i].load(std::memory_order_relaxed) == list)
                _skipped[i].store(0, std::memory_order_relaxed);
        }
    }
    void Clear()
    {
        for (int i = 0; i < kSlots; ++i)
        {
            _entries[i] = Entry {};
            _lists[i].store(0, std::memory_order_relaxed);
            _skipped[i].store(0, std::memory_order_relaxed);
        }
    }
    bool MayHold(uint64_t list) const
    {
        if (list == 0)
            return false;
        for (int i = 0; i < kSlots; ++i)
            if (_lists[i].load(std::memory_order_relaxed) == list ||
                _skipped[i].load(std::memory_order_relaxed) == list)
                return true;
        return false;
    }
    Found Find(uint64_t list, uint64_t pick) const
    {
        Found found;
        uint32_t pending = 0, older = 0, otherPick = 0;
        bool skipped = false;
        for (int i = 0; i < kSlots; ++i)
        {
            skipped = skipped || (list != 0 && _skipped[i].load(std::memory_order_relaxed) == list);
            const Entry& e = _entries[i];
            if (list == 0 || e.list != list)
                continue;
            if (e.pick != pick)
            {
                ++otherPick;
                continue;
            }
            // A present-time MSAA resolve does not make an earlier recording readable here.
            if (!e.singleSample)
            {
                if (e.frame != _frame)
                    ++older;
                else
                {
                    ++found.copiesInList;
                    ++pending;
                }
                continue;
            }
            ++found.copiesInList;
            if (found.slot < 0 || e.vertices > found.entry.vertices ||
                (e.vertices == found.entry.vertices && e.ordinal > found.entry.ordinal))
            {
                found.slot = i;
                found.entry = e;
            }
        }
        if (found.slot >= 0)
        {
            found.valid = true;
            found.acrossPresent = found.entry.frame != _frame;
        }
        else if (pending)
            found.why = "resolve pending";
        else if (older)
            found.why = "the list's copy was taken before the last present";
        else if (otherPick)
            found.why = "the list's copies are of a depth buffer no longer picked";
        else if (skipped)
            found.why = "the frame's copy slots ran out";
        else
            found.why = "no copy in this list this frame";
        return found;
    }
    void Resolved(int slot) { _entries[slot].singleSample = true; }
    const Entry& At(int slot) const { return _entries[slot]; }

  private:
    uint64_t _frame = 0;
    Entry _entries[kSlots] {};
    std::atomic<uint64_t> _lists[kSlots] {}, _skipped[kSlots] {};
    int _skippedNext = 0;
};

class DepthCopySlots
{
  public:
    static constexpr int kSlots = DepthCopyLedger::kSlots;
    template <class Make> int Take(uint64_t list, Make&& make)
    {
        while (_used < kSlots && _pending[_used].load(std::memory_order_relaxed) != 0)
            ++_used;
        if (_used >= kSlots)
        {
            if (HandsOut())
                _ledger.NoteSkipped(list);
            return -1;
        }
        if (!make(_used))
            return -1;
        _taken |= 1u << _used;
        return _used++;
    }
    void GiveBack(int slot)
    {
        if (slot >= 0 && slot < kSlots)
            _taken &= ~(1u << slot);
        if (slot >= 0 && slot == _used - 1)
            --_used;
    }
    void Recorded(int slot, uint64_t list, uint64_t pick, uint64_t vertices, bool reversed, bool singleSample)
    {
        if (HandsOut())
            _ledger.Record(slot, list, pick, vertices, reversed, singleSample);
    }
    void NewFrame(uint64_t frame)
    {
        _used = 0;
        _taken = 0;
        _ledger.NewFrame(frame);
    }
    // A deferred MSAA resolve may wait under the finder lock: its lists never enter the ledger, so their Close hooks
    // and CopyThisFrame can stay off that lock altogether.
    void Remade(bool handOut)
    {
        _used = 0;
        _taken = 0;
        for (auto& pending : _pending)
            pending.store(0, std::memory_order_relaxed);
        _ledger.Clear();
        _handOut.store(handOut, std::memory_order_release);
    }
    bool HandsOut() const { return _handOut.load(std::memory_order_acquire); }
    bool MayHold(uint64_t list) const { return HandsOut() && _ledger.MayHold(list); }
    void Closed(uint64_t list) { _ledger.Forget(list); }
    DepthCopyLedger::Found Find(uint64_t list, uint64_t pick) const { return _ledger.Find(list, pick); }
    int Pending(uint64_t list) const
    {
        for (int i = 0; i < kSlots; ++i)
            if (list != 0 && _pending[i].load(std::memory_order_relaxed) == list)
                return i;
        return -1;
    }
    void Pending(int slot, uint64_t list) { _pending[slot].store(list, std::memory_order_relaxed); }
    bool Current(int slot) const { return (_taken & (1u << slot)) && !_pending[slot].load(std::memory_order_relaxed); }
    void Resolved(int slot) { _ledger.Resolved(slot); }
    int Used() const { return _used; }
    const DepthCopyLedger& Ledger() const { return _ledger; }

  private:
    int _used = 0;
    unsigned _taken = 0;
    std::atomic<uint64_t> _pending[kSlots] {};
    DepthCopyLedger _ledger;
    std::atomic<bool> _handOut { true };
};
} // namespace native
