#pragma once

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

struct ID3D12GraphicsCommandList;

namespace native
{
namespace detail
{
inline constinit std::atomic_flag g_listVtableWrites[1024] {};
}
template <typename Tag> class ListVtables
{
  public:
    static constexpr int kSlots = 81;
    struct Counts
    {
        uint32_t tables, patched, repatched, foreign, failed;
    };

    void Install(void* const* standardTable)
    {
        _install = standardTable;
        if (_slab == nullptr)
            _slab = static_cast<unsigned char*>(VirtualAlloc(nullptr, kBytes, MEM_RESERVE, PAGE_READWRITE));
        if (_slab == nullptr)
            Fail("command-list vtable storage could not be reserved");
    }
    void Hook(int slot, void* thunk)
    {
        if (slot < 0 || slot >= kSlots || _install == nullptr || thunk == nullptr)
        {
            Fail("invalid command-list vtable hook");
            return;
        }
        if (_thunks[slot] == nullptr)
            _slots[_hookCount++] = slot;
        _thunks[slot] = thunk;
        _installEntries[slot] = _install[slot];
    }
    void SetLog(void (*log)(const char*)) { _log = log; }

    bool Examine(ID3D12GraphicsCommandList* list) { return Examine(list, nullptr); }
    bool Examine(ID3D12GraphicsCommandList* list, bool* newlyPatched)
    {
        if (newlyPatched != nullptr)
            *newlyPatched = false;
        void** table = Table(list);
        if (_install == nullptr || table == _install)
            return true;
        if (table == nullptr)
            return Fail("a command list has no vtable");
        Record* record = Find(table);
        if (record == nullptr)
            record = Insert(table);
        if (record == nullptr)
            return Fail("command-list vtable storage is full or could not be committed");

        bool needsPatch = false;
        for (int i = 0; i < _hookCount; ++i)
        {
            const int slot = _slots[i];
            void* entry = Entry(table, slot);
            if (entry == nullptr)
                return Fail("a command-list vtable entry is missing");
            if (entry == _thunks[slot] || entry == _installEntries[slot])
                continue;
            void* previous = record->previous[slot].load(std::memory_order_acquire);
            if (previous == nullptr || entry == previous)
                needsPatch = true;
            else
                Foreign(*record);
        }
        if (!needsPatch)
            return true;

        // Another recorder can be publishing these originals. Never wait, and never overwrite its chain.
        if (record->updating.test_and_set(std::memory_order_acquire))
            return Fail("a command-list vtable is being patched by another recorder");
        bool complete = true, changed = false;
        for (int i = 0; i < _hookCount; ++i)
        {
            const int slot = _slots[i];
            void* entry = Entry(table, slot);
            if (entry == _thunks[slot] || entry == _installEntries[slot])
                continue;
            void* previous = record->previous[slot].load(std::memory_order_acquire);
            // A different entry after our patch is an outer hook. Wrapping it would call ourselves in a cycle.
            if (previous != nullptr && entry != previous)
            {
                Foreign(*record);
                continue;
            }
            // VirtualProtect changes a whole page. Another table or client on it must not restore read-only access
            // while we are writing. Hash collisions refuse this attempt too; no recording thread waits.
            auto& page = detail::g_listVtableWrites[(reinterpret_cast<uintptr_t>(&table[slot]) >> 12) & 1023];
            if (page.test_and_set(std::memory_order_acquire))
            {
                complete = Fail("a command-list vtable page is being patched by another recorder");
                continue;
            }
            DWORD protection = 0;
            if (entry == nullptr || !VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &protection))
            {
                page.clear(std::memory_order_release);
                complete = Fail("a command-list vtable entry could not be made writable");
                continue;
            }
            // Publish before the thunk: a call arriving between these writes must already find its original.
            record->previous[slot].store(entry, std::memory_order_release);
            const char* failed = nullptr;
            if (InterlockedCompareExchangePointer(&table[slot], _thunks[slot], entry) == entry)
                changed = true;
            else
            {
                record->previous[slot].store(previous, std::memory_order_release);
                failed = "a command-list vtable entry changed while being patched";
            }
            DWORD ignored = 0;
            if (!VirtualProtect(&table[slot], sizeof(void*), protection, &ignored))
                failed = "a command-list vtable entry's protection could not be restored";
            page.clear(std::memory_order_release);
            if (failed != nullptr)
                complete = Fail(failed);
        }
        if (changed)
        {
            if (record->patched)
                _repatched.fetch_add(1, std::memory_order_relaxed);
            else
            {
                record->patched = true;
                _patched.fetch_add(1, std::memory_order_relaxed);
                if (newlyPatched != nullptr)
                    *newlyPatched = true;
            }
        }
        record->updating.clear(std::memory_order_release);
        return complete;
    }

    void* Original(ID3D12GraphicsCommandList* list, int slot, void* trampoline) const
    {
        void** table = Table(list);
        if (table == nullptr || table == _install || slot < 0 || slot >= kSlots)
            return trampoline;
        if (const Record* record = Find(table))
        {
            void* previous = record->previous[slot].load(std::memory_order_acquire);
            if (previous != nullptr && previous != _installEntries[slot])
                return previous;
        }
        return trampoline;
    }
    template <typename Function> Function Original(ID3D12GraphicsCommandList* list, int slot, Function trampoline) const
    {
        return reinterpret_cast<Function>(Original(list, slot, reinterpret_cast<void*>(trampoline)));
    }
    Counts TakeCounts()
    {
        return { _tables.load(std::memory_order_relaxed), _patched.load(std::memory_order_relaxed),
                 _repatched.exchange(0, std::memory_order_relaxed), _foreign.exchange(0, std::memory_order_relaxed),
                 _failed.exchange(0, std::memory_order_relaxed) };
    }

  private:
    struct Record
    {
        void** table = nullptr;
        Record* next = nullptr;
        std::atomic<void*> previous[kSlots] {};
        std::atomic_flag updating = ATOMIC_FLAG_INIT;
        std::atomic<bool> foreign { false };
        bool patched = false;
    };
    static constexpr size_t kBytes = 64 * 1024 * 1024, kBuckets = 1024;
    static_assert(std::atomic<void*>::is_always_lock_free && std::atomic<Record*>::is_always_lock_free);
    static void** Table(ID3D12GraphicsCommandList* list)
    {
        return list != nullptr ? *reinterpret_cast<void***>(list) : nullptr;
    }
    static void* Entry(void** table, int slot)
    {
        return std::atomic_ref<void*>(table[slot]).load(std::memory_order_acquire);
    }
    static size_t Hash(void** table)
    {
        return static_cast<size_t>(
                   (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(table)) >> 3) * 0x9E3779B97F4A7C15ull >> 40) &
               (kBuckets - 1);
    }
    Record* Find(void** table) const
    {
        for (Record* record = _buckets[Hash(table)].load(std::memory_order_acquire); record != nullptr;
             record = record->next)
            if (record->table == table)
                return record;
        return nullptr;
    }
    Record* Insert(void** table)
    {
        if (_slab == nullptr)
            return nullptr;
        const size_t index = _allocated.fetch_add(1, std::memory_order_relaxed);
        if (index >= kBytes / sizeof(Record))
            return nullptr;
        void* storage = _slab + index * sizeof(Record);
        if (VirtualAlloc(storage, sizeof(Record), MEM_COMMIT, PAGE_READWRITE) == nullptr)
            return nullptr;
        Record* added = new (storage) Record;
        added->table = table;
        auto& bucket = _buckets[Hash(table)];
        Record* head = bucket.load(std::memory_order_acquire);
        do
        {
            for (Record* record = head; record != nullptr; record = record->next)
                if (record->table == table)
                    return record;
            added->next = head;
        } while (!bucket.compare_exchange_weak(head, added, std::memory_order_release, std::memory_order_acquire));
        _tables.fetch_add(1, std::memory_order_relaxed);
        return added;
    }
    bool Fail(const char* why)
    {
        _failed.fetch_add(1, std::memory_order_relaxed);
        if (!_failureSaid.exchange(true, std::memory_order_relaxed))
            Warn(why);
        return false;
    }
    void Foreign(Record& record)
    {
        if (!record.foreign.load(std::memory_order_relaxed) &&
            !record.foreign.exchange(true, std::memory_order_relaxed))
            _foreign.fetch_add(1, std::memory_order_relaxed);
        if (!_foreignSaid.load(std::memory_order_relaxed) && !_foreignSaid.exchange(true, std::memory_order_relaxed))
            Warn("another command-list vtable patcher wraps our hook; its chain is left intact");
    }
    void Warn(const char* why) const
    {
        if (_log != nullptr)
            _log(why);
        else
            OutputDebugStringA(why);
    }

    // Records outlive the lists: an outer patcher may retain a pointer to our thunk and still need its original.
    unsigned char* _slab = nullptr;
    void* const* _install = nullptr;
    void *_installEntries[kSlots] {}, *_thunks[kSlots] {};
    int _slots[kSlots] {}, _hookCount = 0;
    void (*_log)(const char*) = nullptr;
    std::atomic<Record*> _buckets[kBuckets] {};
    std::atomic<size_t> _allocated { 0 };
    std::atomic<uint32_t> _tables { 0 }, _patched { 0 }, _repatched { 0 }, _foreign { 0 }, _failed { 0 };
    std::atomic<bool> _failureSaid { false }, _foreignSaid { false };
};

// A private entry can forward into the detoured runtime function on the same object. Only the table path looks up its
// previous entry; the code-detour path must use its own trampoline or the forwarding call would loop.
template <auto& Tables, int Slot, auto Original, auto Body> struct ListVtableHook;

template <auto& Tables, int Slot, typename Result, typename This, typename... Args,
          Result(STDMETHODCALLTYPE** Original)(This*, Args...),
          Result(STDMETHODCALLTYPE* Body)(Result(STDMETHODCALLTYPE*)(This*, Args...), This*, Args...)>
struct ListVtableHook<Tables, Slot, Original, Body>
{
    static Result STDMETHODCALLTYPE Detour(This* list, Args... args) { return Body(*Original, list, args...); }
    static Result STDMETHODCALLTYPE Table(This* list, Args... args)
    {
        return Body(Tables.Original(reinterpret_cast<ID3D12GraphicsCommandList*>(list), Slot, *Original), list,
                    args...);
    }
};
} // namespace native
