// KatarnEngine - crash report: writes katarn_crash.txt (exception, faulting module + offset, call stack)
#include <windows.h>
#include <stdio.h>

namespace kt {

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
	FILE* f = fopen("katarn_crash.txt", "w");
	if (!f) return EXCEPTION_CONTINUE_SEARCH;
	auto where = [&](void* addr) {
		HMODULE m = nullptr;
		char name[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
			GetModuleFileNameA(m, name, sizeof(name));
			const char* s = strrchr(name, '\\');
			fprintf(f, "  %s+0x%llx\n", s ? s + 1 : name, (unsigned long long)((char*)addr - (char*)m));
		}
		else fprintf(f, "  %p\n", addr);
	};
	fprintf(f, "KatarnEngine crash: exception 0x%08lx at\n", ep->ExceptionRecord->ExceptionCode);
	where(ep->ExceptionRecord->ExceptionAddress);
	if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
		fprintf(f, "access violation %s address %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (void*)ep->ExceptionRecord->ExceptionInformation[1]);
	// walk the stack from the faulting context
	CONTEXT ctx = *ep->ContextRecord;
	fprintf(f, "stack:\n");
	for (int i = 0; i < 48; i++) {
		DWORD64 imageBase = 0;
		PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
		where((void*)ctx.Rip);
		if (!fn) {
			// leaf function: return address is at rsp
			ctx.Rip = *(DWORD64*)ctx.Rsp;
			ctx.Rsp += 8;
		}
		else {
			void* handlerData = nullptr; DWORD64 establisher = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
		}
		if (!ctx.Rip) break;
	}
	fclose(f);
	return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler()
{
	static bool done = false;
	if (done) return;
	done = true;
	AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* ep) -> LONG {
		DWORD c = ep->ExceptionRecord->ExceptionCode;
		if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_INT_DIVIDE_BY_ZERO
			|| c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || c == 0xC0000409 /* stack buffer overrun */)
			CrashFilter(ep);
		return EXCEPTION_CONTINUE_SEARCH;
	});
}

} // namespace kt
