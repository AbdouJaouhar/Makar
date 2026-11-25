// Recordings of the ship viewer as the player sees it (for checking the animation, not only stills).
//
//   Makar.Ship <Type> <Seed> <WindFrom> <WindSpeed>   the ship to show (Sloop, Brigantine, Frigate, Galleon)
//   Makar.Wind <From> [Speed]                          the wind (the rig is trimmed to it as in the viewer)
//   Makar.View <Yaw> <Pitch> <Metres> [X Y Z]          a fixed camera round it (centre offset in metres)
//   Makar.At <Seconds> <command...>                    a command run that long into the recording (wind shifts...)
//   Makar.Record <Seconds> <Fps> <Name> [quit]         once the ship is built: every frame at a fixed time step, saved
//                                                      to Saved/Recordings/<Name>/frame_00000.bmp; quits after if asked
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Ship/ProceduralShip.h"
#include "Ship/ShipDesign.h"
#include "Ship/ShipViewer.h"
#include "UnrealClient.h"
#include "Engine/Engine.h"

namespace ShipRecorder
{
	struct FTimed
	{
		float At = 0.f;
		FString Command;
	};

	struct FState
	{
		bool bArmed = false;
		bool bRecording = false;
		bool bQuit = false;
		float Seconds = 6.f;
		float Fps = 30.f;
		FString Name;
		FString Dir;
		int32 Frame = 0;
		int32 Frames = 0;
		double ReadyAt = 0.0;
		TArray<FTimed> Timeline;
		FTSTicker::FDelegateHandle Ticker;
		TWeakObjectPtr<UWorld> World;
		FDelegateHandle Captured;
		int32 Pending = -1;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	AProceduralShip* FindShip(UWorld* World)
	{
		for (TActorIterator<AProceduralShip> It(World); It; ++It)
		{
			return *It;
		}
		return nullptr;
	}

	AShipViewerPawn* FindPawn(UWorld* World)
	{
		for (TActorIterator<AShipViewerPawn> It(World); It; ++It)
		{
			return *It;
		}
		return nullptr;
	}

	void OnCaptured(int32 Width, int32 Height, const TArray<FColor>& Colors)
	{
		FState& S = State();
		if (S.Pending < 0 || Colors.Num() != Width * Height)
		{
			return;
		}
		TArray<FColor> Pixels = Colors;
		for (FColor& C : Pixels)
		{
			C.A = 255;
		}
		FFileHelper::CreateBitmap(*FString::Printf(TEXT("%s/frame_%05d.bmp"), *S.Dir, S.Pending), Width, Height, Pixels.GetData());
		S.Pending = -1;
	}

	void Finish()
	{
		FState& S = State();
		FApp::SetUseFixedTimeStep(false);
		UE_LOG(LogTemp, Display, TEXT("MAKARREC done %s %d frames"), *S.Name, S.Frame);
		S.bRecording = false;
		S.bArmed = false;
		if (S.bQuit && S.World.IsValid())
		{
			UKismetSystemLibrary::QuitGame(S.World.Get(), nullptr, EQuitPreference::Quit, false);
		}
	}

	bool Tick(float)
	{
		FState& S = State();
		UWorld* World = S.World.Get();
		if (!S.bArmed || !World)
		{
			return true;
		}
		if (!S.bRecording)
		{
			// Ready once the ship is built and has settled for a moment.
			AProceduralShip* Ship = FindShip(World);
			if (!Ship || Ship->IsBuildPending())
			{
				S.ReadyAt = FPlatformTime::Seconds() + 1.5;
				return true;
			}
			if (FPlatformTime::Seconds() < S.ReadyAt)
			{
				return true;
			}
			S.bRecording = true;
			S.Frame = 0;
			S.Frames = FMath::Max(1, FMath::RoundToInt(S.Seconds * S.Fps));
			FApp::SetUseFixedTimeStep(true);
			FApp::SetFixedDeltaTime(1.0 / S.Fps);
			UE_LOG(LogTemp, Display, TEXT("MAKARREC start %s: %d frames at %.0f fps"), *S.Name, S.Frames, S.Fps);
			return true;
		}
		if (S.Frame >= S.Frames)
		{
			if (S.Pending < 0)
			{
				Finish();
			}
			return true;
		}
		// This frame captured once drawn (the engine's screenshot path: it works offscreen); saved by OnCaptured.
		if (!GEngine || !GEngine->GameViewport)
		{
			UE_LOG(LogTemp, Warning, TEXT("MAKARREC no viewport"));
			Finish();
			return true;
		}
		if (!S.Captured.IsValid())
		{
			S.Captured = GEngine->GameViewport->OnScreenshotCaptured().AddStatic(&OnCaptured);
		}
		S.Pending = S.Frame;
		FScreenshotRequest::RequestScreenshot(false);
		// Timed commands, in the recording's own time.
		const float Now = S.Frame / S.Fps;
		for (int32 I = 0; I < S.Timeline.Num(); ++I)
		{
			if (S.Timeline[I].At <= Now)
			{
				const FString Command = S.Timeline[I].Command;
				S.Timeline.RemoveAt(I--);
				UE_LOG(LogTemp, Display, TEXT("MAKARREC at %.2f s: %s"), Now, *Command);
				GEngine->Exec(World, *Command);
			}
		}
		++S.Frame;
		return true;
	}

	void EnsureTicker(UWorld* World)
	{
		FState& S = State();
		S.World = World;
		if (!S.Ticker.IsValid())
		{
			S.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
		}
	}

	static FAutoConsoleCommandWithWorldAndArgs ShipCommand(TEXT("Makar.Ship"), TEXT("Makar.Ship <Type> <Seed> <WindFrom> <WindSpeed>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AProceduralShip* Ship = FindShip(World);
			if (!Ship || Args.Num() < 1)
			{
				return;
			}
			const int64 Value = StaticEnum<EShipType>()->GetValueByNameString(Args[0]);
			const EShipType Type = Value == INDEX_NONE ? Ship->Type : EShipType(Value);
			const int32 Seed = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : Ship->Seed;
			if (Args.Num() > 2)
			{
				Ship->WindFrom = FCString::Atof(*Args[2]);
			}
			if (Args.Num() > 3)
			{
				Ship->WindSpeed = FCString::Atof(*Args[3]);
			}
			FShipDesign Design = FShipDesign::Make(Type, Seed);
			Design.bSailsSet = Ship->bSailsSet;
			TWeakObjectPtr<UWorld> WeakWorld(World);
			Ship->BuildFromDesignAsync(Design, [WeakWorld]()
			{
				UWorld* W = WeakWorld.Get();
				AProceduralShip* Built = W ? FindShip(W) : nullptr;
				AShipViewerPawn* Pawn = W ? FindPawn(W) : nullptr;
				if (Pawn && Built)
				{
					Pawn->Frame(Built->GetShipBounds());
				}
			});
		}));

	static FAutoConsoleCommandWithWorldAndArgs SailsCommand(TEXT("Makar.Sails"), TEXT("Makar.Sails: logs how the wind takes each sail"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (AProceduralShip* Ship = FindShip(World))
			{
				UE_LOG(LogTemp, Display, TEXT("MAKARSAILS wind from %.0f at %.1f m/s\n%s"), Ship->WindFrom, Ship->WindSpeed, *Ship->DescribeSails());
			}
		}));

	static FAutoConsoleCommandWithWorldAndArgs WindCommand(TEXT("Makar.Wind"), TEXT("Makar.Wind <From> [Speed]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AProceduralShip* Ship = FindShip(World);
			if (Ship && Args.Num() > 0)
			{
				Ship->WindFrom = FCString::Atof(*Args[0]);
				if (Args.Num() > 1)
				{
					Ship->WindSpeed = FCString::Atof(*Args[1]);
				}
			}
		}));

	static FAutoConsoleCommandWithWorldAndArgs ViewCommand(TEXT("Makar.View"), TEXT("Makar.View <Yaw> <Pitch> <Metres> [X Y Z]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AShipViewerPawn* Pawn = FindPawn(World);
			if (!Pawn || Args.Num() < 3)
			{
				return;
			}
			const FVector Offset = Args.Num() >= 6 ? FVector(FCString::Atof(*Args[3]), FCString::Atof(*Args[4]), FCString::Atof(*Args[5])) * 100.0 : FVector::ZeroVector;
			Pawn->SetView(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]) * 100.f, Offset);
		}));

	static FAutoConsoleCommandWithWorldAndArgs AtCommand(TEXT("Makar.At"), TEXT("Makar.At <Seconds> <command...>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() < 2)
			{
				return;
			}
			EnsureTicker(World);
			FTimed Timed;
			Timed.At = FCString::Atof(*Args[0]);
			for (int32 I = 1; I < Args.Num(); ++I)
			{
				Timed.Command += (I > 1 ? TEXT(" ") : TEXT("")) + Args[I];
			}
			State().Timeline.Add(Timed);
		}));

	static FAutoConsoleCommandWithWorldAndArgs RecordCommand(TEXT("Makar.Record"), TEXT("Makar.Record <Seconds> <Fps> <Name> [quit]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			EnsureTicker(World);
			FState& S = State();
			S.Seconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 6.f;
			S.Fps = Args.Num() > 1 ? FMath::Max(1.f, FCString::Atof(*Args[1])) : 30.f;
			S.Name = Args.Num() > 2 ? Args[2] : TEXT("recording");
			S.bQuit = Args.Num() > 3 && Args[3] == TEXT("quit");
			S.Dir = FPaths::ProjectSavedDir() / TEXT("Recordings") / S.Name;
			IFileManager::Get().DeleteDirectory(*S.Dir, false, true);
			IFileManager::Get().MakeDirectory(*S.Dir, true);
			S.bArmed = true;
			S.bRecording = false;
			S.ReadyAt = FPlatformTime::Seconds() + 1.5;
			UE_LOG(LogTemp, Display, TEXT("MAKARREC armed %s"), *S.Name);
		}));
}
