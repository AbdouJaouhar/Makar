#include "MakarGameMode.h"
#include "PirateShip.h"

AMakarGameMode::AMakarGameMode()
{
	DefaultPawnClass = APirateShip::StaticClass();
}
