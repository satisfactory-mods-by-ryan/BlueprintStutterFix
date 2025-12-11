#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FBlueprintStutterFixModule : public IModuleInterface {
public:
    virtual void StartupModule() override;

private:
    int32 refreshCallCount = 0;
};