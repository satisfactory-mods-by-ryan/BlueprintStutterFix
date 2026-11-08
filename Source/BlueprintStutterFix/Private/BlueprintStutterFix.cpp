#include "BlueprintStutterFix.h"

#include "BSFFunctionInterceptor.h"
#include "FGBlueprintSubsystem.h"
#include "FGSchematicManager.h"

#include "Patching/NativeHookManager.h"

#define LOCTEXT_NAMESPACE "FBlueprintStutterFixModule"

void FBlueprintStutterFixModule::StartupModule() {
#if !WITH_EDITOR

	SUBSCRIBE_METHOD(AFGBlueprintSubsystem::RefreshBlueprintRecipeRequirements, [this](auto& scope, AFGBlueprintSubsystem* self) {
		if (refreshCallCount++ >= 2) {
			scope.Cancel();
			return;
		}
	});

	SUBSCRIBE_METHOD_AFTER(AFGSchematicManager::UnlockAllSchematicsOfType, [this](AFGSchematicManager* self, ESchematicType schematicType, bool requireDependency) {
		this->RefreshBlueprintRecipeRequirements(self->GetWorld());
	});

	SUBSCRIBE_METHOD_AFTER(AFGSchematicManager::UnlockSchematicsUpToTier, [this](AFGSchematicManager* self, int32 tier) {
		this->RefreshBlueprintRecipeRequirements(self->GetWorld());
	});

	FBSFFunctionInterceptor::Get().Startup();

#endif
}

void FBlueprintStutterFixModule::ShutdownModule() {
	FBSFFunctionInterceptor::Get().Shutdown();
}

void FBlueprintStutterFixModule::RefreshBlueprintRecipeRequirements(UWorld* world) {
	AFGBlueprintSubsystem* blueprintSubsystem = AFGBlueprintSubsystem::Get(world);
	if (!blueprintSubsystem) {
		return;
	}
	this->refreshCallCount = 1;
	blueprintSubsystem->RefreshBlueprintRecipeRequirements();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintStutterFixModule, BlueprintStutterFix)