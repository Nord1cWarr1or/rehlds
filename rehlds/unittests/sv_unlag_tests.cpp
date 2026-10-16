#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#ifdef REHLDS_FIXES

TEST(UnlagForClient, SvUnlag, 1000) {
	static client_t cl;
	Q_memset(&cl, 0, sizeof(cl));
	float saved = sv_unlag.value;

	// sv_unlag 1: the client decides with cl_lw and cl_lc
	sv_unlag.value = 1.0f;
	cl.lw = TRUE; cl.lc = TRUE;
	CHECK("lw 1 lc 1 compensated", SV_UnlagForClient(&cl));
	cl.lc = FALSE;
	CHECK("cl_lc 0 not compensated", !SV_UnlagForClient(&cl));
	cl.lc = TRUE; cl.lw = FALSE;
	CHECK("cl_lw 0 not compensated", !SV_UnlagForClient(&cl));

	// other non-zero values keep behaving like 1
	sv_unlag.value = 3.0f;
	CHECK("sv_unlag 3 respects cl_lw 0", !SV_UnlagForClient(&cl));

	// sv_unlag 2: every client, whatever it asks for
	sv_unlag.value = 2.0f;
	cl.lw = FALSE; cl.lc = FALSE;
	CHECK("forced for cl_lw 0 cl_lc 0", SV_UnlagForClient(&cl));
	cl.lw = TRUE;
	CHECK("forced for cl_lc 0", SV_UnlagForClient(&cl));

	sv_unlag.value = saved;
}

#endif // REHLDS_FIXES
