#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

enum InSARDiagnosticSeverity
{
	INSAR_DIAGNOSTIC_DEBUG = 0,
	INSAR_DIAGNOSTIC_INFO = 1,
	INSAR_DIAGNOSTIC_WARNING = 2,
	INSAR_DIAGNOSTIC_ERROR = 3,
	INSAR_DIAGNOSTIC_TRACE = 4
};

struct InSARDiagnosticEvent
{
	int version;
	InSARDiagnosticSeverity severity;
	const char* category;
	const char* phase;
	const char* message;
	const char* detail;
	const char* h5File;
	const char* dataset;
	int imageIndex;
	int burstIndex;
	int statusCode;
	int rows;
	int columns;
	int cvType;
	long long elapsedMs;
};

typedef void (__stdcall *InSARDiagnosticCallback)(const InSARDiagnosticEvent* event, void* userData);
