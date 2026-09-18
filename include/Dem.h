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
	DEM_ERROR_CANCELLED = -2007,
	DEM_ERROR_ABSOLUTE_PHASE_ANCHOR_AMBIGUOUS = -2008,
	DEM_ERROR_ABSOLUTE_PHASE_ANCHOR_CONTRACT = -2009
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

// ABI-stable request for DEM absolute-phase anchoring v2.  Every path must
// name an immutable worker-prepared snapshot; this DLL never searches a
// project directory or .dem_cache for an implicit reference DEM.
// Keep this name distinct from the Qt worker's value-type request.  The UI
// owns QString/QList snapshots; this ABI only receives their immutable C
// string representation after the worker has revalidated them.
struct DemAbsolutePhaseAnchorV2CoreRequest
{
	uint32_t structSize;
	uint32_t version;
	const char* phaseH5Snapshot;
	const char* masterH5Snapshot;
	const char* slaveH5Snapshot;
	const char* auxiliaryDemRasterSnapshot;
	const char* auxiliaryDemValidMaskSnapshot;
	const char* auxiliaryDemIdentityH5Snapshot;
	const char* geoidModelSnapshot;
	const char* phaseH5SnapshotHash;
	const char* masterH5SnapshotHash;
	const char* slaveH5SnapshotHash;
	const char* auxiliaryDemRasterSnapshotHash;
	const char* auxiliaryDemValidMaskSnapshotHash;
	const char* auxiliaryDemIdentityH5SnapshotHash;
	const char* referenceIdentityH5SourceHash;
	const char* geoidModelSnapshotHash;
	const char* referenceResourceId;
	const char* referenceResourceHash;
	const char* referenceVerticalDatum;
	const char* referenceCrs;
	const char* orbitInterpolationStrategy;
	int32_t iterations;
	int32_t azimuthCellsPerBurst;
	int32_t rangeCellsPerBurst;
	double minimumConsensusFraction;
	double maximumSparseHeightResidualMeters;
	double minimumComplexGamma;
	// Selection and validation are separate strata. A burst must retain at
	// least this many samples in each set and both sets must cover every range
	// stratum before a global K may be accepted.
	int32_t minimumSelectionCandidatesPerBurst;
	int32_t minimumValidationCandidatesPerBurst;
	// GDAL affine transform from WGS84 longitude/latitude to the managed
	// raster's pixel grid.  The caller copies the exact identity-H5 metadata;
	// the raster itself is never discovered from a project directory.
	double referenceGeoTransform[6];
	double referenceNoDataValue;
	const char* referenceVerticalPipeline;
	const char* referenceGeoidModelId;
	// Core independently resolves the phase source metadata against this root
	// and rejects anything other than these UI-frozen identities.
	const char* snapshotRoot;
	const char* phaseSource1ResolvedPath;
	const char* phaseSource1ResolvedHash;
	const char* phaseSource2ResolvedPath;
	const char* phaseSource2ResolvedHash;
	const char* geometryReferenceResolvedPath;
	const char* geometryReferenceResolvedHash;
	const char* geometryReferenceCanonicalIdentity;
	const char* expectedMasterOrbitSource;
	const char* expectedMasterOrbitSelectionReason;
	const char* expectedSlaveOrbitSource;
	const char* expectedSlaveOrbitSelectionReason;
};

// Caller-owned, fixed-capacity diagnostics returned only on an accepted
// anchor.  No full-scene diagnostic raster crosses this ABI.
struct DemAbsolutePhaseAnchorV2Result
{
	uint32_t structSize;
	uint32_t version;
	int32_t selectedK;
	int32_t candidateCount;
	double consensusFraction;
	double sparseHeightResidualStats[4]; // validationCount, mean, RMS, maxAbs
	int32_t* burstIndices;
	int32_t* candidateCountByBurst;
	int32_t* validationCountByBurst;
	// burstCount x rangeCellsPerBurst matrices, row-major, containing 0/1
	// coverage for the selection and independent validation partitions.
	int32_t* selectionRangeCoverage;
	int32_t* validationRangeCoverage;
	// One row per component x FEP-burst pair: [component_label, burst_index,
	// selection_count, validation_count].  This is intentionally not a
	// component-only aggregate: accepted global K needs both partitions for
	// every observed component/burst pair.
	int32_t* componentEvidenceTriples;
	uint32_t burstCapacity;
	uint32_t rangeCoverageCapacity;
	uint32_t componentEvidenceCapacity;
	uint32_t componentEvidenceCount;
	uint32_t burstCount;
	int32_t* kHistogramPairs; // {K, count} pairs
	uint32_t histogramCapacity;
	uint32_t histogramCount;
	char status[32];
};

constexpr uint32_t DEM_ABSOLUTE_PHASE_ANCHOR_V2_VERSION = 2;
constexpr uint32_t DEM_ABSOLUTE_PHASE_ANCHOR_V2_MIN_SIZE =
	static_cast<uint32_t>(offsetof(DemAbsolutePhaseAnchorV2CoreRequest, version) + sizeof(uint32_t));
// Request/H5 contract remains anchoring v2; this is the result-buffer layout
// revision that added component x burst evidence.  It must reject an older
// DLL rather than reinterpret its three-column evidence as four columns.
constexpr uint32_t DEM_ABSOLUTE_PHASE_ANCHOR_V2_RESULT_VERSION = 3;
constexpr uint32_t DEM_ABSOLUTE_PHASE_ANCHOR_V2_RESULT_MIN_SIZE =
	static_cast<uint32_t>(offsetof(DemAbsolutePhaseAnchorV2Result, version) + sizeof(uint32_t));

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
	// v2 never delegates to dem_newton_iter[_ex].  It requires the FEP v2
	// phase contract plus explicit external-DEM, mask and geoid snapshots.
	int dem_newton_iter_absolute_phase_anchor_v2(
		const DemAbsolutePhaseAnchorV2CoreRequest* request,
		Mat& dem,
		DemAbsolutePhaseAnchorV2Result* result,
		const DemDiagnosticOptions* diagnostics = nullptr
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
