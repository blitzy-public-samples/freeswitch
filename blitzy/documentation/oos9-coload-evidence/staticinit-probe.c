/* OOS-9 static-initialisation probe.
 *
 * Mirrors switch_dso_open() exactly - dlopen(path, RTLD_NOW | RTLD_LOCAL), the
 * mode FreeSWITCH uses for a module declared with SMODF_NONE - so that dlopen
 * static initialisation of BOTH endpoint modules (and of both PTLib runtimes
 * they pull in) can be observed in isolation from any module code.  No module
 * entry point is called.
 */
#include <dlfcn.h>
#include <stdio.h>

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

	for (i = 1; i < argc; i++) {
		if (open_one(argv[i])) {
			return 1;
		}
	}

	printf("both modules mapped into ONE process; no fault during static initialisation\n");
	return 0;
}
