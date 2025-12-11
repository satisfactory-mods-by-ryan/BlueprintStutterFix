#include "BlueprintStutterFix.h"

#include "FGBlueprintSubsystem.h"
#include "FGSchematicManager.h"
#include "FGCharacterPlayer.h"

#include "Patching/NativeHookManager.h"

#define LOCTEXT_NAMESPACE "FBlueprintStutterFixModule"

void FBlueprintStutterFixModule::StartupModule() {
#if !WITH_EDITOR

	SUBSCRIBE_METHOD(AFGBlueprintSubsystem::RefreshBlueprintRecipeRequirements, [this](auto& scope, AFGBlueprintSubsystem* self) {
		if (refreshCallCount >= 2) {
			scope.Cancel();
			return;
		}
		refreshCallCount++;
	});

	SUBSCRIBE_METHOD_AFTER(AFGSchematicManager::LaunchShip, [this](AFGSchematicManager* self, AFGCharacterPlayer* accessInstigator) {
		AFGBlueprintSubsystem* blueprintSubsystem = AFGBlueprintSubsystem::Get(self->GetWorld());
		if (!blueprintSubsystem) {
			return;
		}
		refreshCallCount = 1;
		blueprintSubsystem->RefreshBlueprintRecipeRequirements();
	});

#endif
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintStutterFixModule, BlueprintStutterFix)