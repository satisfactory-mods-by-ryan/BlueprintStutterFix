#include "BSFFunctionInterceptor.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWidget.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintStutterFix, Warning, All);

namespace {
	// The widget classes are loaded after this module starts, so installing is retried
	constexpr double installIntervalSeconds = 3.0;

	const TCHAR* const blueprintMenuClassPath = TEXT("/Game/FactoryGame/Interface/UI/InGame/BuildMenu/Blueprints/BPW_BlueprintMenu.BPW_BlueprintMenu_C");
	const TCHAR* const buildMenuClassPath = TEXT("/Game/FactoryGame/Interface/UI/InGame/BuildMenu/Prototype/Widget_BuildMenu.Widget_BuildMenu_C");
	const TCHAR* const directorySelectorPathFragment = TEXT("BPW_BlueprintDirectorySelector");

	// Names of the generated component bound events for the search box
	const TCHAR* const searchChangedSuffix = TEXT("_OnSearchTextChanged__DelegateSignature");
	const TCHAR* const searchCommittedSuffix = TEXT("_OnSearchComittedEnter__DelegateSignature");

	TAutoConsoleVariable<float> CVarSearchDebounceMs(
		TEXT("BSF.SearchDebounceMs"), 300.0f,
		TEXT("Build menu search: typing pause in milliseconds before a slow search runs. 0 disables."),
		ECVF_Default
	);

	// Small libraries search fast, and delaying those searches would only add input lag
	TAutoConsoleVariable<float> CVarSearchDebounceMinCostMs(
		TEXT("BSF.SearchDebounceMinCostMs"), 50.0f,
		TEXT("Build menu search: only delay a search if the last one on that widget took at least this many milliseconds."),
		ECVF_Default
	);

	TAutoConsoleVariable<int32> CVarLazyDirectoryMenu(
		TEXT("BSF.LazyDirectoryMenu"), 1,
		TEXT("Cog edit popup: 1 = build the directory selector's blueprint list only when it is used or shown. 0 disables."),
		ECVF_Default
	);

	// What every Blueprint function pointed to before wrapping, used for calls we can't identify
	FNativeFuncPtr defaultOriginal = nullptr;

	// A lazy menu has to be built when it appears, even if nothing calls into it
	bool IsShown(UWidget* widget) {
		for (int32 depth = 0; widget && depth < 64; depth++) {
			if (!widget->IsVisible()) {
				return false;
			}
			UWidget* parent = widget->GetParent();
			if (!parent) {
				// Top of this widget tree, continue with the user widget that owns it
				const UWidgetTree* tree = Cast<UWidgetTree>(widget->GetOuter());
				parent = tree ? Cast<UWidget>(tree->GetOuter()) : nullptr;
				if (!parent) {
					const TSharedPtr<SWidget> slateWidget = widget->GetCachedWidget();
					return slateWidget.IsValid() && slateWidget->GetParentWidget().IsValid();
				}
			}
			widget = parent;
		}
		return false;
	}
}

FBSFFunctionInterceptor& FBSFFunctionInterceptor::Get() {
	static FBSFFunctionInterceptor instance;
	return instance;
}

void FBSFFunctionInterceptor::Startup() {
	tickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FBSFFunctionInterceptor::Tick), 0.0f);
}

void FBSFFunctionInterceptor::Shutdown() {
	if (tickerHandle.IsValid()) {
		FTSTicker::GetCoreTicker().RemoveTicker(tickerHandle);
		tickerHandle.Reset();
	}

	for (FPendingSearch& pending : pendingSearches) {
		FreeCall(pending.call);
	}
	pendingSearches.Reset();
	for (FLazyMenu& menu : lazyMenus) {
		FreeCalls(menu.calls);
	}
	lazyMenus.Reset();

	for (const TPair<UFunction*, FInterceptedFunction>& entry : interceptedFunctions) {
		UFunction* function = entry.Value.function.Get();
		if (function && function->GetNativeFunc() == &FBSFFunctionInterceptor::Wrapper) {
			function->SetNativeFunc(entry.Value.original);
		}
	}
}

void FBSFFunctionInterceptor::InstallWrappers() {
	for (const TCHAR* classPath : { blueprintMenuClassPath, buildMenuClassPath }) {
		UClass* widgetClass = FindObject<UClass>(nullptr, classPath);
		if (!widgetClass) {
			continue;
		}
		const bool isBlueprintMenu = FCString::Strcmp(classPath, blueprintMenuClassPath) == 0;

		int32 wrappedCount = 0;
		for (TFieldIterator<UFunction> it(widgetClass, EFieldIterationFlags::None); it; ++it) {
			UFunction* function = *it;
			if (function->HasAnyFunctionFlags(FUNC_Native) || function->Script.IsEmpty() || function->GetNativeFunc() == &FBSFFunctionInterceptor::Wrapper) {
				continue;
			}

			const FString functionName = function->GetName();
			FInterceptedFunction intercepted;
			intercepted.isBlueprintMenuFunction = isBlueprintMenu;
			intercepted.isSearchChanged = functionName.EndsWith(searchChangedSuffix);
			intercepted.isSearchCommitted = functionName.EndsWith(searchCommittedSuffix);
			intercepted.isConstruct = isBlueprintMenu && functionName == TEXT("Construct");
			intercepted.isInputDeviceChanged = isBlueprintMenu && functionName == TEXT("OnInputDeviceTypeChanged");
			if (!intercepted.isBlueprintMenuFunction && !intercepted.isSearchChanged && !intercepted.isSearchCommitted) {
				continue;
			}

			intercepted.function = function;
			intercepted.original = function->GetNativeFunc();
			if (!defaultOriginal) {
				defaultOriginal = intercepted.original;
			}
			interceptedFunctions.Add(function, intercepted);
			function->SetNativeFunc(&FBSFFunctionInterceptor::Wrapper);
			wrappedCount++;
		}

		if (wrappedCount > 0) {
			UE_LOG(LogBlueprintStutterFix, Log, TEXT("Intercepting %d Blueprint functions of %s"), wrappedCount, *widgetClass->GetName());
		}
	}
}

void FBSFFunctionInterceptor::Wrapper(UObject* context, FFrame& stack, void* const result) {
	FBSFFunctionInterceptor& self = Get();
	UFunction* function = stack.CurrentNativeFunction;

	const FInterceptedFunction* found = (function && IsInGameThread()) ? self.interceptedFunctions.Find(function) : nullptr;
	if (!found) {
		(*defaultOriginal)(context, stack, result);
		return;
	}
	const FInterceptedFunction intercepted = *found;

	if (!self.replaying && context) {
		if ((intercepted.isConstruct || intercepted.isInputDeviceChanged) && CVarLazyDirectoryMenu.GetValueOnGameThread() != 0
			&& self.TryDeferLazyCall(context, function, stack, intercepted.isConstruct)) {
			return;
		}

		// Params are only copied for plain events, which is all the search box sends
		if (intercepted.isSearchChanged && stack.Locals && !function->HasAnyFunctionFlags(FUNC_HasOutParms) && function->ReturnValueOffset == MAX_uint16
			&& CVarSearchDebounceMs.GetValueOnGameThread() > 0.0f && self.ShouldDeferSearch(context, function)) {
			self.DeferSearch(context, function, stack);
			return;
		}

		// Anything else called on a lazy menu means it is in use
		if (intercepted.isBlueprintMenuFunction && !self.lazyMenus.IsEmpty()) {
			self.FlushLazyMenu(context);
		}

		// Enter should act on the final text, not on the results from before the pause
		if (intercepted.isSearchCommitted && !self.pendingSearches.IsEmpty()) {
			self.RunPendingSearches(context);
		}
	}

	if (!intercepted.isSearchChanged) {
		(*intercepted.original)(context, stack, result);
		return;
	}

	const double startSeconds = FPlatformTime::Seconds();
	(*intercepted.original)(context, stack, result);
	if (context) {
		self.RecordSearchCost(context, function, (FPlatformTime::Seconds() - startSeconds) * 1000.0);
	}
}

bool FBSFFunctionInterceptor::Tick(float) {
	const double nowSeconds = FPlatformTime::Seconds();
	if (nowSeconds - lastInstallSeconds >= installIntervalSeconds) {
		lastInstallSeconds = nowSeconds;
		InstallWrappers();
	}

	if (!lazyMenus.IsEmpty()) {
		FlushShownLazyMenus();
	}
	if (!pendingSearches.IsEmpty()) {
		RunPendingSearches(nullptr);
	}
	return true;
}

// ProcessEvent puts a copy of a call's parameters at the start of the frame's locals
uint8* FBSFFunctionInterceptor::CopyParams(UFunction* function, const uint8* locals) {
	const int32 paramsSize = FMath::Max<int32>(function->ParmsSize, 1);
	uint8* params = static_cast<uint8*>(FMemory::Malloc(paramsSize, function->GetMinAlignment()));
	FMemory::Memzero(params, paramsSize);
	for (TFieldIterator<FProperty> it(function); it; ++it) {
		if (it->HasAnyPropertyFlags(CPF_Parm)) {
			it->InitializeValue_InContainer(params);
			if (locals) {
				it->CopyCompleteValue_InContainer(params, locals);
			}
		}
	}
	return params;
}

void FBSFFunctionInterceptor::FreeCall(FDeferredCall& call) {
	if (!call.params) {
		return;
	}
	if (UFunction* function = call.function.Get()) {
		for (TFieldIterator<FProperty> it(function); it; ++it) {
			if (it->HasAnyPropertyFlags(CPF_Parm)) {
				it->DestroyValue_InContainer(call.params);
			}
		}
	}
	FMemory::Free(call.params);
	call.params = nullptr;
}

void FBSFFunctionInterceptor::FreeCalls(TArray<FDeferredCall>& calls) {
	for (FDeferredCall& call : calls) {
		FreeCall(call);
	}
}

FBSFFunctionInterceptor::FPendingSearch* FBSFFunctionInterceptor::FindPendingSearch(UObject* context, UFunction* function) {
	return pendingSearches.FindByPredicate([&](const FPendingSearch& pending) {
		return pending.context.Get() == context && pending.call.function.Get() == function;
	});
}

FBSFFunctionInterceptor::FSearchCost* FBSFFunctionInterceptor::FindSearchCost(UObject* context, UFunction* function) {
	return searchCosts.FindByPredicate([&](const FSearchCost& cost) {
		return cost.context.Get() == context && cost.function.Get() == function;
	});
}

bool FBSFFunctionInterceptor::ShouldDeferSearch(UObject* context, UFunction* function) {
	if (FindPendingSearch(context, function)) {
		return true;
	}
	// A widget we haven't timed yet counts as slow, otherwise the first (often broadest) search would never be delayed
	const FSearchCost* cost = FindSearchCost(context, function);
	return !cost || cost->costMs >= CVarSearchDebounceMinCostMs.GetValueOnGameThread();
}

void FBSFFunctionInterceptor::DeferSearch(UObject* context, UFunction* function, FFrame& stack) {
	// Only the latest text matters, so a new keystroke replaces the one waiting
	FPendingSearch* pending = FindPendingSearch(context, function);
	if (pending) {
		FreeCall(pending->call);
	} else {
		pending = &pendingSearches.AddDefaulted_GetRef();
		pending->context = context;
	}
	pending->call.function = function;
	pending->call.params = CopyParams(function, stack.Locals);
	pending->dueSeconds = FPlatformTime::Seconds() + CVarSearchDebounceMs.GetValueOnGameThread() / 1000.0;
}

void FBSFFunctionInterceptor::RecordSearchCost(UObject* context, UFunction* function, double costMs) {
	if (FSearchCost* cost = FindSearchCost(context, function)) {
		cost->costMs = costMs;
		return;
	}
	searchCosts.RemoveAll([](const FSearchCost& cost) {
		return !cost.context.IsValid();
	});
	FSearchCost& added = searchCosts.AddDefaulted_GetRef();
	added.context = context;
	added.function = function;
	added.costMs = costMs;
}

// Runs the searches of one widget, or with a null context every search whose pause is over
void FBSFFunctionInterceptor::RunPendingSearches(UObject* context) {
	const double nowSeconds = FPlatformTime::Seconds();

	// Take them out of the list first, running a search can defer or flush others
	TArray<FPendingSearch> searchesToRun;
	for (int32 i = pendingSearches.Num() - 1; i >= 0; i--) {
		const FPendingSearch& pending = pendingSearches[i];
		const bool isGone = !pending.context.IsValid() || !pending.call.function.IsValid();
		const bool isSelected = context ? pending.context.Get() == context : pending.dueSeconds <= nowSeconds;
		if (isGone || isSelected) {
			searchesToRun.Add(pending);
			pendingSearches.RemoveAt(i);
		}
	}

	for (FPendingSearch& pending : searchesToRun) {
		UObject* widget = pending.context.Get();
		UFunction* function = pending.call.function.Get();
		if (widget && function) {
			// The search reads the list the lazy menu would have built
			FlushLazyMenu(widget);

			const bool wasReplaying = replaying;
			replaying = true;
			widget->ProcessEvent(function, pending.call.params);
			replaying = wasReplaying;
		}
		FreeCall(pending.call);
	}
}

bool FBSFFunctionInterceptor::TryDeferLazyCall(UObject* context, UFunction* function, FFrame& stack, bool isConstruct) {
	if (function->HasAnyFunctionFlags(FUNC_HasOutParms) || function->ReturnValueOffset != MAX_uint16) {
		return false;
	}

	FLazyMenu* menu = lazyMenus.FindByPredicate([&](const FLazyMenu& lazyMenu) {
		return lazyMenu.context.Get() == context;
	});
	if (!menu) {
		// Only a Construct starts deferring, and only in the directory selector. The other menus are shown right away.
		if (!isConstruct || !context->GetPathName().Contains(directorySelectorPathFragment)) {
			return false;
		}
		menu = &lazyMenus.AddDefaulted_GetRef();
		menu->context = context;
	}

	FDeferredCall& call = menu->calls.AddDefaulted_GetRef();
	call.function = function;
	call.params = CopyParams(function, stack.Locals);
	return true;
}

void FBSFFunctionInterceptor::FlushLazyMenu(UObject* context) {
	const int32 index = lazyMenus.IndexOfByPredicate([&](const FLazyMenu& lazyMenu) {
		return lazyMenu.context.Get() == context;
	});
	if (index == INDEX_NONE) {
		return;
	}

	// Take it out of the list first, its calls can call other functions on the same menu
	FLazyMenu menu = MoveTemp(lazyMenus[index]);
	lazyMenus.RemoveAt(index);

	const bool wasReplaying = replaying;
	replaying = true;
	for (FDeferredCall& call : menu.calls) {
		UFunction* function = call.function.Get();
		if (function && context) {
			context->ProcessEvent(function, call.params);
		}
	}
	replaying = wasReplaying;
	FreeCalls(menu.calls);
}

void FBSFFunctionInterceptor::FlushShownLazyMenus() {
	TArray<UObject*> shownMenus;
	for (int32 i = lazyMenus.Num() - 1; i >= 0; i--) {
		UObject* menu = lazyMenus[i].context.Get();
		if (!menu) {
			FreeCalls(lazyMenus[i].calls);
			lazyMenus.RemoveAt(i);
		} else if (IsShown(Cast<UWidget>(menu))) {
			shownMenus.Add(menu);
		}
	}

	for (UObject* menu : shownMenus) {
		FlushLazyMenu(menu);
	}
}
