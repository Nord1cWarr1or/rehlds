#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#ifdef REHLDS_FIXES

// Gives back the resident data of the fake models and forgets them.
static void ResetKnownModels()
{
	for (int i = 0; i < mod_numknown; i++)
	{
		if (Cache_IsResident(&mod_known[i].cache))
		{
			Cache_Free(&mod_known[i].cache);
		}
	}

	Q_memset(mod_known, 0, sizeof(mod_known));
	Q_memset(mod_known_info, 0, sizeof(mod_known_info));
	mod_numknown = 0;
}

static model_t *AddFakeModel(const char *name, modtype_t type, int needload)
{
	model_t *mod = &mod_known[mod_numknown++];
	Q_strncpy(mod->name, name, sizeof(mod->name) - 1);
	mod->type = type;
	mod->needload = needload;
	return mod;
}

// A studio model whose data is already resident (as if it was loaded with enough budget).
static model_t *AddResidentStudio(const char *name, int needload, int size)
{
	model_t *mod = AddFakeModel(name, mod_studio, NL_PRESENT);
	if (!Cache_AllocResident(&mod->cache, size, mod->name, 0x7FFFFFFF))
	{
		return nullptr;
	}

	mod->needload = needload;
	return mod;
}

TEST(ResidentBudgetFromCvar, ResidentModels, 1000)
{
	const float saved = sv_rehlds_resident_models.value;

	struct testdata_t { float value; int budget; } testdata[] = {
		{ 0.0f, 0 },
		{ -5.0f, 0 },
		{ 1.0f, 1024 * 1024 },
		{ 1.5f, 1536 * 1024 },
		{ 2047.0f, 2047 * 1024 * 1024 },
		{ 100000.0f, 2047 * 1024 * 1024 }, // clamped, must not overflow
		{ NAN, 0 },
	};

	for (const auto &t : testdata)
	{
		sv_rehlds_resident_models.value = t.value;
		LONGS_EQUAL("cvar value to budget", t.budget, Mod_ResidentStudioBudget());
	}

	sv_rehlds_resident_models.value = saved;
}

TEST(ResidentFitsIntoBudget, ResidentModels, 1000)
{
	ResetKnownModels();
	const int base = Cache_ResidentUsed();
	LONGS_EQUAL("no resident data must be left by other tests", 0, base);

	model_t *mod = AddFakeModel("models/a.mdl", mod_studio, NL_PRESENT);
	CHECK("a model within the budget must become resident", Mod_AllocResidentStudio(mod, 1000, 10000));
	CHECK("the model data must be resident", Cache_IsResident(&mod->cache));
	LONGS_EQUAL("the usage must grow by the entry cost", Cache_ResidentCost(1000), Cache_ResidentUsed());
	LONGS_EQUAL("the entry size must match its cost", Cache_ResidentCost(1000), Cache_ResidentSize(&mod->cache));

	ResetKnownModels();
	LONGS_EQUAL("the usage must return to zero", 0, Cache_ResidentUsed());
}

TEST(ResidentDisabledBudget, ResidentModels, 1000)
{
	ResetKnownModels();
	model_t *stale = AddResidentStudio("models/stale.mdl", NL_UNREFERENCED, 1000);
	model_t *mod = AddFakeModel("models/a.mdl", mod_studio, NL_PRESENT);

	CHECK("a zero budget must refuse", !Mod_AllocResidentStudio(mod, 16, 0));
	CHECK("a negative budget must refuse", !Mod_AllocResidentStudio(mod, 16, -1));
	CHECK("a refused model must stay unallocated", mod->cache.data == nullptr);
	CHECK("a disabled budget must not free stale models", Cache_IsResident(&stale->cache));

	ResetKnownModels();
}

// A model that is larger than the whole budget must go to the cache without
// throwing out the stale models that could still be used by the next maps.
TEST(ResidentLargerThanBudget, ResidentModels, 1000)
{
	ResetKnownModels();
	model_t *stale1 = AddResidentStudio("models/stale1.mdl", NL_UNREFERENCED, 1000);
	model_t *stale2 = AddResidentStudio("models/stale2.mdl", NL_UNREFERENCED, 1000);
	const int used = Cache_ResidentUsed();

	model_t *huge = AddFakeModel("models/huge.mdl", mod_studio, NL_PRESENT);
	CHECK("a model larger than the budget must be refused", !Mod_AllocResidentStudio(huge, 20000, 10000));
	CHECK("the refused model must stay unallocated", huge->cache.data == nullptr);
	CHECK("stale model 1 must be kept", Cache_IsResident(&stale1->cache));
	CHECK("stale model 2 must be kept", Cache_IsResident(&stale2->cache));
	LONGS_EQUAL("the usage must not change", used, Cache_ResidentUsed());

	ResetKnownModels();
}

// Even freeing every stale model does not give enough room: nothing must be freed.
TEST(ResidentStaleNotEnough, ResidentModels, 1000)
{
	ResetKnownModels();
	model_t *present = AddResidentStudio("models/present.mdl", NL_PRESENT, 6000);
	model_t *stale = AddResidentStudio("models/stale.mdl", NL_UNREFERENCED, 1000);
	const int used = Cache_ResidentUsed();
	const int budget = used + 500; // the stale model frees ~1100 bytes, the new one needs ~4100

	model_t *mod = AddFakeModel("models/a.mdl", mod_studio, NL_PRESENT);
	CHECK("a model that cannot fit must be refused", !Mod_AllocResidentStudio(mod, 4000, budget));
	CHECK("the refused model must stay unallocated", mod->cache.data == nullptr);
	CHECK("the stale model must be kept", Cache_IsResident(&stale->cache));
	CHECK("the present model must be kept", Cache_IsResident(&present->cache));
	LONGS_EQUAL("the usage must not change", used, Cache_ResidentUsed());

	ResetKnownModels();
}

// The budget is taken by models of a previous map: just enough of them is freed,
// the models of the current map are never freed.
TEST(ResidentFreesStale, ResidentModels, 1000)
{
	ResetKnownModels();
	model_t *present = AddResidentStudio("models/present.mdl", NL_PRESENT, 1000);
	model_t *stale1 = AddResidentStudio("models/stale1.mdl", NL_UNREFERENCED, 4000);
	model_t *stale2 = AddResidentStudio("models/stale2.mdl", NL_UNREFERENCED, 4000);
	const int budget = Cache_ResidentUsed() + 100; // room for a new model only after one stale model is gone

	model_t *mod = AddFakeModel("models/a.mdl", mod_studio, NL_PRESENT);
	CHECK("the model must fit after freeing a stale one", Mod_AllocResidentStudio(mod, 4000, budget));
	CHECK("the new model must be resident", Cache_IsResident(&mod->cache));
	CHECK("the present model must be kept", Cache_IsResident(&present->cache));
	LONGS_EQUAL("exactly one stale model must be freed", 1,
		(int)Cache_IsResident(&stale1->cache) + (int)Cache_IsResident(&stale2->cache));
	CHECK("the usage must stay within the budget", Cache_ResidentUsed() <= budget);

	ResetKnownModels();
}

// The usage is over the budget (the cvar was lowered): the excess must be freed too.
TEST(ResidentLoweredBudget, ResidentModels, 1000)
{
	ResetKnownModels();
	model_t *stale1 = AddResidentStudio("models/stale1.mdl", NL_UNREFERENCED, 4000);
	model_t *stale2 = AddResidentStudio("models/stale2.mdl", NL_UNREFERENCED, 4000);
	const int budget = Cache_ResidentCost(4000) + 100;

	model_t *mod = AddFakeModel("models/a.mdl", mod_studio, NL_PRESENT);
	CHECK("the model must fit after freeing both stale models", Mod_AllocResidentStudio(mod, 4000, budget));
	CHECK("stale model 1 must be freed", !Cache_IsResident(&stale1->cache));
	CHECK("stale model 2 must be freed", !Cache_IsResident(&stale2->cache));
	LONGS_EQUAL("only the new model must be left", Cache_ResidentCost(4000), Cache_ResidentUsed());

	ResetKnownModels();
}

// With MAX_KNOWN_MODELS reached, a slot of an unreferenced model is given to the new name.
// The data of the previous model must not survive there: it would be served as the new model
// and, being resident, it would never be freed.
TEST(FindNameReusedSlotDropsData, ResidentModels, 1000)
{
	ResetKnownModels();

	char name[64];
	for (int i = 0; i < MAX_KNOWN_MODELS; i++)
	{
		Q_snprintf(name, sizeof(name), "models/m%d.mdl", i);
		if (i == 5)
		{
			AddResidentStudio(name, NL_UNREFERENCED, 1000);
		}
		else
		{
			AddFakeModel(name, mod_brush, NL_PRESENT);
		}
	}

	model_t *mod = Mod_FindName(FALSE, "models/new.mdl");
	CHECK("the only unreferenced slot must be reused", mod == &mod_known[5]);
	ZSTR_EQUAL("the slot must get the new name", "models/new.mdl", mod->name);
	CHECK("the data of the previous model must be dropped", mod->cache.data == nullptr);
	LONGS_EQUAL("the slot must need loading", NL_NEEDS_LOADED, mod->needload);
	LONGS_EQUAL("the resident budget must be given back", 0, Cache_ResidentUsed());

	ResetKnownModels();
}

#endif // REHLDS_FIXES
