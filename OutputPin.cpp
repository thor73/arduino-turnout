
#include "OutputPin.h"

OutputPin::OutputPin(byte Pin)
{
	pin = Pin;
    state = LOW;
	pinMode(pin, OUTPUT);
}

void OutputPin::SetPin(bool State)
{
	state = State;
	digitalWrite(pin, state);
}

bool OutputPin::GetState()
{
	return state;
}
