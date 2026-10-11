#include "NCAimTrainerInputSettings.h"

#include "GameFramework/InputSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

namespace NCAimTrainerInputSettings
{
	bool IsAllowedRestartKey(FKey Key)
	{
		// Invalid is the explicit unbound choice. Do not let a restart bind remove
		// access to the menus/movement toggle, or consume the firing buttons.
		// The viewport handles configured console keys before controller input.
		return Key == EKeys::Invalid || (Key.IsValid()
			&& !Key.IsFloatAxis() && !Key.IsVectorAxis() && !Key.IsModifierKey()
			&& Key != EKeys::Escape && Key != EKeys::F5 && Key != EKeys::F6
			&& Key != EKeys::M && Key != EKeys::Tilde
			&& Key != EKeys::LeftMouseButton && Key != EKeys::RightMouseButton
			&& !GetDefault<UInputSettings>()->ConsoleKeys.Contains(Key));
	}

	FKey GetRestartKey()
	{
		const FKey DefaultKey = IsAllowedRestartKey(EKeys::R) ? EKeys::R : EKeys::Invalid;
		FString KeyName;
		// GConfig is an in-memory cache: this does not read the disk per key event
		// or HUD draw, and Save changes take effect immediately in the same run.
		if (!GConfig || !GConfig->GetString(TEXT("NCAimTrainer"), TEXT("RestartKey"), KeyName,
			FPaths::GeneratedConfigDir() + TEXT("Mod.ini")))
		{
			return DefaultKey;
		}
		KeyName.Trim();
		KeyName.TrimTrailing();
		if (KeyName.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { return EKeys::Invalid; }
		const FKey Key{FName(*KeyName)};
		return Key.IsValid() && IsAllowedRestartKey(Key) ? Key : DefaultKey;
	}

	bool SetRestartKey(FKey Key)
	{
		if (!GConfig || !IsAllowedRestartKey(Key)) { return false; }
		const FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
		const FString KeyName = Key == EKeys::Invalid ? TEXT("None") : Key.GetFName().ToString();
		GConfig->SetString(TEXT("NCAimTrainer"), TEXT("RestartKey"), *KeyName, ConfigPath);
		GConfig->Flush(false, ConfigPath);
		return true;
	}
}
