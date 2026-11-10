#pragma once

class CNEOBot;

// Research: before its team's first contact in a CTG round, a bot that has run ahead of every living
// teammate walks or holds until the nearest one catches up, so the opening fight is not a lone bot's
enum NeoBotPaceMode
{
	NEO_BOT_PACE_NONE,
	NEO_BOT_PACE_WALK,
	NEO_BOT_PACE_HOLD,
};

// Once a tick from the main action: decides the pace and applies it to the bot's buttons and speed
NeoBotPaceMode NeoBotUpdatePacing( CNEOBot *me );

// True while the bot holds still for a teammate; locomotion then presses no move buttons
bool NeoBotIsPacingHold( const CNEOBot *me );
