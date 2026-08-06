/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2024, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Blitzy Agent <agent@blitzy.com>
 *
 * staticinit-probe.c -- OOS-9 static-initialisation probe
 *
 */

/* OOS-9 static-initialisation probe.
 *
 * Mirrors switch_dso_open() exactly - dlopen(path, RTLD_NOW | RTLD_LOCAL), the
 * mode FreeSWITCH uses for a module declared with SMODF_NONE - so that dlopen
 * static initialisation of BOTH endpoint modules (and of both PTLib runtimes
 * they pull in) can be observed in isolation from any module code.  No module
 * entry point is called.
 *
 * EXACTLY TWO module paths are required.  The verdict this probe prints is a
 * statement about two modules sharing one process, so a run given fewer than
 * two could otherwise print that verdict without ever having tested it: one
 * PTLib runtime mapped alone never conflicts with anything.  A missing or
 * surplus argument is therefore a usage failure, not a shorter run.
 *
 * Exit status:
 *   0  both modules mapped, no fault during static initialisation
 *   1  a dlopen failed (the path is reported with the loader's own diagnosis)
 *   2  usage error - this probe was not given exactly two module paths
 */
#include <dlfcn.h>
#include <stdio.h>

#define STATICINIT_PROBE_MODULES 2

static int open_one(const char *path)
{
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);

	if (!h) {
		printf("dlopen FAILED %s: %s\n", path, dlerror());
		return 1;
	}

	printf("dlopen OK (static initialisation complete) %s\n", path);
	return 0;
}

int main(int argc, char **argv)
{
	int i;

	if (argc != STATICINIT_PROBE_MODULES + 1) {
		fprintf(stderr, "usage: %s <module-1.so> <module-2.so>\n", argv[0]);
		fprintf(stderr, "       exactly %d module paths are required: this probe reports whether TWO\n", STATICINIT_PROBE_MODULES);
		fprintf(stderr, "       modules survive static initialisation in one process\n");
		return 2;
	}

	for (i = 1; i < argc; i++) {
		if (open_one(argv[i])) {
			return 1;
		}
	}

	printf("both modules mapped into ONE process; no fault during static initialisation\n");
	return 0;
}
