#pragma once
// vim: set ts=4 sw=4 tw=99 noet:
//
// AMX Mod X, based on AMX Mod by Aleksander Naszko ("OLO").
// Copyright (C) The AMX Mod X Development Team.
//
// This software is licensed under the GNU General Public License, version 3 or higher.
// Additional exceptions apply. For full license details, see LICENSE.txt or visit:
//     https://alliedmods.net/amxmodx-license

#ifndef _INCLUDE_DEBUGGER_H_
#define _INCLUDE_DEBUGGER_H_
#include <sp_vm_types.h>
#include <sp_vm_api.h>
#include "sp_vm_debug_api.h"
#include "plugin-context.h"
#include <smx/smx-headers.h>
#include <smx/smx-v1.h>
#include "smx-v1-image.h"
#include "stack-frames.h"
#include "extension.h"
#endif //_INCLUDE_DEBUGGER_H_


class DebugReport : public IDebugListener {
public:
	/**
	 * @brief Called on debug spew.
	 *
	 * @param msg    Message text.
	 * @param fmt    Message formatting arguments (printf-style).
	 */
	void OnDebugSpew(const char *msg, ...);

	/**
	 * @brief Called when an error is reported and no exception
	 * handler was available.
	 *
	 * @param report  Error report object.
	 * @param iter      Stack frame iterator.
	 */
	void ReportError(const IErrorReport &report, IFrameIterator &iter);
	
	IDebugListener *original;
};