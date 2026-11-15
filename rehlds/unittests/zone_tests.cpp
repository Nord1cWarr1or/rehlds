#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#ifdef REHLDS_FIXES

// Resident entries must live outside of the evictable cache: they are not linked
// into the LRU, they obey the budget and they give their memory back on Cache_Free.
TEST(ResidentEntryLifecycle, Zone, 1000)
{
	cache_user_t first = {};
	cache_user_t second = {};

	const int base = Cache_ResidentUsed();

	void *data = Cache_AllocResident(&first, 1000, "first", 4096);
	CHECK("a resident entry must be allocated within the budget", data != nullptr);
	CHECK("the user must point at the data", first.data == data);
	CHECK("the entry must be resident", Cache_IsResident(&first));
	CHECK("the usage must grow by at least the payload", Cache_ResidentUsed() >= base + 1000);

	// Cache_Check must not treat the entry as an LRU member (the LRU is not even initialised here)
	// and must not move the data.
	CHECK("Cache_Check must return the same data", Cache_Check(&first) == data);
	CHECK("Cache_Check must return the same data again", Cache_Check(&first) == data);

	// The second entry does not fit into what is left of the budget.
	CHECK("an entry over the budget must be refused", Cache_AllocResident(&second, 4000, "second", 4096) == nullptr);
	CHECK("a refused entry must stay unallocated", second.data == nullptr);

	// A zero or negative budget disables resident allocation.
	CHECK("a zero budget must refuse", Cache_AllocResident(&second, 16, "second", 0) == nullptr);

	Cache_Free(&first);
	CHECK("Cache_Free must clear the data", first.data == nullptr);
	CHECK("Cache_Free must return the memory to the budget", Cache_ResidentUsed() == base);

	// The freed budget is available again.
	CHECK("the budget must be reusable after a free", Cache_AllocResident(&second, 4000, "second", 4096) != nullptr);
	Cache_Free(&second);
	CHECK("the usage must return to the base", Cache_ResidentUsed() == base);
}

#endif // REHLDS_FIXES
