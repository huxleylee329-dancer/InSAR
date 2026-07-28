#pragma once
#ifndef __DEM__H__
#define __DEM__H__
#include <cstddef>
#include <cstdint>
#include"..\include\Package.h"
#include"..\include\Deflat.h"



// 定义 Dem 专用的进度回调函数指针类型
typedef bool (__stdcall *DemProgressCallback)(int progress, const char* message);
typedef bool (__stdcall *DemProgressCallbackEx)(int progress, const char* message, void* userData);

enum DemLogLevel : int32_t
{
	DEM_LOG_DEBUG = 0,
	DEM_LOG_INFO = 1,
	DEM_LOG_WARNING = 2,
	DEM_LOG_ERROR = 3
};

enum DemError : int32_t
{
	DEM_ERROR_INVALID_DIAGNOSTIC_OPTIONS = -2001,
	DEM_ERROR_INVALID_INPUT = -2002,
	DEM_ERROR_HDF5_READ = -2003,
	DEM_ERROR_SOURCE_PATH = -2004,
	DEM_ERROR_INVALID_SHAPE = -2005,
	DEM_ERROR_PROCESSING = -2006,
	DEM_ERROR_CANCELLED = -2007
};

struct DemDiagnosticEvent
{
	DemLogLevel level;
	DemError error;
	const char* callId;
	const char* stage;
	const char* message;
	const char* detail;
	const char* h5File;
	const char* dataset;
	int32_t hdf5Status;
	int32_t rows;
	int32_t columns;
	int32_t cvType;
};

// The callback is synchronous. All string fields are borrowed for the callback
// duration only; callers must copy them before returning.
typedef void (__stdcall *DemDiagnosticCallback)(const DemDiagnosticEvent* event, void* userData);

struct DemDiagnosticOptions
{
	uint32_t structSize;
	uint32_t version;
	const char* callId;
	int32_t logLevel;
	DemDiagnosticCallback callback;
	void* userData;
	DemProgressCallbackEx progressCallback;
	void* progressUserData;
};

constexpr uint32_t DEM_DIAGNOSTIC_OPTIONS_VERSION_V1 = 1;
constexpr uint32_t DEM_DIAGNOSTIC_OPTIONS_VERSION = 2;
constexpr uint32_t DEM_DIAGNOSTIC_OPTIONS_MIN_SIZE =
	static_cast<uint32_t>(offsetof(DemDiagnosticOptions, version) + sizeof(uint32_t));

class InSAR_API Dem
{
public:
	Dem();
	~Dem();
	/** @brief 相位高程转换
	
	@param unwrapped_phase                                 解缠相位
	@param flat_phase                                      平地相位
	@param dem                                             高程值（返回值）
	@param auxi_m                                          主星辅助参数
	@param auxi_s                                          辅星辅助参数
	@param orbit_m                                         主星轨道参数
	@param orbit_s                                         辅星轨道参数
	@param doppler_frequency                               多普勒中心频率
	@param gcps                                            地面控制点
	@param regis_out                                       配准结果（包括配准偏移量和配准前图像尺寸）
	@param delta_m                                         主星成像间隔时间
	@param delta_s                                         辅星成像间隔时间
	@param multilook_times                                 干涉多视倍数
	@param mode                                            收发模式（1：单发单收，2：单发双收）
	@param iters                                           牛顿迭代次数（默认30次）
	@return  成功返回0，否则返回-1
	*/
	int phase2dem_newton_iter(
		Mat unwrapped_phase,
		Mat flat_phase,
		Mat& dem,
		Mat auxi_m,
		Mat auxi_s,
		Mat orbit_m,
		Mat orbit_s,
		Mat doppler_frequency,
		Mat gcps,
		Mat regis_out,
		double delta_m,
		double delta_s,
		int multilook_times,
		int mode, 
		int iters,
		DemProgressCallback cb = nullptr
	);
	/** @brief 牛顿迭代法反演高程
	
	@param unwrapped_phase_file                            解缠相位h5文件
	@param dem                                             高程反演结果（返回值）
	@param project_path                                    工程路径
	@param iter_times                                      迭代次数
	@param mode                                            收发方式（1自发自收（默认），2单发双收）
	@return  成功返回0，否则返回-1
	*/
	int dem_newton_iter(
		const char* unwrapped_phase_file,
		Mat& dem,
		const char* project_path,
		int iter_times,
		int mode = TR_MODE_SINGLE_TX_SINGLE_RX,
		DemProgressCallback cb = nullptr
	);
	// Extended diagnostic entry point. v1 diagnostics use cb for progress. v2
	// diagnostics may provide progressCallback/progressUserData instead; when
	// present, the v2 callback is used exclusively for progress notifications.
	int dem_newton_iter_ex(
		const char* unwrapped_phase_file,
		Mat& dem,
		const char* project_path,
		int iter_times,
		int mode,
		const DemDiagnosticOptions* diagnostics,
		DemProgressCallback cb = nullptr
	);

	/** @brief 牛顿迭代法反演高程（测试版）
	@param unwrapped_phase_file                            解缠相位h5文件
	@param dem                                             高程反演结果（返回值）
	@param project_path                                    工程路径
	@param iter_times                                      迭代次数
	@param mode                                            收发方式（1自发自收（默认），2单发双收）
	@return  成功返回0，否则返回-1
	*/
	int dem_newton_iter_test(
		const char* unwrapped_phase_file,
		Mat& dem,
		const char* project_path,
		int iter_times,
		int mode = TR_MODE_SINGLE_TX_SINGLE_RX,
		DemProgressCallback cb = nullptr
	);

	/** @brief 牛顿迭代法反演高程（测试版）
	@param unwrapped_phase_file                            解缠相位h5文件
	@param dem                                             高程反演结果（高程返回值）
	@param lon                                             高程反演结果（经度返回值）
	@param lat                                             高程反演结果（纬度返回值）
	@param dem_x                                           高程反演结果（X坐标返回值）
	@param dem_y                                           高程反演结果（Y坐标返回值）
	@param dem_z                                           高程反演结果（Z坐标返回值）
	@param error_llh                                       控制点经纬高误差（返回值, n_gcps×3）
	@param error_xyz                                       控制点XYZ误差（返回值，n_gcps×3）
	@param project_path                                    工程路径
	@param iter_times                                      迭代次数
	@param mode                                            收发方式（1自发自收（默认），2单发双收）
	@return  成功返回0，否则返回-1
	*/
	int dem_newton_iter_14(
		const char* unwrapped_phase_file,
		Mat& dem,
		Mat& lon,
		Mat& lat,
		Mat& dem_x,
		Mat& dem_y,
		Mat& dem_z,
		Mat& error_llh,
		Mat& error_xyz,
		const char* project_path,
		int iter_times,
		int mode = TR_MODE_SINGLE_TX_SINGLE_RX,
		DemProgressCallback cb = nullptr
	);

	/** @brief 牛顿迭代法反演高程（双频乒乓模式）
	@param unwrapped_phase_file                            解缠相位h5文件
	@param dem                                             高程反演结果（高程返回值）
	@param lon                                             高程反演结果（经度返回值）
	@param lat                                             高程反演结果（纬度返回值）
	@param dem_x                                           高程反演结果（X坐标返回值）
	@param dem_y                                           高程反演结果（Y坐标返回值）
	@param dem_z                                           高程反演结果（Z坐标返回值）
	@param error_llh                                       控制点经纬高误差（返回值, n_gcps×3）
	@param error_xyz                                       控制点XYZ误差（返回值，n_gcps×3）
	@param project_path                                    工程路径
	@param iter_times                                      迭代次数
	@param mode                                            收发方式（1自发自收（默认），2单发双收）
	@return  成功返回0，否则返回-1
	*/
	int dem_newton_iter_14_dualfreqpingpong(
		const char* unwrapped_phase_file,
		Mat& dem,
		Mat& lon,
		Mat& lat,
		Mat& dem_x,
		Mat& dem_y,
		Mat& dem_z,
		Mat& error_llh,
		Mat& error_xyz,
		const char* project_path,
		int iter_times,
		int mode = TR_MODE_SINGLE_TX_SINGLE_RX,
		DemProgressCallback cb = nullptr
	);

private:
	int dem_newton_iter_impl(
		const char* unwrapped_phase_file,
		Mat& dem,
		const char* project_path,
		int iter_times,
		int mode,
		const DemDiagnosticOptions* diagnostics,
		DemProgressCallback cb,
		bool legacyConsoleLogging
	);
	char error_head[256];
	char parallel_error_head[256];

};


#endif // !__DEM__H__
