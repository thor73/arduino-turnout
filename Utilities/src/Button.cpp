/*

This file is part of Arduino Turnout
Copyright (C) 2017-2018 Eric Thorstenson

Arduino Turnout is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Arduino Turnout is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.

*/

#include "Button.h"

// Create a button.
Button::Button(byte Pin, bool EnablePullup)
{
	// set up pin modes
	pin = Pin;
	if (EnablePullup)
	{
		pinMode(pin, INPUT_PULLUP);
	}
	else
	{
		pinMode(pin, INPUT);
	}
}


// Check for change in pin state, and update switch state after debounce interval.
void Button::Update(unsigned long CurrentMillis)
{
	// get the current state of the pin
	byte currentRawState = digitalRead(pin);

	// this registers a change of state in the input pin
	if (currentRawState != lastRawState)
	{
		readEnable = true;
		interruptTime = CurrentMillis;
		numInterrupts++;       // for testing

		lastRawState = currentRawState;
	}

	// wait a bit to debounce before updating our switch state
	if (readEnable && (CurrentMillis - interruptTime > debounceTime))
	{
		readEnable = false;
		switchState = currentRawState;
		hasChanged = true;
		numUpdates++;       // for testing

		// raise event
		if (buttonPressHandler) buttonPressHandler(switchState);
	}
}

// In case we want to call Update without supplying millis.
void Button::Update()
{
	Update(millis());
}

// Get the debounced switch state.
byte Button::SwitchState() 
{ 
	hasChanged = false;    // reset changed flag after reading
	return switchState;
}

// Read the state of the switch directly, without debounce
byte Button::RawState()
{
	return digitalRead(pin);
}

// Get the number of times the raw switch state has changed.
int Button::NumInterrupts() { return numInterrupts; }

// Get the number of times the debounced switch state has changed.
int Button::NumUpdates() { return numUpdates; }

// Check if the switch status has changed since the last read
bool Button::HasChanged() { return hasChanged; }

// Assign the callback function for the button press event
void Button::SetButtonPressHandler(ButtonPressHandlerFunc Handler) { buttonPressHandler = Handler; }
