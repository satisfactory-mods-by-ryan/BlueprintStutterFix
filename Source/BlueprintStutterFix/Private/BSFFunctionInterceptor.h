#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/CoreNative.h"
#include "UObject/Stack.h"

// With a big blueprint library two widgets in the build menu do far too much work:
//  - the search boxes rebuild the whole result list on every keystroke (about 1 ms per result)
//  - the cog edit popup builds the full blueprint list twice, once for the selector and once for the directory selector
// Both are Blueprint script, so they are fixed by intercepting a few Blueprint functions and running them later (or
// not until needed). This is done on the UFunction instead of with SML hooks so no hook assets are needed and the
// original function can be run again afterwards.

// Interception swaps UFunction::Func, which every Blueprint call goes through. Only script functions are safe to wrap:
// native functions called through EX_CallMath skip UFunction::Invoke, so the wrapper could not tell which one it was.

class FBSFFunctionInterceptor {
public:
	static FBSFFunctionInterceptor& Get();

	void Startup();
	void Shutdown();

private:
	struct FInterceptedFunction {
		TWeakObjectPtr<UFunction> function;
		FNativeFuncPtr original = nullptr;

		// Every function of the blueprint menu counts as "using" it, which builds a lazy menu
		bool isBlueprintMenuFunction = false;
		bool isSearchChanged = false;
		bool isSearchCommitted = false;
		bool isConstruct = false;
		bool isInputDeviceChanged = false;
	};

	// The parameters of an intercepted call, so it can be run again with ProcessEvent
	struct FDeferredCall {
		TWeakObjectPtr<UFunction> function;
		uint8* params = nullptr;
	};

	struct FPendingSearch {
		TWeakObjectPtr<UObject> context;
		FDeferredCall call;
		double dueSeconds = 0.0;
	};

	// How long the last search on a widget took, to decide if the next one is worth delaying
	struct FSearchCost {
		TWeakObjectPtr<UObject> context;
		TWeakObjectPtr<UFunction> function;
		double costMs = 0.0;
	};

	// A menu whose Construct was deferred, plus the calls that came right after it
	struct FLazyMenu {
		TWeakObjectPtr<UObject> context;
		TArray<FDeferredCall> calls;
	};

	TMap<UFunction*, FInterceptedFunction> interceptedFunctions;
	TArray<FPendingSearch> pendingSearches;
	TArray<FSearchCost> searchCosts;
	TArray<FLazyMenu> lazyMenus;

	// Set while deferred calls are replayed so the wrapper lets them through
	bool replaying = false;

	FTSTicker::FDelegateHandle tickerHandle;
	double lastInstallSeconds = -1000.0;

	static void Wrapper(UObject* context, FFrame& stack, void* const result);

	bool Tick(float);
	void InstallWrappers();

	static uint8* CopyParams(UFunction* function, const uint8* locals);
	static void FreeCall(FDeferredCall& call);
	static void FreeCalls(TArray<FDeferredCall>& calls);

	FPendingSearch* FindPendingSearch(UObject* context, UFunction* function);
	FSearchCost* FindSearchCost(UObject* context, UFunction* function);
	bool ShouldDeferSearch(UObject* context, UFunction* function);
	void DeferSearch(UObject* context, UFunction* function, FFrame& stack);
	void RecordSearchCost(UObject* context, UFunction* function, double costMs);
	void RunPendingSearches(UObject* context);

	bool TryDeferLazyCall(UObject* context, UFunction* function, FFrame& stack, bool isConstruct);
	void FlushLazyMenu(UObject* context);
	void FlushShownLazyMenus();
};
