#pragma once

#include "..\include\Dem.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace DemInternal
{
	class ProgressReporter
	{
	public:
		explicit ProgressReporter(DemProgressCallback callback,
			DemProgressCallbackEx callbackEx = nullptr, void* userData = nullptr)
			: callback_(callback), callbackEx_(callbackEx), userData_(userData)
		{
		}

		static bool __stdcall callback(int progress, const char* message, void* userData)
		{
			return static_cast<ProgressReporter*>(userData)->report(progress, message);
		}

		bool report(int progress, const char* message)
		{
			if (cancelled_.load()) return false;
			std::lock_guard<std::mutex> lock(mutex_);
			if (cancelled_.load()) return false;
			progress = std::max(0, std::min(100, progress));
			if (progress <= lastReportedProgress_) return true;
			lastReportedProgress_ = progress;
			if (callbackEx_ && !callbackEx_(progress, message, userData_))
			{
				cancelled_ = true;
				return false;
			}
			if (!callbackEx_ && callback_ && !callback_(progress, message))
			{
				cancelled_ = true;
				return false;
			}
			return true;
		}

		bool reportCoordinateConversion(int progress)
		{
			return report(std::min(99, progress), "Converting coordinates...");
		}

		bool reportSuccess()
		{
			return report(100, "DEM generation complete.");
		}

		bool cancelled() const
		{
			return cancelled_.load();
		}

	private:
		DemProgressCallback callback_ = nullptr;
		DemProgressCallbackEx callbackEx_ = nullptr;
		void* userData_ = nullptr;
		std::atomic<bool> cancelled_{ false };
		std::mutex mutex_;
		int lastReportedProgress_ = -1;
	};
}
