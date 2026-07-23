#pragma once
#ifndef __FORMATCONVERSION__H__
#define __FORMATCONVERSION__H__

#include <atomic>
#include <cstdint>
#include "..\include\InSARDiagnostics.h"
#include<string>
#include<memory>
#include"..\include\Package.h"
#include"..\include\ComplexMat.h"
#define Big2Little64(A) ((uint64_t)(A&0xff00000000000000)>>56|(A&0x00ff000000000000)>>40|(A&0x0000ff0000000000)>>24|(A&0x000000ff00000000)>>8|(A&0x00000000ff000000)<<8|(A&0x0000000000ff0000)<<24|(A&0x000000000000ff00)<<40|(A&0x00000000000000ff)<<56)
#define Big2Little32(A) ((uint32_t)(A&0xff000000)>>24|(uint32_t)(A&0x00ff0000)>>8 | (uint32_t)(A&0x0000ff00)<<8|(uint32_t)(A&0x000000ff)<<24)
#define Big2Little16(A) ((uint16_t)(A&0xff00)>>8 | (uint16_t)(A&0x00ff)<<8)


/*********************************************************/
/***************   XML文件参数读写类库    ****************/
/*********************************************************/

class TiXmlElement;

class InSAR_API XMLFile
{
public:
	XMLFile();
	XMLFile(const XMLFile& other);
	XMLFile& operator=(const XMLFile& other);
	~XMLFile();

	/** @brief 创建新的工程文件
	
	@param project_path       工程路径
	@param project_name       工程�?
	@param project_version    工程文件版本
	*/
	int XMLFile_creat_new_project(
		const char* project_path,
		const char* project_name,
		const char* project_version
	);
	/** @brief 添加导入原始数据节点

	@param datanode_node  节点�?
	@param node_name      图像�?
	@param node_path      图像路径
	@param sensor         卫星
	*/
	int XMLFile_add_origin(
		const char* datanode_node,
		const char* node_name,
		const char* node_path,
		const char* sensor = "unknown"
	);
	/** @brief 添加导入原始数据节点(14_project)
	* @param datanode_node  节点�?
	* @param node_name      图像�?
	* @param node_path      图像路径
	* @param mode           收发模式�?：单发单收，2：单发双收，3：乒乓，4：双频乒乓）
	* @param sensor         卫星
	*/
	int XMLFile_add_origin_14(
		const char* datanode_node,
		const char* node_name,
		const char* node_path,
		int mode = 1,
		const char* sensor = "unknown"
	);
	/** @brief 添加裁剪图像节点

	@param datanode_node  裁剪图像节点�?
	@param master_index   裁剪节点主图序号�?-based�?
	@param node_name      裁剪图像�?
	@param node_path      裁剪图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param lon            中心经度
	@param lat            中心纬度
	@param width          裁剪宽度
	@param height         裁剪高度
	@param data_rank      数据等级
	*/
	int XMLFile_add_cut(
		const char* datenode_name,
		int master_index,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		double lon, double lat,
		double width, double height,
		const char* data_rank
	);

	/** @brief 添加裁剪图像节点
	@param datanode_node  裁剪图像节点�?
	@param master_index   裁剪节点主图序号�?-based�?
	@param node_name      裁剪图像�?
	@param node_path      裁剪图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param lon            中心经度
	@param lat            中心纬度
	@param width          裁剪宽度
	@param height         裁剪高度
	@param data_rank      数据等级
	*/
	int XMLFile_add_cut_14(
		const char* datanode_name,
		int master_index,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		double lon, double lat,
		double width, double height,
		const char* data_rank
	);

	/** @brief 添加配准图像节点

	@param datanode_node  配准图像节点�?
	@param node_name      配准图像�?
	@param node_path      配准图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param master_index   主图像序�?
	@param interp_times   插值倍数�?的n次幂�?
	@param block_size     子块尺寸�?的n次幂�?
	@param temporal_baseline 时间基线估计
	@param B_effect       垂直基线估计
	@param B_parallel     水平基线估计
	*/
	int XMLFile_add_regis(
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		int master_index, int interp_times, int block_size,
		const char* temporal_baseline, const char* B_effect, const char* B_parallel
	);

	/** @brief 添加配准图像节点
	@param mode           收发模式�?：单发单收，2：单发双收，3：乒乓，4：双频乒乓）
	@param datanode_node  配准图像节点�?
	@param node_name      配准图像�?
	@param node_path      配准图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param master_index   主图像序�?
	@param interp_times   插值倍数�?的n次幂�?
	@param block_size     子块尺寸�?的n次幂�?
	@param temporal_baseline 时间基线估计
	@param B_effect       垂直基线估计
	@param B_parallel     水平基线估计
	*/
	int XMLFile_add_regis14(
		int mode,
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		int master_index, int interp_times, int block_size,
		const char* temporal_baseline, const char* B_effect, const char* B_parallel
	);

	/** @brief 添加后向地理编码配准节点
	* @param dataNode            配准图像数据节点�?
	* @param dataName            配准图像数据�?
	* @param dataPath            配准图像数据储存路径（相对路径）
	* @param masterIndex         主图像序�?
	* @return 成功返回0，否则返�?1
	*/
	int XMLFile_add_backgeocoding(
		const char* dataNode,
		const char* dataName,
		const char* dataPath,
		int masterIndex
	);
	/** @brief 添加单视复图像去参考相位节�?
	* @param dataNode            配准图像数据节点�?
	* @param dataName            配准图像数据�?
	* @param dataPath            配准图像数据储存路径（相对路径）
	* @param masterIndex         主图像序�?
	* @return 成功返回0，否则返�?1
	*/
	int XMLFile_add_SLC_deramp(
		const char* dataNode,
		const char* dataName,
		const char* dataPath,
		int masterIndex
	);

	/** @brief 添加单视复图像去参考相位节�?
	* @param mode                收发模式�?：单发单收，2：单发双收，3：乒乓模式，4：双频乒乓模式）
	* @param dataNode            配准图像数据节点�?
	* @param dataName            配准图像数据�?
	* @param dataPath            配准图像数据储存路径（相对路径）
	* @param masterIndex         主图像序�?
	* @return 成功返回0，否则返�?1
	*/
	int XMLFile_add_SLC_deramp_14(
		int mode,
		const char* dataNode,
		const char* dataName,
		const char* dataPath,
		int masterIndex
	);

	/** @brief 添加小基线集时间序列分析节点
	* @param dataNode            SBAS时间序列分析数据节点�?
	* @param dataName            SBAS时间序列分析数据�?
	* @param dataPath            SBAS时间序列分析图像数据储存路径（相对路径）
	* @return 成功返回0，否则返�?1
	*/
	int XMLFile_add_SBAS(
		const char* dataNode,
		const char* dataName,
		const char* dataPath
	);
	/** @brief 添加哨兵一号burst拼接节点
	* @param dataNode            deburst图像数据节点�?
	* @param dataName            deburst图像数据�?
	* @param dataPath            deburst图像数据储存路径（相对路径）
	* @return 成功返回0，否则返�?1
	*/
	int XMLFile_add_S1_Deburst(
		const char* dataNode,
		const char* dataName,
		const char* dataPath
	);
	/** @brief 添加地理编码节点
	* @param dataNode            地理编码图像数据节点�?
	* @param dataName            地理编码图像数据�?
	* @param dataPath            地理编码图像数据储存路径（相对路径）
	* @param level               数据等级
	* @return 成功返回0，否则返�?1
	*/
	int XMLFile_add_geocoding(
		const char* dataNode,
		const char* dataName,
		const char* dataPath,
		const char* level
	);

	/** @brief 添加干涉相位生成节点
	* @param datanode_node                 干涉相位图像节点�?
	* @param node_name                     干涉相位图像�?
	* @param node_path                     干涉相位图像路径
	* @param master_name                   干涉相位主图�?
	* @param rank                          节点等级
	* @param offset_row                    主图像行偏移�?
	* @param offset_col                    主图像列偏移�?
	* @param multilook_rg                  多视倍数（距离向�?
	* @param multilook_az                  多视倍数（方位向�?
	* @return 成功返回0，否则返�?1
	*/
	int XMLFile_add_interferometric_phase_14(
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		const char* master_name,
		const char* rank,
		int offset_row,
		int offset_col,
		int multilook_rg,
		int multilook_az
	);

	/** @brief 添加干涉相位生成节点

	@param datanode_node  干涉相位图像节点�?
	@param node_name      干涉相位图像�?
	@param node_path      干涉相位图像路径
	@param master_name    干涉相位主图�?
	@param rank			  节点等级
	@param offset_row     主图像行偏移�?
	@param offset_col     主图像列偏移�?
	@param isdeflat       是否去平�?
	@param istopo_removal 是否去地�?
	@param iscoherence    是否估计相干系数
	@param win_w          相干系数估计窗口宽度
	@param win_h          相干系数估计窗口高度
	@param multilook_rg   多视倍数（距离向�?
	@param multilook_az   多视倍数（方位向�?
	*/
	int XMLFile_add_interferometric_phase(
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		const char* master_name,
		const char* rank,
		int offset_row, int offset_col,
		int isdeflat, int istopo_removal, int iscoherence,
		int win_w, int win_h, int multilook_rg, int multilook_az
	);

	/** @brief 添加滤波图像节点
	@param mode           收发模式�?：单发单收，2：单发双收，3：乒乓模式，4：双频乒乓模式）
	@param datanode_node  滤波图像节点�?
	@param node_name      滤波图像�?
	@param node_path      滤波图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param method		  方法名称
	@param Slop_win		  斜坡自适应窗口尺寸
	@param Pre_win		  预窗口尺�?
	@param Goldstein_win  Goldstein滤波FFT窗口尺寸
	@param Goldstein_filled_win		Goldstein滤波补零窗口尺寸
	@param alpha		  Goldstein滤波阈�?
	@param filter_dl_path			深度学习滤波可执行程序路�?
	@param dl_model_file			深度学习滤波模型路径
	@param tmp_path        深度学习滤波中间文件路径
	*/
	int XMLFile_add_denoise_14(
		int mode,
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		const char* method,
		int Slop_win, int Pre_win,
		int Goldstein_win, int Goldstein_filled_win, double alpha,
		const char* filter_dl_path, const char* dl_model_file, const char* tmp_path
	);

	/** @brief 添加滤波图像节点

	@param datanode_node  滤波图像节点�?
	@param node_name      滤波图像�?
	@param node_path      滤波图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param method		  方法名称
	@param Slop_win		  斜坡自适应窗口尺寸
	@param Pre_win		  预窗口尺�?
	@param Goldstein_win  Goldstein滤波FFT窗口尺寸
	@param Goldstein_filled_win		Goldstein滤波补零窗口尺寸
	@param alpha		  Goldstein滤波阈�?
	@param filter_dl_path			深度学习滤波可执行程序路�?
	@param dl_model_file			深度学习滤波模型路径
	@param tmp_path        深度学习滤波中间文件路径
	*/
	int XMLFile_add_denoise(
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		const char* method,
		int Slop_win, int Pre_win,
		int Goldstein_win, int Goldstein_filled_win, double alpha,
		const char* filter_dl_path, const char* dl_model_file, const char* tmp_path
	);
	/** @brief 添加解缠图像节点

	@param datanode_node  解缠图像节点�?
	@param node_name      解缠图像�?
	@param node_path      解缠图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param method		  方法名称
	@param threshold	  综合法阈�?
	*/
	int XMLFile_add_unwrap(
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		const char* method,
		double threshold
	);

	/** @brief 添加解缠图像节点
	@param mode           收发模式�?：单发单收，2：单发双收，3：乒乓模式，4：双频乒乓模式）
	@param datanode_node  解缠图像节点�?
	@param node_name      解缠图像�?
	@param node_path      解缠图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param method		  方法名称
	@param threshold	  综合法阈�?
	*/
	int XMLFile_add_unwrap_14(
		int mode,
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		const char* method,
		double threshold
	);

	/** @brief 添加Dem图像节点

	@param datanode_node  Dem图像节点�?
	@param node_name      Dem图像�?
	@param node_path      Dem图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param method		  方法名称
	@param threshold	  迭代次数
	*/
	int XMLFile_add_dem(
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		const char* method,
		int times
	);

	/** @brief 添加Dem图像节点
	@param mode           收发模式�?：单发单收，2：单发双收，3：乒乓模式，4：双频乒乓模式）
	@param datanode_node  Dem图像节点�?
	@param node_name      Dem图像�?
	@param node_path      Dem图像路径
	@param Row_offset     行偏移量
	@param Col_offset     列偏移量
	@param method		  方法名称
	@param threshold	  迭代次数
	*/
	int XMLFile_add_dem_14(
		int mode,
		const char* datanode_name,
		const char* node_name,
		const char* node_path,
		int Row_offset,
		int Col_offset,
		const char* method,
		int times
	);

	/** @brief 删除图像节点
	@param datanode_node  待删除图像节点名
	@param node_name      待删除图像名
	@param node_path      待删除图像路�?
	*/
	int XMLFile_remove_node(
		const char* datanode_name,
		const char* node_name,
		const char* node_path
	);

	/** @brief 返回字符�?

	@param n			输入整数�?
	*/
	string int2str(int n);
	/** @brief 返回整型

	@param s			输入字符�?
	*/
	int str2int(const string& s);
	/** @brief 保存XML

	@param save_path    保存路径
	*/
	int XMLFile_save(
		const char* save_path
	);




	/*XML文件加载
	* 参数1：待加载文件�?
	*/
	int XMLFile_load(const char* xmlFileName);
	/*
	* 按名称查找节�?
	* 参数1：根节点（返回值）
	*/
	int get_root(TiXmlElement*& root);
	/*
	* 按节点查询子节点个数
	* 参数1：节点名
	* 参数2：子节点数量（返回值）
	*/
	int get_children_count(TiXmlElement* pRoot, int* count);
	/*
	* 按名称查找节�?
	* 参数1：节点名
	* 参数2：节点名
	* 参数3：节点指针（返回值）
	*/
	int _find_node(TiXmlElement* pRoot, const char* node_name, TiXmlElement*& pnode);
	/*
	* 按名称查找节�?
	* 参数1：节点名
	* 参数2：节点指针（返回值）
	*/
	int find_node(const char* node_name, TiXmlElement*& pnode);
	/*
	* 按名称及属性值查找节�?
	* 参数1：根节点（遍历用，非文件根节点）
	* 参数2：节点名
	* 参数3：节点属性名（关键属性）
	* 参数4：节点属性�?
	* 参数5：节点指针（返回值）
	*/
	int find_node_with_attribute(
		TiXmlElement* pRoot,
		const char* node_name,
		const char* attribute_name,
		const char* attribute_value,
		TiXmlElement*& pnode);

	/*
	* 按名称及属性值查找节�?
	* @param             节点�?
	* @param             节点属性名
	* @param             节点属性�?
	* @param             节点指针（返回值）
	* @return 成功找到返回0�?否则返回-1
	*/
	int find_node_with_attribute(
		const char* node_name,
		const char* attribute_name,
		const char* attribute_value,
		TiXmlElement*& pnode
	);

	/*
	* 从XML文件中读出字符串参数
	* 参数1：参数名（节点名�?
	* 参数2：参数值（输出�?
	*/
	int get_str_para(const char* node_name, string& value);
	/*
	* 从XML文件中读出double类型参数
	* 参数1：参数名（节点名�?
	* 参数2：参数值（输出�?
	*/
	int get_double_para(const char* node_name, double* value);
	/* 从xml文件中读出double数组
	* @param node_name                   数据节点�?
	* @param Array                       数组
	* @param rootNode                    根节点（默认为NULL，若提供根节点，则在根节点下面搜索）
	* @return 成功返回0，否则返�?1
	*/
	int getDoubleArray(const char* node_name, Mat& Array, TiXmlElement* rootNode = NULL);
	/*
	* 从XML文件中读出整型参�?
	* 参数1：参数名（节点名�?
	* 参数2：参数值（输出�?
	*/
	int get_int_para(const char* node_name, int* value);
	/* 从xml文件中读出int数组
	* @param node_name                   数据节点�?
	* @param Array                       数组
	* @param rootNode                    根节点（默认为NULL，若提供根节点，则在根节点下面搜索）
	* @return 成功返回0，否则返�?1
	*/
	int getIntArray(const char* node_name, Mat& Array, TiXmlElement* rootNode = NULL);



	/*************************************************/
	/**********   Sensor-specific functions **********/
	/*************************************************/


	/*
	* 从TerraSAR-X GEOREF.xml文件中读出控制点参数
	* 参数1：控制点参数（输出值，N×6矩阵，每列分别为：经，纬，高，行，列，下视角�?
	*/
	int get_gcps_from_TSX(Mat& gcps);
	/*
	* 从TerraSAR-X的主XML文件中读出轨道数�?
	* 参数1：轨道参数（输出值，N×7矩阵，每列分别是：GPS时间，位置，速度�?
	*/
	int get_stateVec_from_TSX(Mat& stateVec);
	/*
	* 从TerraSAR-X主xml文件中读出多普勒中心频率参数估计系数
	* 参数1：多普勒中心频率（输出值， N×(N_order+2)矩阵，N为多普勒中心估计数，N_order为斜距向多普勒中心拟合阶�? 另外两个是零阶系数和参考点距离向时间）
	*/
	int get_dopplerCentroid_from_TSX(Mat& doppler);


	/*
	* 从sentinel1卫星数据xml文件中读出地面控制点数据
	* 参数1：控制点数据（输出值，N×6矩阵，每列分别为：经，纬，高，行，列，下视角�?
	*/
	int get_gcps_from_sentinel(Mat& gcps);
	/*
	* 从sentinel1卫星数据xml文件中读出多普勒中心频率参数
	* 参数1：多普勒中心频率（输出值， N×(N_order+2)矩阵，N为多普勒中心估计数，N_order为斜距向多普勒中心拟合阶�? 另外两个是零阶系数和参考点距离向时间）
	*/
	int get_dopplerCentroid_from_sentinel(Mat& doppler);
	/*
	* 从sentinel1卫星数据xml文件中读出轨道参�?
	* 参数1：轨道参数（输出值，N×7矩阵，每列分别是：GPS时间，位置，速度�?
	*/
	int get_stateVec_from_sentinel(Mat& stateVec);


private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

};



/**************************************************/
/*********           格式转换类库        **********/
/**************************************************/



/* // Task-scoped native diagnostics. The callback is synchronous: string pointers
// remain valid only for the duration of the callback and implementations must
// be thread-safe and must not throw across the DLL boundary.
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

typedef void (__stdcall *InSARDiagnosticCallback)(const InSARDiagnosticEvent* event, void* userData); */

class InSAR_API FormatConversion
{
public:
	FormatConversion();
	~FormatConversion();

	/*
	* 功能：将字符串格式的UTC时间转换为GPS时间
	* 参数1：UTC时间
	* 参数2：GPS时间
	*/
	int utc2gps(const char* utc_time, double* gps_time);
	/** @brief 创建新的h5文件（若文件已存在则覆盖�?

	@param filename     文件�?
	*/
	int creat_new_h5(const char* filename);
	/** @brief 获取 HDF5 数据集维�?(Rows �?Cols)
	* @param filename       H5 文件�?
	* @param dataset_name   数据集名
	* @param rows           输出行数指针
	* @param cols           输出列数指针
	* @return 成功返回 0，否则返�?-1
	*/
	int get_dataset_dims(
		const char* filename,
		const char* dataset_name,
		int* rows,
		int* cols
	);
	/** @brief 功能：向h5文件中写入零矩阵
	* @param filename                文件�?
	* @param dataset_name            dataset�?
	* @param type                    数据类型（CV_8U/CV_16S/CV_32S/CV_32F/CV_64F�?
	* @param rows                    dataset行数
	* @param cols                    dataset列数
	* @return 成功返回0，否则返�?1
	*/
	int write_zero_array_to_h5(const char* filename, const char* dataset_name, int type, int rows, int cols);
	/** @brief �?H5 文件中创建一个空的且延迟分配物理空间的二�?Dataset 结构
	* @param filePath                      H5 文件路径
	* @param datasetName                   数据集名�?
	* @param rows                          数据集总行�?
	* @param cols                          数据集总列�?
	* @param dataType                      OpenCV 数据类型 (CV_32F, CV_64F, CV_16S �?
	* @param chunkRows                     分块行数（默�?56，可根据 Burst 写入尺寸优化�?
	* @param chunkCols                     分块列数（默�?56，可根据 Burst 写入尺寸优化�?
	* @return 成功返回0�?否则返回-1
	*/
	int create_empty_dataset(
		const char* filePath,
		const char* datasetName,
		int rows,
		int cols,
		int dataType,
		int chunkRows = 256,
		int chunkCols = 256
	);
	/*
	* 功能：向h5文件中写入实数矩阵（干涉相位，相干系数等�?input_array可以�?6位整型或者double/float�?
	* 参数1：文件名
	* 参数2：dataset�?
	* 参数3 待写入矩�?
	*/
	int write_array_to_h5(const char* filename, const char* dataset_name, const Mat& input_array);
	/** @brief 向h5文件中写入double类型�?
	* @param h5File                     h5文件
	* @param datasetName                数据�?
	* @param data                       double型数�?
	* @return 成功返回0，否则返�?1
	*/
	int write_double_to_h5(
		const char* h5File,
		const char* datasetName,
		double data
	);
	/** @brief 向h5文件中写入int类型�?
	* @param h5File                     h5文件
	* @param datasetName                数据�?
	* @param data                       double型数�?
	* @return 成功返回0，否则返�?1
	*/
	int write_int_to_h5(
		const char* h5File,
		const char* datasetName,
		int data
	);
	/*
	* 功能：从h5文件中读出实数矩�?读出类型为double型�?6位整型或�?2位整�?
	* 参数1：文件名
	* 参数2：dataset�?
	* 参数3：输出矩�?
	*/
	int read_array_from_h5(const char* filename, const char* dataset_name, Mat& out_array);
	/** @brief 从h5文件中读出double数据
	* @param h5File                      h5文件
	* @param datasetName                 数据�?
	* @param data                        数据
	* @return 成功返回0，否则返�?1
	*/
	int read_double_from_h5(
		const char* h5File,
		const char* datasetName,
		double* data
	);
	/** @brief 从h5文件中读出int数据
	* @param h5File                      h5文件
	* @param datasetName                 数据�?
	* @param data                        数据
	* @return 成功返回0，否则返�?1
	*/
	int read_int_from_h5(
		const char* h5File,
		const char* datasetName,
		int* data
	);
	/*
	* 功能：从h5文件中读取矩阵数据子�?
	* 参数1：h5文件�?
	* 参数2：dataset�?
	* 参数3：行偏移�?�?开�?
	* 参数4：列偏移量（�?开始）
	* 参数5：子集行�?
	* 参数6：子集列�?
	* 参数7：输出子集矩�?
	*/
	int read_subarray_from_h5(
		const char* filename,
		const char* dataset_name,
		int offset_row,
		int offset_col,
		int rows_subarray,
		int cols_subarray,
		Mat& out_array
	);
	/** @brief 向已有的H5文件指定数据集矩阵中的指定位置写入子矩阵

	@param h5_filename                   h5文件�?
	@param dataset_name                  数据集名�?
	@param subarray                      子矩阵数�?
	@param offset_row                    行偏移量（从0开始）
	@param offset_col                    列偏移量（从0开始）
	@param rows_subarray                 子集（矩阵）行数
	@param cols_subarray                 子集（矩阵）列数
	@return 成功返回0�?否则返回-1
	*/
	int write_subarray_to_h5(
		const char* h5_filename,
		const char* dataset_name,
		Mat& subarray,
		int offset_row,
		int offset_col,
		int rows_subarray,
		int cols_subarray
	);
	/*
	* 功能：向h5文件写入字符串参�?
	* 参数1：文件名
	* 参数2：dataset�?
	* 参数3：待写入字符串参�?
	*/
	int write_str_to_h5(const char* filename, const char* dataset_name, const char* str);
	/*
	* 功能：从h5文件中读出字符串参数
	* 参数1：文件名
	* 参数2：dataset�?
	* 参数3：输出字符串参数
	*/
	int read_str_from_h5(const char* filename, const char* dataset_name, string& string);
	/*
	* 功能：向h5文件写入复图像数据（SLC），如果已经存在则不写入�?
	* 参数1：文件名
	* 参数2：复数据
	*/
	int write_slc_to_h5(const char* filename, const ComplexMat& slc);
	/*
	* 功能：从h5文件中读出slc数据
	* 参数1：文件名
	* 参数2：输出slc
	*/
	int read_slc_from_h5(const char* filename, ComplexMat& slc);


	/*------------------------------------------------*/
	/*            TerraSAR-X产品数据导入工具          */
	/*------------------------------------------------*/

	/*
	* 从TerraSAR-X卫星�?cos数据中读出slc数据(不改变类型，仍然�?6位整�?
	* 参数1�?cos文件�?
	* 参数2：复数据矩阵（输出值）
	*/
	int read_slc_from_TSXcos(const char* filename, ComplexMat& slc);
	/** @brief 将TerraSAR-X卫星数据格式转换为自定义的h5格式

	@param cosar_filename                     TerraSAR-X .cos文件�?
	@param xml_filename                       TerraSAR-X 主xml文件�?
	@param GEOREF_filename                    TerraSAR-X GEOREF.xml文件�?
	@param dst_h5_filename                    目标h5文件（若文件已经存在则覆盖）
	@return 成功返回0，否则返�?1
	*/
	int TSX2h5(
		const char* cosar_filename,
		const char* xml_filename,
		const char* GEOREF_filename,
		const char* dst_h5_filename,
		ProgressCallback progressCallback = nullptr,
		void* userData = nullptr
	);
	/** @brief 将TerraSAR-X卫星数据格式转换为自定义的h5格式

	@param xml_filename                       TerraSAR-X 主xml文件�?
	@param dst_h5_filename                    目标h5文件
	@param polarization                       极化方式
	@param progressCallback                   进度回调函数
	@param userData                           用户自定义数�?
	@return 成功返回0，否则返�?1
	*/
	int TSX2h5(
		const char* xml_filename,
		const char* dst_h5_filename,
		const char* polarization = nullptr,
		ProgressCallback progressCallback = nullptr,
		void* userData = nullptr
	);
	int TSX2h5(
		const char* xml_filename,
		const char* dst_h5_filename,
		ProgressCallback progressCallback,
		void* userData = nullptr
	);





	/*------------------------------------------------*/
	/*            Sentinel1产品数据导入工具           */
	/*------------------------------------------------*/

	/*
	* 从sentinel1卫星精密轨道数据文件中读出精密轨道数�?
	* 参数1：精密轨道数据文�?
	* 参数2：粗轨道数据起始时间
	* 参数3：粗轨道数据结束时间
	* 参数4：目标h5文件
	*/
	int read_POD(const char* POD_filename, double start_time, double stop_time, const char* dst_h5_filename);
	/** @brief 从sentinel1卫星数据中读出slc数据（读出数据类型为16位整型）
	* 
	* @param filename                    sentinel1卫星数据文件�?
	* @param xml_filename                xml参数文件
	* @param slc                         复矩阵（读出的slc数据�?
	* @param gcps_line                   deburst之后控制点行坐标（int型，1×n�?
	* @return 成功返回0，否则返�?1
	*/
	int read_slc_from_Sentinel(const char* filename, const char* xml_filename, ComplexMat& slc, Mat& gcps_line, ProgressCallback progressCallback = nullptr, void* userData = nullptr);
	/*
	* 功能：sentinel1数据deburst(数据类型�?6位整�?
	* 参数1：用于deburst的xml参数文件
	* 参数2：待处理slc数据（原地操作）
	* 参数3：无效行积累标志数据(CV_32S�?
	*/
	int sentinel_deburst(const char* xml_filename, ComplexMat& slc, Mat& sentinel);
	/*
	* 将sentinel1卫星数据格式转换为自定义的h5格式
	* 参数1：tiff格式文件名（储存SLC图像�?
	* 参数2：xml文件�?
	* 参数3：目标h5文件�?
	* 参数4：精密轨道数据文�?
	*/
	int sentinel2h5(
		const char* tiff_filename,
		const char* xml_filename,
		const char* dst_h5_filename,
		const char* POD_file = NULL
	);
	int sentinel2h5(
		const char* tiff_filename,
		const char* xml_filename,
		const char* dst_h5_filename,
		const char* POD_file,
		ProgressCallback progressCallback,
		void* userData
	);
	/** 导入sentinel卫星数据至h5文件�?
	* @param manifest                            sentinel卫星数据manifest文件
	* @param subswath_name                       sentinel卫星IW模式中为（iw1/iw2/iw3�?
	* @param polarization                        极化方式（vv/vh�?
	* @param dest_h5_file                        目标h5文件
	* @param PODFile                             精轨数据文件
	* @return 成功返回0，否则返�?1
	*/
	int import_sentinel(
		const char* manifest,
		const char* subswath_name,
		const char* polarization,
		const char* dest_h5_file,
		const char* PODFile = NULL,
		int start_burst = -1,
		int end_burst = -1
	);
	int import_sentinel(
		const char* manifest,
		const char* subswath_name,
		const char* polarization,
		const char* dest_h5_file,
		const char* PODFile,
		ProgressCallback progressCallback,
		void* userData,
		int start_burst = -1,
		int end_burst = -1
	);
	/** @brief 读出一个burst数据
	* 
	* @param pnode                         burst节点信息
	* @param xmldoc                        xml结构�?
	* @param fp                            图像文件指针
	* @param linesPerBurst                 每个burst数据行数
	* @param samplesPerBurst               每行数据点数
	* @param burst                         读出的burst数据
	* @return 成功返回0，否则返�?1（返�?1会自动关闭文件指针）
	*/
	int get_a_burst(
		TiXmlElement* pnode,
		XMLFile& xmldoc,
		FILE*& fp,
		int linesPerBurst,
		int samplesPerBurst,
		ComplexMat& burst
	);
	/** @brief 读出一个burst数据（并计算与下一个burst之间的重叠区域大小）
	*
	* @param burst_num                         burst序号
	* @param xml_file                          xml文件
	* @param tiff_file                         tiff数据文件
	* @param burst                             burst数据
	* @param overlapSize                       重叠区域尺寸（方位向�?
	*/
	int get_burst_sentinel(
		int burst_num,
		const char* xml_file,
		const char* tiff_file,
		ComplexMat& burst,
		int* overlapSize
	);
	/** @brief 计算burst之间的重叠区域尺�?
	* 
	* @param last_burst                      上一个burst
	* @param this_burst                      待计算重叠区域尺寸的burst
	* @param overlapSize                     重叠区域尺寸
	* @return 成功返回0，否则返�?1
	*/
	int deburst_overlapSize(
		ComplexMat& last_burst,
		ComplexMat& this_burst,
		int* overlapSize
	);
	/** @brief burst之间进行拼接（将src_burst拼接到dst_burst上）
	* 
	* @param src_burst                 被拼接burst
	* @param dst_burst                 拼接burst
	* @param stitch_type               缝合方式（low/mid/high�?
	* @param overlapSize               重叠区域尺寸
	* @return 成功返回0，否则返�?1
	*/
	int burst_stitch(
		ComplexMat& src_burst,
		ComplexMat& dst_burst,
		int overlapSize,
		const char* stitch_type = "mid"
	);



	/*------------------------------------------------*/
	/*            ALOS1/2产品数据导入工具             */
	/*------------------------------------------------*/

	/*
	* 从ALOS1/2 CEOS格式Level-1.1产品中读取slc数据(float�?
	* 参数1：图像文�?
	* 参数2：slc数据
	*/
	int read_slc_from_ALOS(const char* img_file, ComplexMat& slc);
	/*
	* 从ALOS1/2 CEOS格式Level-1.1产品中读取卫星轨道数�?
	* 参数1：ALOS LED文件
	* 参数2：轨道数�?
	*/
	int read_stateVec_from_ALOS(const char* LED_file, Mat& stateVec);
	/*
	* 从ALOS1/2 CEOS格式Level-1.1产品中读取经纬坐标与图像坐标之间的转换关系系�?
	* 参数1：ALOS LED文件
	* 参数2：图像坐标（行、列）与经度之间的转换关系（行列-->经度�?
	* 参数3：图像坐标（行、列）与纬度之间的转换关系（行列-->纬度�?
	* 参数4：经纬度与行坐标之间的转换关系（经纬�?->行数�?
	* 参数5：经纬度与列坐标之间的转换关系（经纬�?->列数�?
	*/
	int read_conversion_coefficient_from_ALOS(
		const char* LED_file,
		Mat& lon_coefficient,
		Mat& lat_coefficient,
		Mat& row_coefficient,
		Mat& col_coefficient
	);
	/*
	* 将ALOS1/2 CEOS格式Level-1.1产品数据读出到自定义的hdf5文件�?
	* 参数1：ALOS IMG文件
	* 参数2：ALOS LED文件
	* 参数3：自定义h5文件
	*/
	int ALOS2h5(const char* IMG_file, const char* LED_file, const char* dst_h5);
	int ALOS2h5(
		const char* IMG_file,
		const char* LED_file,
		const char* dst_h5,
		ProgressCallback progressCallback,
		void* userData
	);
	/** @brief 将原h5文件中的参数信息拷贝到另一个h5�?
	
    @param Input_file        原始h5文件
	@param Output_file       输出h5文件
	*/
	int Copy_para_from_h5_2_h5(const char* Input_file, const char* Output_file);

	/*------------------------------------------------*/
	/*               GEDI产品数据导入工具             */
	/*------------------------------------------------*/

	/** @brief 从GEDI L2B级产品中读取DSM、DTM以及地理坐标等信�?
	* @param gedi_h5_file                        GEDI L2B级产品文件（HDF5格式�?
	* @param rh100                               rh100参数
	* @param elev_lowestmode                     林下地形参数（DTM�?
	* @param elev_highestreturn                  冠层高度信息（DSM�?
	* @param lon                                 经度信息
	* @param lat                                 纬度信息
	* @param dem                                 TanDEM-X 90m 高程信息
	* @param quality_index                       质量信息
	* @return 成功返回0，否则返�?1
	*/
	int read_height_metric_from_GEDI_L2B(const char* gedi_h5_file, Mat& rh100, Mat& elev_lowestmode, Mat& elev_highestreturn, Mat& lon, Mat& lat, Mat& dem, Mat& quality_index);

	/** @brief 从GEDI L2A级产品中读取树高及地理坐标等信息
	* @param gedi_h5_file                        GEDI L2A级产品文件（HDF5格式�?
	* @param rh                                  rh参数
	* @param lon                                 经度信息
	* @param lat                                 纬度信息
	* @param dem                                 TanDEM-X 90m 高程信息
	* @param quality_index                       质量信息
	* @param rh_percentile                       树高提取参数（rh_percentile = 1~100，默认为100�?
	* @return 成功返回0，否则返�?1
	*/
	int read_height_metric_from_GEDI_L2A(const char* gedi_h5_file, Mat& rh, Mat& lon, Mat& lat, Mat& dem, Mat& quality_index, int rh_percentile = 100);
	/** @brief 从ICESat-2 L3A级产品中读取树高及地理坐标等信息
	* @param gedi_h5_file                        ICESat-2 L3A级产品文件（HDF5格式�?
	* @param rh                                  rh参数
	* @param lon                                 经度信息
	* @param lat                                 纬度信息
	* @param dem                                 高程信息
	* @param quality_index                       质量信息([80,100]则为质量合格数据�?27为无效�?
	* @param rh_percentile                       树高提取参数（rh_percentile = 10(1), 15(2), 20(3), 25(4), 30(...),..., 80(15), 85(16), 90(17), 95(18)，默认为95(18)�?
	* @return 成功返回0，否则返�?1
	*/
	int read_height_metric_from_ICESat_2_L3A(const char* ICESat_2_h5_file, Mat& rh, Mat& lon, Mat& lat, Mat& dem, Mat& quality_index, int rh_percentile = 18);



	/** @brief �?�?�?---> x/y/z
	* @param lon                 经度
	* @param lat                 纬度
	* @param elevation           高度
	* @param xyz                 x/y/z
	* @return 成功返回0，否则返�?1
	*/
	static int ell2xyz(double lon, double lat, double elevation, Position& xyz);
	/** @brief 将相位转换成cos和sin（实部和虚部，支持double和float�?

	@param phase                     输入相位
	@param cos                       实部
	@param sin                       虚部
	@return 成功返回0，否则返�?1
	*/
	int phase2cos(const Mat& phase, Mat& cos, Mat& sin);
	/** @brief 生成范德蒙矩�?
	* @param inArray                           自变量序�?
	* @param vandermondeMatrix                 范德蒙矩�?
	* @param degree                            阶数
	* @return 成功返回0，否则返�?1
	*/
	static int createVandermondeMatrix(
		Mat& inArray,
		Mat& vandermondeMatrix,
		int degree
	);
	/** @brief 多项式拟合（Ax=b�?
	* @param A
	* @param b
	* @param x
	* @return 成功返回0，否则返�?1
	*/
	static int polyFit(
		Mat& A,
		Mat& b,
		Mat& x
	);
	/** @brief polyVal
	* @param coefficient
	* @param x
	* @param val
	*/
	static int polyVal(Mat& coefficient, double x, double* val);
	/*求取两幅辅图像的实相关函�?
	 参数1 主图像（复）
	 参数2 辅图像（复）
	 参数3 行偏移量（返回值）
	 参数4 列偏移量（返回值）
	*/
	int real_coherent(const ComplexMat& Master, const ComplexMat& Slave, int* offset_row, int* offset_col);
	/*2D FFTSHIFT(原地操作)*/
	int fftshift2(Mat& matrix);
	/*2D FFT
	 参数1 输入矩阵
	 参数2 输出矩阵
	*/
	int fft2(Mat& Src, Mat& Dst);

private:
	char error_head[256];
	char parallel_error_head[256];

};

struct BurstIndices
{
	int firstBurstIndex;
	int secondBurstIndex;
	bool inUpperPartOfFirstBurst;
	bool inUpperPartOfSecondBurst;
	/*默认构造函�?/
*/
	BurstIndices()
	{
		firstBurstIndex = secondBurstIndex = -1;
		inUpperPartOfFirstBurst = inUpperPartOfSecondBurst = false;
	}
	/*拷贝构造函�?/
*/
	BurstIndices(const BurstIndices& cp)
	{
		this->firstBurstIndex = cp.firstBurstIndex;
		this->inUpperPartOfFirstBurst = cp.inUpperPartOfFirstBurst;
		this->inUpperPartOfSecondBurst = cp.inUpperPartOfSecondBurst;
		this->secondBurstIndex = cp.secondBurstIndex;
	}
	/*赋值函�?/
*/
	BurstIndices& operator=(const BurstIndices& cp)
	{
		this->firstBurstIndex = cp.firstBurstIndex;
		this->inUpperPartOfFirstBurst = cp.inUpperPartOfFirstBurst;
		this->inUpperPartOfSecondBurst = cp.inUpperPartOfSecondBurst;
		this->secondBurstIndex = cp.secondBurstIndex;
		return *this;
	}
};

/*---------------------------------------*/
/*              数字高程模型             */
/*---------------------------------------*/
class InSAR_API DigitalElevationModel
{
public:
	DigitalElevationModel();
	~DigitalElevationModel();
	/** @brief 计算SRTM高程文件�?
	* @param lonMin                       最小经�?
	* @param lonMax                       最大经�?
	* @param latMin                       最小纬�?
	* @param latMax                       最大纬�?
	* @param name                         文件�?
	* @return 成功返回0，否则返�?1
	*/
	int getSRTMFileName(
		double lonMin,
		double lonMax,
		double latMin,
		double latMax,
		vector<string>& name
	);
	/** @brief 下载SRTM高程数据
	* @param name                         文件�?
	* @return 成功返回0，否则返�?1
	*/
	int downloadSRTM(const char* name);
	/** @brief 获取数字高程模型
	* @param filepath                     文件路径
	* @param lonMin                       最小经�?
	* @param lonMax                       最大经�?
	* @param latMin                       最小纬�?
	* @param latMax                       最大纬�?
	* @return 成功返回0，否则返�?1
	*/
	int getRawDEM(
		const char* filepath,
		double lonMin,
		double lonMax,
		double latMin,
		double latMax
	);
	/** @brief 根据经纬度获取高�?平均插值法)
	* @param lon                          经度
	* @param lat                          纬度
	* @param elevation                    高度
	* @return 成功返回0，否则返�?1
	*/
	int getElevation(
		double lon,
		double lat,
		double* elevation
	);
	/** @brief 读取SRTM中的geotiff高程数据
	* @param geotiffFile                  geotiff文件
	* @param outDEM                       读出的DEM数据
	* @return 成功返回0，否则返�?1
	*/
	static int geotiffread(
		const char* geotiffFile,
		Mat& outDEM
	);
	/** @brief 解压文件到指定文件夹
	* @param srcFile                      待解压压缩文�?
	* @param dstPath                      目标文件�?
	* @return 成功返回0，否则返�?1
	*/
	static int unzip(const char* srcFile, const char* dstPath);

public:
	/*DEM数据（高程为short型数据）*/
	Mat rawDEM;
	/*DEM数据行数*/
	int rows;
	/*DEM数据列数*/
	int cols;
	/*左上角经�?/
*/
	double lonUpperLeft;
	/*左上角纬�?/
*/
	double latUpperLeft;
	/*右下角经�?/
*/
	double lonLowerRight;
	/*右下角纬�?/
*/
	double latLowerRight;
	/*DEM经度采样间隔*/
	double lonSpacing;
	/*DEM纬度采样间隔*/
	double latSpacing;
	/*DEM路径*/
	string DEMPath;
	/*SRTM全球高程url*/
	string SRTMURL = "http://srtm.csi.cgiar.org/wp-content/uploads/files/srtm_5x5/TIFF/";

	char error_head[512];

};



/*--------------------------------------*/
/*              卫星轨道数据            */
/*--------------------------------------*/
class orbitStateVectors;
#if 0
class InSAR_API orbitStateVectors
{
public:
	orbitStateVectors(const Mat& stateVectors, double startTime, double stopTime);
	orbitStateVectors(const Mat& stateVectors, double startTime, double stopTime, double delta_time);
	~orbitStateVectors();
	/** @brief 设置场景拍摄起始终止时间
	* @param startTime
	* @param stopTime
	* @return 成功返回0，否则返�?1
	*/
	int setSceneStartStopTime(double startTime, double stopTime);
	/** @brief 获取拍摄起始时刻
	* @return 返回拍摄起始时刻
	*/
	double get_start_time();
	/** @brief 获取拍摄结束时刻
	* @return 返回拍摄结束时刻
	*/
	double get_stop_time();
	/** @brief 获取卫星三维位置信息（拉格朗日插值）
	* @param azimuthTime                   方位向时�?
	* @param position                      卫星三维位置
	* @return 成功返回0，否则返�?1
	*/
	int getPosition(double azimuthTime, Position& position);
	/** @brief 获取卫星三维速度信息（拉格朗日插值）
	* @param azimuthTime                   方位向时�?
	* @param velocity                      卫星三维速度
	* @return 成功返回0，否则返�?1
	*/
	int getVelocity(double azimuthTime, Velocity& velocity);
	/** @brief 根据方位向时间获取statevector（多项式插值）
	* @param time                          方位向时�?
	* @param osv                           轨道信息
	* @return 成功返回0，否则返�?1
	*/
	int getOrbitData(double time, OSV* osv);
	/** @brief 更新轨道信息
	* @return 成功返回0，否则返�?1
	*/
	int applyOrbit(ProgressCallback progressCallback = nullptr, void* userData = nullptr);

	/** @brief 零多普勒时间搜索算法（移�?Utils 以消除循环依赖）
	* @param stateVectors                  轨道数据
	* @param groundPosition                目标地心三维坐标
	* @param wavelength                    雷达波长
	* @param time_interval                 方位向时间间隔（脉冲重复间隔或行间隔时间�?
	* @param dopplerFrequency              目标多普勒频率（通常�?0.0�?
	* @param zeroDopplerTime               解算得到的零多普勒时刻（返回值）
	* @param distance                      解算得到的传感器与地面点斜距（返回值）
	* @param dopplerThreshold              搜索截止频偏阈值（Hz，默�?0.01�?
	* @return 成功返回 true，否则返�?false
	*/
	static bool findZeroDopplerTime(
		orbitStateVectors& stateVectors,
		const Position& groundPosition,
		double wavelength,
		double time_interval,
		double dopplerFrequency,
		double& zeroDopplerTime,
		double& distance,
		double dopplerThreshold = 0.01);

public:
	Mat stateVectors;
	Mat newStateVectors;
private:
	
	int nv = 10;
	double dt;
	int polyDegree = 3;
	double startTime;
	double stopTime;
	/*轨道信息是否已更�?/
*/
	bool isOrbitUpdated;
};
#endif


class InSAR_API SARDataReader
{
public:
	SARDataReader();
	virtual ~SARDataReader();

	int write_to_h5(const char* dst_h5);

	virtual int init() = 0;

protected:
	virtual int write_custom_h5_data(FormatConversion& conversion, const char* dst_h5) = 0;
	int write_common_coordinates(FormatConversion& conversion, const char* dst_h5);

protected:
	bool b_initialized;
	string acquisition_start_time;
	string acquisition_stop_time;
	double azimuth_resolution;
	double azimuth_spacing;
	double carrier_frequency;
	double prf;
	double range_resolution;
	double range_spacing;
	double slant_range_first_pixel;
	double slant_range_last_pixel;
	Mat state_vec;
	string sensor;
	ComplexMat slc;
	string polarization;

	double inc_center;
	double topleft_lon, topright_lon, bottomleft_lon, bottomright_lon;
	double topleft_lat, topright_lat, bottomleft_lat, bottomright_lat;
};


/*------------------------------------------------*/
/*             COSMO-SkyMed数据读取工具           */
/*------------------------------------------------*/
class InSAR_API CSK_reader : public SARDataReader
{
public:
	CSK_reader(const char* csk_data_file);
	~CSK_reader();
	/** @brief 初始�?
	* @param csk_data_file                    COSMO-SkyMed源hdf5数据文件
	* @return 成功返回0，否则返�?1
	*/
	int init();

protected:
	int write_custom_h5_data(FormatConversion& conversion, const char* dst_h5) override;

private:

	/** @brief 从COSMO-SkyMed源hdf5数据L1A产品中读取数�?
	* @param CSK_data_file                    COSMO-SkyMed源hdf5数据文件
	* @return 成功返回0，否则返�?1
	*/
	int read_data(
		const char* CSK_data_file
	);
	/** @brief 从COSMO-SkyMed源hdf5数据L1A产品中读取单视复图像
	* @param CSK_data_file                    COSMO-SkyMed源hdf5数据文件
	* @param slc                              读出的单视复数据矩阵
	* @return 成功返回0，否则返�?1
	*/

	int read_slc(
		const char* CSK_data_file,
		ComplexMat& slc
	);
	/** @brief 从hdf5文件读取string类型属�?
	* @param object_id                       相应的object
	* @param attribute_name                  string属性名
	* @param attribute_value                 string属性值（返回值）
	* @return 成功返回0，否则返�?1
	*/
	/** @brief 从hdf5文件读取数组类型属�?
	* @param object_id                       相应的object
	* @param attribute_name                  属性名
	* @param attribute_value                 属性值（返回值）
	* @return 成功返回0，否则返�?1
	*/
private:
	string csk_data_file;
	Mat topleft, topright, bottomleft, bottomright;
	Mat inc_coefficient, lon_coefficient, lat_coefficient, row_coefficient, col_coefficient;
	string lookside;
	string orbit_direction;
};

/*------------------------------------------------*/
/*               航天宏图数据读取工具             */
/*------------------------------------------------*/
class InSAR_API HTHT_reader : public SARDataReader
{
public:
	HTHT_reader(const char* data_file, const char* xml_file, int mode = 0);//mode=0为单星模�?mode=1为多星干涉模�?
	~HTHT_reader();
	/** @brief 初始�?
	* @return 成功返回0，否则返�?1
	*/
	int init();

protected:
	int write_custom_h5_data(FormatConversion& conversion, const char* dst_h5) override;

private:

	/** @brief 从宏图L1产品中读取数�?
	@param xml_file                    宏图xml数据文件�?xml�?
	@param data_file                   宏图xml数据文件�?tiff�?
	@return 成功返回0，否则返�?1
	*/
	int read_data(
		const char* xml_file,
		const char* data_file
	);
	/** @brief 从宏图数据L1产品中读取单视复图像
	* @param data_file                        宏图图像数据文件�?tiff�?
	* @param slc                              读出的单视复数据矩阵
	* @return 成功返回0，否则返�?1
	*/

	int read_slc(
		const char* data_file,
		ComplexMat& slc
	);
private:
	string HT_data_file, HT_xml_file;
	int mode = 0;

};

/*------------------------------------------------*/
/*            中科卫星Ku-SAR数据读取工具          */
/*------------------------------------------------*/
class InSAR_API AIRSAT_reader : public SARDataReader
{
public:
	AIRSAT_reader(const char* data_file, const char* xml_file);
	~AIRSAT_reader();
	/** @brief 初始�?
	* @return 成功返回0，否则返�?1
	*/
	int init();

protected:
	int write_custom_h5_data(FormatConversion& conversion, const char* dst_h5) override;

private:

	/** @brief 时间戳转换（UTC2GPS�?
	* @param utc_time                  UTC时间�? 
	* @param gps_time                  GPS时间
	* @return 成功返回0，否则返�?1
	*/
	int UTC2GPS(const char* utc_time, double* gps_time);

	/** @brief 从L1产品中读取数�?
	@param xml_file                    xml数据文件�?xml�?
	@param data_file                   xml数据文件�?tiff�?
	@return 成功返回0，否则返�?1
	*/
	int read_data(
		const char* xml_file,
		const char* data_file
	);
	/** @brief 从L1产品中读取单视复图像
	* @param data_file                        图像数据文件�?tiff�?
	* @param slc                              读出的单视复数据矩阵
	* @return 成功返回0，否则返�?1
	*/

	int read_slc(
		const char* data_file,
		ComplexMat& slc
	);
private:
	string AIRSAT_data_file, AIRSAT_xml_file;
};


/*------------------------------------------------*/
/*                Biomass L1A reader              */
/*------------------------------------------------*/
class InSAR_API Biomass1A_reader : public SARDataReader
{
public:
	Biomass1A_reader(const char* amp_file, const char* phase_file, const char* xml_file, const char* orbit_file, const char* polarization);
	~Biomass1A_reader();
	/** @brief 初始�?
	* @return 成功返回0，否则返�?1
	*/
	int init();

protected:
	int write_custom_h5_data(FormatConversion& conversion, const char* dst_h5) override;

private:

	/** @brief 时间戳转换（UTC2GPS�?
	* @param utc_time                  UTC时间�?
	* @param gps_time                  GPS时间
	* @return 成功返回0，否则返�?1
	*/
	int UTC2GPS(const char* utc_time, double* gps_time);

	/** @brief 从L1产品中读取数�?
	@param xml_file                    xml数据文件�?xml�?
	@param amp_file                    幅度数据文件�?tiff�?
	@param orbit_file                  轨道数据文件�?tiff�?
	@param phase_file                  相位数据文件�?tiff�?
	@return 成功返回0，否则返�?1
	*/
	int read_data(
		const char* xml_file,
		const char* amp_file,
		const char* orbit_file,
		const char* phase_file
	);
	/** @brief 从L1产品中读取单视复图像
	* @param amp_file                         幅度数据文件�?tiff�?
	* @param phase_file                       相位数据文件�?tiff�?
	* @param slc                              读出的单视复数据矩阵
	* @return 成功返回0，否则返�?1
	*/

	int read_slc(
		const char* amp_file,
		const char* phase_file,
		ComplexMat& slc
	);
private:
	string Biomass1A_reader_amp_file, Biomass1A_reader_phase_file, Biomass1A_reader_orbit_file, Biomass1A_reader_xml_file;
};


/*------------------------------------------------*/
/*               陆探1号数据读取工�?             */
/*------------------------------------------------*/
class InSAR_API LUTAN_reader : public SARDataReader
{
public:
	LUTAN_reader(const char* data_file, const char* xml_file, int mode = 1);//mode=1为单星模�?mode=2为双星干涉模�?
	~LUTAN_reader();
	/** @brief 初始�?
	* @return 成功返回0，否则返�?1
	*/
	int init();

protected:
	int write_custom_h5_data(FormatConversion& conversion, const char* dst_h5) override;

private:

	/** @brief 从陆探一号L1产品中读取数�?
	@param xml_file                    陆探一号xml数据文件�?xml�?
	@param data_file                   陆探一号xml数据文件�?tiff�?
	@return 成功返回0，否则返�?1
	*/
	int read_data(
		const char* xml_file,
		const char* data_file
	);
	/** @brief 从陆探一号L1产品中读取单视复图像
	* @param data_file                        陆探一号图像数据文件（.tiff�?
	* @param slc                              读出的单视复数据矩阵
	* @return 成功返回0，否则返�?1
	*/

	int read_slc(
		const char* data_file,
		ComplexMat& slc
	);
private:
	string LT_data_file, LT_xml_file;
	int mode = 1;

};



/*------------------------------------------------*/
/*             天仪涪城一号数据读取工�?          */
/*------------------------------------------------*/
class InSAR_API Spacety_reader : public SARDataReader
{
public:
	Spacety_reader(const char* data_file, const char* xml_file);
	~Spacety_reader();
	/** @brief 初始�?
	* @return 成功返回0，否则返�?1
	*/
	int init();
	/** @brief 初始化（聚束模式数据测试用）
	* @return 成功返回0，否则返�?1
	*/
	int init_test();

protected:
	int write_custom_h5_data(FormatConversion& conversion, const char* dst_h5) override;

private:

	/** @brief 从天仪L1产品中读取数�?
	@param xml_file                    天仪xml数据文件�?xml�?
	@param data_file                   天仪xml数据文件�?tiff�?
	@return 成功返回0，否则返�?1
	*/
	int read_data(
		const char* xml_file,
		const char* data_file
	);
	/** @brief 从天仪L1产品中读取数据（聚束模式数据测试用）
	@param xml_file                    天仪xml数据文件�?xml�?
	@param data_file                   天仪xml数据文件�?tiff�?
	@return 成功返回0，否则返�?1
	*/
	int read_data_test(
		const char* xml_file,
		const char* data_file
	);
	/** @brief 从天仪数据L1产品中读取单视复图像
	* @param data_file                        天仪图像数据文件�?tiff�?
	* @param slc                              读出的单视复数据矩阵
	* @return 成功返回0，否则返�?1
	*/

	int read_slc(
		const char* data_file,
		ComplexMat& slc
	);
private:
	string Spacety_data_file, Spacety_xml_file;
};

/*------------------------------------------------*/
/*               哨兵一号数据读取工�?            */
/*------------------------------------------------*/
class InSAR_API Sentinel1Reader
{
public:
	Sentinel1Reader();
	Sentinel1Reader(const char* xmlfile, const char* tiffFile, const char* PODFile = NULL);
	~Sentinel1Reader();
	/* 加载xml文件
	* @param xmlfile            xml文件
	* @param tiffFile           tiff文件
	* @return 成功返回0，否则返�?1
	*/
	int load(const char* xmlfile, const char* tiffFile);
	/* 获取多普勒中心频率估计数�?
	* @param dcEstimateList               多普勒中心频率估计数据（count×5�?
	* @return 成功返回0，否则返�?1
	*/
	int getDcEstimateList();
	/* 获取多普勒调频率数据
	* @return 成功返回0，否则返�?1
	*/
	int getAzimuthFmRateList();
	/* 获取antennaPattern数据
	* @param antennaPattern_slantRangeTime
	* @param antennaPattern_elevationAngle
	* @return 成功返回0，否则返�?1
	*/
	int getAntennaPattern();
	/* @brief 获取burst个数
	* @param burstCount                 burst个数
	* @return 成功返回0，否则返�?1
	*/
	int getBurstCount(int* burstCount);
	/** @brief 获取每个burst第一行方位向时间
	* @return 成功返回0�?否则返回-1
	*/
	int getBurstAzimuthTime();
	/** @brief 获取每个burst每行第一个有效像素列�?
	* @return 成功返回0，否则返�?1
	*/
	int getFirstValidSample();
	/** @brief 获取每个burst每行最后一个有效像素列�?
	* @param firstValidSample                        每个burst每行最后一个有效像素列�?burstCount×1)
	* @return 成功返回0，否则返�?1
	*/
	int getLastValidSample();
	/** @brief 获取每个burst第一行有效像素行�?
	* @param firstValidLine                          每个burst第一行有效像素行�?burstCount×1)
	* @return 成功返回0，否则返�?1
	*/
	int getFirstValidLine();
	/** @brief 获取每个burst最后一行有效像素行�?
	* @param lastValidLine                           每个burst最后一行有效像素行�?burstCount×1)
	* @return 成功返回0，否则返�?1
	*/
	int getLastValidLine();
	/** @brief 获取地面控制点信�?
	* @param geolocationGridPoint                    地面控制�?n×6，经/�?�?�?�?下视�?
	* @return 成功返回0，否则返�?1
	*/
	int getGeolocationGridPoint();
	/** @brief 更新控制点信�?
	* @return 成功返回0，否则返�?1
	*/
	int updateGeolocationGridPoint();
	/** @brief 根据控制点数据拟合经纬度、下视角与像素坐标（行、列）之间的多项式关�?
	* @return 成功返回0，否则返�?1
	*/
	int fitCoordinateConversionCoefficient();
	/** @brief 获取轨道信息
	* @return 成功返回0，否则返�?1
	*/
	int getOrbitList();
	/** @brief 获取精密轨道数据
	* @param POD_file                                精密轨道数据文件
	* @return 成功返回0，否则返�?1
	*/
	int getPOD(const char* POD_file);
	/** @brief 获取其他参数
	* @return 成功返回0，否则返�?1
	*/
	int getOtherParameters();

	/** @brief 准备数据
	* @param PODFile                                  精密轨道数据文件
	* @return 成功返回0，否则返�?1
	*/
	int prepareData(const char* PODFile = NULL);
	/** @brief 从TIFF文件中读出复图像数据
	* @param slc                                     复图像数�?
	* @return 成功返回0，否则返�?1
	*/
	int getSLC(ComplexMat& slc);
	/** @brief 将数据写入h5文件
	* @param h5File                                  目标h5文件(默认为NULL)
	* @return 成功返回0，否则返�?1
	*/
	int writeToh5(
		const char* h5File,
		int start_burst = -1,
		int end_burst = -1,
		ProgressCallback progressCallback = NULL,
		void* userData = NULL
	);

private:

	/*方位向时间间�?/
*/
	double azimuthTimeInterval;
	/*方位向采样间�?/
*/
	double azimuthPixelSpacing;
	/*距离向采样率*/
	double rangeSamplingRate;
	/*距离向采样间�?/
*/
	double rangePixelSpacing;
	/*雷达载频*/
	double radarFrequency;
	/*方位向扫频率*/
	double azimuthSteeringRate;
	/*最近斜距时�?/
*/
	double slantRangeTime;
	/*Heading*/
	double headingAngle;
	/*中心下视�?/
*/
	double incidence_center;
	/*数据行数*/
	int numberOfLines;
	/*数据列数*/
	int numberOfSamples;
	/*每个burst行数*/
	int linesPerBurst;
	/*burst个数*/
	int burstCount;

	/*卫星（雷达）名称*/
	string sensor;
	/*极化方式*/
	string polarization;
	/*子带名称*/
	string swath;
	/*升降�?/
*/
	string pass;
	/*拍摄起始UTC时间*/
	string startTime;
	/*拍摄结束UTC时间*/
	string stopTime;
	



	/*方位向调频率估计数据*/
	Mat AzimuthFmRateList;
	/*多普勒中心频率估计数�?/
*/
	Mat DcEstimateList;
	/*每个burst第一行方位向时间*/
	Mat burstAzimuthTime;
	/*每个burst每行第一个有效像素列�?/
*/
	Mat firstValidSample;
	/*每个burst每行最后一个有效像素列�?/
*/
	Mat lastValidSample;
	/*每个burst第一行有效数据行�?/
*/
	Mat firstValidLine;
	/*每个burst最后一行有效数据行�?/
*/
	Mat lastValidLine;
	/*轨道原始数据*/
	Mat orbitList;
	/*精密原始轨道数据*/
	Mat preciseOrbitList;
	/*antennaPattern_slantRangeTime*/
	Mat antennaPattern_slantRangeTime;
	/*antennaPattern_elevationAngle*/
	Mat antennaPattern_elevationAngle;
	/*地面控制�?/
*/
	Mat geolocationGridPoint;


	/*经度拟合系数*/
	Mat lon_coefficient;
	/*纬度拟合系数*/
	Mat lat_coefficient;
	/*行坐标拟合系�?/
*/
	Mat row_coefficient;
	/*列坐标拟合系�?/
*/
	Mat col_coefficient;
	/*下视角拟合系�?/
*/
	Mat inc_coefficient;


	/*tiff文件*/
	string tiffFile;
	/*精密轨道数据文件*/
	string PODFile;
	bool isDataAvailable;
	XMLFile xmldoc;
	bool bXmlLoad;
	char m_xmlFileName[2048];
	char error_head[256];
};




/*------------------------------------------------*/
/*                哨兵一号计算工�?               */
/*------------------------------------------------*/
enum SentinelZeroDopplerFailureReason
{
	SENTINEL_ZERO_DOPPLER_NONE = 0,
	SENTINEL_ZERO_DOPPLER_INVALID_INPUT = 1,
	SENTINEL_ZERO_DOPPLER_NO_BRACKET = 2,
	SENTINEL_ZERO_DOPPLER_NONFINITE_RESULT = 3
};

enum SentinelZeroDopplerCallPath
{
	SENTINEL_ZERO_DOPPLER_CALL_UNKNOWN = 0,
	SENTINEL_ZERO_DOPPLER_CALL_MASTER_RG_AZ = 1,
	SENTINEL_ZERO_DOPPLER_CALL_SLAVE_RG_AZ = 2
};

enum SentinelBurstQualityCode
{
	SENTINEL_BURST_NOT_PROCESSED = 0,
	SENTINEL_BURST_NOMINAL = 1,
	SENTINEL_BURST_WARNING_PARTIAL_INVALID = 2,
	SENTINEL_BURST_WARNING_ZERO_OFFSET_FALLBACK = 3,
	SENTINEL_BURST_FAILED = 4
};

struct SentinelZeroDopplerDiagnostic
{
	int reason;
	int returnCode;
	int callPath;
	int imageIndex;
	int burstIndex;
	int line;
	int sample;
	Position groundPosition;
	double targetDoppler;
	double orbitStartTime;
	double orbitStopTime;
	double nearestOrbitTime;
	int stateVectorCount;
	char scene[260];
	char swath[32];
	char polarization[32];

	SentinelZeroDopplerDiagnostic()
		: reason(SENTINEL_ZERO_DOPPLER_NONE), returnCode(0), callPath(SENTINEL_ZERO_DOPPLER_CALL_UNKNOWN),
		imageIndex(0), burstIndex(0), line(-1), sample(-1),
		targetDoppler(0.0), orbitStartTime(0.0),
		orbitStopTime(0.0), nearestOrbitTime(0.0), stateVectorCount(0)
	{
		memset(scene, 0, sizeof(scene));
		memset(swath, 0, sizeof(swath));
		memset(polarization, 0, sizeof(polarization));
	}
};

struct SentinelZeroDopplerFailureStatistic
{
	int imageIndex;
	int burstIndex;
	int reason;
	int callPath;
	int returnCode;
	int count;

	SentinelZeroDopplerFailureStatistic()
		: imageIndex(0), burstIndex(0), reason(SENTINEL_ZERO_DOPPLER_NONE),
		callPath(SENTINEL_ZERO_DOPPLER_CALL_UNKNOWN), returnCode(0), count(0)
	{
	}
};

struct SentinelBurstQualityStatus
{
	int imageIndex;
	int burstIndex;
	int attemptedPoints;
	int validPoints;
	int invalidPoints;
	int zeroDopplerFailures;
	int rangeOrBurstFailures;
	int slantRangeFailures;
	int invalidInputFailures;
	int fitPointCount;
	double fitRms;
	int qualityCode;

	SentinelBurstQualityStatus()
		: imageIndex(0), burstIndex(0), attemptedPoints(0), validPoints(0), invalidPoints(0),
		zeroDopplerFailures(0), rangeOrBurstFailures(0), slantRangeFailures(0), invalidInputFailures(0),
		fitPointCount(0), fitRms(0.0), qualityCode(SENTINEL_BURST_NOT_PROCESSED)
	{
	}
};

/*------------------------------------------------*/
/*                哨兵一号计算工�?               */
/*------------------------------------------------*/
class InSAR_API Sentinel1Utils
{
public:
	/** @brief 默认构造函�?
	*/
	Sentinel1Utils(const char* h5File);
	~Sentinel1Utils();
	/** @brief 初始�?
	* @return 成功返回0，否则返�?1
	*/
	int init();
	/** @brief 计算每个burst的方位向参考时�?
	* @return 成功返回0，否则返�?1
	*/
	int computeReferenceTime();
	/** @brief 计算每个burst的方位向多普勒调频率
	* @return 成功返回0，否则返�?1
	*/
	int computeRangeDependDopplerRate();
	/** @brief 计算每个burst的方位向多普勒中心频�?
	* @return 成功返回0，否则返�?1
	*/
	int computeDopplerCentroid();
	/** @brief 计算每个burst的总多普勒率（调频率加扫频率）
	* @return 成功返回0，否则返�?1
	*/
	int computeDopplerRate();
	/** @brief 计算burst数据的去斜相位和去模相位
	* @param burstIndex                           burst序号
	* @param derampDemodPhase                     去斜去模相位
	* @return 成功返回0，否则返�?1
	*/
	int computeDerampDemodPhase(
		int burstIndex,
		Mat& derampDemodPhase
	);
	/** @brief 从h5文件中读出一个burst的数�?
	* @param burstIndex                          burst序号
	* @param burstSLC                            一个burst的复数据
	* @return 成功返回0，否则返�?1
	*/
	int getBurst(
		int burstIndex,
		ComplexMat& burstSLC
	);



	/** @brief 计算多普勒频�?
	* @param groundPosition                       地面点位�?
	* @param satellitePosition                    卫星位置
	* @param satelliteVelocity                    卫星速度
	* @param dopplerFrequency                     多普勒频率（返回值）
	* @return 成功返回0，否则返�?1
	*/
	int getDopplerFrequency(
		Position groundPosition,
		Position satellitePosition,
		Velocity satelliteVelocity,
		double* dopplerFrequency
	);
	/** @brief 计算地面点对应的零多普勒方位向时�?
	* @param groundPosition                       地面点位�?
	* @param zeroDopplerTime                      零多普勒方位向时�?
	* @param dopplerFrequency                     多普勒频�?默认�?)
	* @return 成功返回0，否则返�?1
	*/
	int getZeroDopplerTime(
		Position groundPosition,
		double* zeroDopplerTime,
		double dopplerFrequency = 0.0
	);
	/** @brief 计算地面点投影到SAR图像坐标系下的距离向和方位向坐标
	* @param burstIndex                           burst序号
	* @param groundPosition                       地面点位�?
	* @param rangeIndex                           距离向坐�?
	* @param azimuthIndex                         方位向坐�?
	* @return 成功返回0，否则返�?1
	*/
	int getRgAzPosition(
		int burstIndex,
		Position groundPosition,
		double* rangeIndex,
		double* azimuthIndex
	);
	/** @brief 计算给定卫星方位向时间和地面点位置时的斜�?
	* @param azimuthTime                         方位向时�?
	* @param groundPosition                      地面点位�?
	* @param slantRange                          斜距
	* @return 成功返回0，否则返�?1
	*/
	int getSlantRange(
		double azimuthTime,
		Position groundPosition,
		double* slantRange
	);
	/** @brief 获取地面点目标所在burst信息
	* @param groundPosition                      地面点目�?
	* @param burstIndice                         burst信息
	* @return 成功返回0，否则返�?1
	*/
	int getBurstIndice(
		Position groundPosition,
		BurstIndices& burstIndice
	);
	/** @brief 计算场景地理位置（经纬度）边�?
	* @param lonMin                           最小经�?
	* @param lonMax                           最大经�?
	* @param latMin                           最小纬�?
	* @param latMax                           最大纬�?
	* @return 成功返回0，否则返�?1
	*/
	int computeImageGeoBoundry(
		double* lonMin,
		double* lonMax,
		double* latMin,
		double* latMax
	);
	/** @brief 计算场景地理位置（经纬度）边�?
	* @param lonMin                           最小经�?
	* @param lonMax                           最大经�?
	* @param latMin                           最小纬�?
	* @param latMax                           最大纬�?
	* @param burstIndex                       burst序号�?-based�?
	* @return 成功返回0，否则返�?1
	*/
	int computeImageGeoBoundry(
		double* lonMin,
		double* lonMax,
		double* latMin,
		double* latMax,
		int burstIndex
	);
	/** @brief burst拼接
	* @param outFile                          deburst输出h5文件
	* @return 成功返回0，否则返�?1
	*/
	int deburst(const char* outFile);
public:

	/*方位向时间间�?/
*/
	double azimuthTimeInterval;
	/*方位向采样间�?/
*/
	double azimuthPixelSpacing;
	/*距离向采样率*/
	double rangeSamplingRate;
	/*距离向采样间�?/
*/
	double rangePixelSpacing;
	/*雷达载频*/
	double radarFrequency;
	/*方位向扫频率*/
	double azimuthSteeringRate;
	/*最近斜距时�?/
*/
	double slantRangeTime;
	/*数据行数*/
	int numberOfLines;
	/*数据列数*/
	int numberOfSamples;
	/*每个burst行数*/
	int linesPerBurst;
	/*每行数据个数*/
	int samplesPerBurst;
	/*burst个数*/
	int burstCount;

	/*卫星（雷达）名称*/
	string sensor;
	/*极化方式*/
	string polarization;
	/*子带名称*/
	string swath;
	/*升降�?/
*/
	string pass;
	/*Heading*/
	double headingAngle;



	/*每个burst方位向参考时间（range_dependent�?/
*/
	Mat referenceTime;
	bool isReferenceTimeAvailable;
	/*每个burst多普勒中心频率（range_dependent�?/
*/
	Mat dopplerCentroid;
	bool isDopplerCentroidAvailable;
	/*每个burst多普勒调频率（range_dependent�?/
*/
	Mat rangeDependDopplerRate;
	bool isRangeDependDopplerRateAvailiable;
	/*方位向调频率估计数据*/
	Mat AzimuthFmRateList;
	/*多普勒中心频率估计数�?/
*/
	Mat DcEstimateList;
	/*每个burst第一行方位向时间*/
	Mat burstAzimuthTime;
	/*每个burst每行第一个有效像素列�?/
*/
	Mat firstValidSample;
	/*每个burst每行最后一个有效像素列�?/
*/
	Mat lastValidSample;
	/*每个burst第一行有效数据行�?/
*/
	Mat firstValidLine;
	/*每个burst最后一行有效数据行�?/
*/
	Mat lastValidLine;
	/*antennaPattern_slantRangeTime*/
	Mat antennaPattern_slantRangeTime;
	/*antennaPattern_elevationAngle*/
	Mat antennaPattern_elevationAngle;
	/*地面控制�?/
*/
	Mat geolocationGridPoint;
	/*每个burst的总多普勒率（调频率加扫频率）*/
	Mat dopplerRate;
	bool isDopplerRateAvailable;
	/*轨道原始数据*/
	Mat orbitList;
	/*精密原始轨道数据*/
	Mat preciseOrbitList;


	/*轨道数据*/
	orbitStateVectors* stateVectors;
	/*h5文件*/
	string h5File;


	/*burst偏移�?/
*/
	int burstOffset;

	bool bInitialized;
	char m_xmlFileName[2048];
	char error_head[256];
};

/*--------------------------------------------------*/
/*              哨兵一号后向地理编码配�?           */
constexpr uint32_t SENTINEL_REFINEMENT_OPTIONS_VERSION = 2;
constexpr uint32_t SENTINEL_REFINEMENT_RESULT_VERSION = 2;
constexpr uint32_t SENTINEL_REFINEMENT_TRANSACTION_STATUS_VERSION = 1;

enum SentinelRefinementQualityCode
{
	SENTINEL_REFINEMENT_QUALITY_NOT_REQUESTED = 0,
	SENTINEL_REFINEMENT_QUALITY_APPLIED = 1,
	SENTINEL_REFINEMENT_QUALITY_APPLIED_ZERO = 2,
	SENTINEL_REFINEMENT_QUALITY_WARNING_LOW_COHERENCE = 3,
	SENTINEL_REFINEMENT_QUALITY_UNAVAILABLE = 4,
	SENTINEL_REFINEMENT_QUALITY_FAILED = 5
};

enum SentinelRefinementWarningFlags
{
	SENTINEL_REFINEMENT_WARNING_NONE = 0,
	SENTINEL_REFINEMENT_WARNING_ESD_LOW_COHERENCE = 1 << 0,
	SENTINEL_REFINEMENT_WARNING_RANGE_NOT_REQUESTED = 1 << 1,
	SENTINEL_REFINEMENT_WARNING_RANGE_EXPLICIT_ZERO = 1 << 2
};

enum SentinelRangeOffsetMode
{
	SENTINEL_RANGE_OFFSET_NONE = 0,
	SENTINEL_RANGE_OFFSET_PROVIDED = 1,
	SENTINEL_RANGE_OFFSET_ESTIMATE = 2
};

// Set only for a successful DLL call; UNSPECIFIED means no successful path was reported.
enum SentinelRefinementExecutionPath
{
	SENTINEL_EXECUTION_PATH_UNSPECIFIED = 0,
	SENTINEL_EXECUTION_PATH_CORE_ONLY_BASELINE = 1,
	SENTINEL_EXECUTION_PATH_POST_REGISTRATION_REFINEMENT = 2
};

enum SentinelRefinementTransactionState
{
	SENTINEL_REFINEMENT_TRANSACTION_NONE = 0,
	SENTINEL_REFINEMENT_TRANSACTION_COMPLETE = 1,
	SENTINEL_REFINEMENT_TRANSACTION_IN_PROGRESS = 2,
	SENTINEL_REFINEMENT_TRANSACTION_FAILED = 3,
	SENTINEL_REFINEMENT_TRANSACTION_INVALID = 4
};

// imageIndex is always 1-based, matching masterIndex and Sentinel burst status.
struct SentinelRangeOffsetInput
{
	int imageIndex;
	double offset;
};

// POD ABI contract. NONE requires rangeOffsets == nullptr and does not estimate
// or apply range offsets. PROVIDED requires one 1-based entry per slave image;
// a present 0.0 is an explicit zero correction. ESTIMATE requires nullptr and
// runs the frozen Registration sampling/correlation strategy in this DLL.
struct SentinelRefinementOptions
{
	uint32_t version;
	uint32_t structSize;
	int enableEsd;
	int esdRangeMultilook;
	int esdAzimuthMultilook;
	double esdCoherenceThreshold;
	double esdHistogramBinSize;
	double esdDopplerRateHz;
	double esdAzimuthBandwidthHz;
	int rangeOffsetMode;
	int rangeSamplePointCount;
	int rangeTemplateSize;
	int rangeSearchSize;
	const SentinelRangeOffsetInput* rangeOffsets;
	int rangeOffsetCount;
	const char* transactionDirectory;
};

struct SentinelRefinementImageResult
{
	int imageIndex;
	double esdAzimuthOffset;
	double rangeOffset;
	int esdQualityCode;
	int rangeQualityCode;
	uint32_t warningFlags;
};

// The caller owns images. imageCapacity must be at least the slave image count.
struct SentinelRefinementResult
{
	uint32_t version;
	uint32_t structSize;
	SentinelRefinementImageResult* images;
	int imageCapacity;
	int imageCount;
	int executionPath;
};

enum SentinelBackGeocodingVerificationMismatchFlags
{
	SENTINEL_VERIFICATION_MISMATCH_NONE = 0,
	SENTINEL_VERIFICATION_MISMATCH_DIMENSIONS = 1 << 0,
	SENTINEL_VERIFICATION_MISMATCH_SLC = 1 << 1,
	SENTINEL_VERIFICATION_MISMATCH_COEFFICIENTS = 1 << 2,
	SENTINEL_VERIFICATION_MISMATCH_ESD_OFFSET = 1 << 3,
	SENTINEL_VERIFICATION_MISMATCH_RANGE_OFFSET = 1 << 4,
	SENTINEL_VERIFICATION_MISMATCH_REQUIRED_DATASET = 1 << 5,
	SENTINEL_VERIFICATION_MISMATCH_DEBURST_ORIGIN = 1 << 6
};
struct SentinelBackGeocodingVerificationImageResult;
struct SentinelBackGeocodingVerificationResult
{
	SentinelBackGeocodingVerificationImageResult* images;
	int imageCapacity;
	int imageCount;
	int passed;
};


// The caller owns images. imageIndex is 1-based. Errors are absolute values.
struct SentinelBackGeocodingVerificationImageResult
{
	int imageIndex;
	int outputRows;
	int outputColumns;
	int referenceRows;
	int outputFirstSourceRow;
	int outputFirstSourceColumn;
	int referenceFirstSourceRow;
	int referenceFirstSourceColumn;
	int referenceColumns;
	int comparedCoefficientBursts;
	uint32_t mismatchFlags;
	double maxCoefficientAbsError;
	double esdOffsetAbsError;
	double rangeOffsetAbsError;
	double maxSlcComponentAbsError;
	double rmsSlcComponentError;
};
// POD status for the output-directory transaction. COMPLETE is returned only
// after the manifest and every H5 marker agree on the transaction ID.
struct SentinelRefinementTransactionStatus
{
	uint32_t version;
	uint32_t structSize;
	int state;
	int outputCount;
	int verifiedOutputCount;
};
/*--------------------------------------------------*/
class InSAR_API Sentinel1BackGeocoding
{
public:
	Sentinel1BackGeocoding();
	~Sentinel1BackGeocoding();
	/** @brief 初始化后向地理编码配�?
	* @param h5Files                       哨兵一号原始数据文�?
	* @param outFiles                      处理结果保存文件
	* @param DEMPath                       DEM文件路径
	* @param masterIndex                   主影像序�?
	* @return 成功返回0，否则返�?1
	*/
	int init(
		vector<string>& h5Files,
		vector<string>& outFiles,
		const char* DEMPath,
		int masterIndex,
		InSARDiagnosticCallback diagnosticCallback = nullptr,
		void* diagnosticUserData = nullptr
	);
	/** @brief 加载哨兵一号数�?
	* @param h5Files                       哨兵一号原始数据文�?
	* @return 成功返回0，否则返�?1
	*/
private:
	int loadData(vector<string>& h5Files);
	/** @brief 设置DEM文件路径
	* @param DEMPath                       DEM文件路径
	* @return 成功返回0，否则返�?1
	*/
	int setDEMPath(const char* DEMPath);
	/** @brief 加载数字高程模型
	* @param filepath                      DEM文件路径
	* @param lonMin                        最小经�?
	* @param lonMax                        最大经�?
	* @param latMin                        最小纬�?
	* @param latMax                        最大纬�?
	* @return 成功返回0，否则返�?1
	*/
	int loadDEM(
		const char* filepath,
		double lonMin,
		double lonMax,
		double latMin,
		double latMax
	);
	/** @brief 加载处理结果保存文件
	* @param outFiles                     处理结果保存文件
	* @return 成功返回0，否则返�?1
	*/
	int loadOutFiles(vector<string>& outFiles);
	/** @brief 准备结果保存文件
	* @return 成功返回0，否则返�?1
	*/
	int prepareOutFiles();
	/** @brief 设置主影�?
	* @param masterIndex                  主影像序�?
	* @return 成功返回0，否则返�?1
	*/
	int setMasterIndex(int masterIndex);



	/** @brief 计算主辅图像之间的burst偏移�?
	* @return 成功返回0，否则返�?1
	*/
	int computeBurstOffset();
	/** @brief 去斜去模操作
	* @param derampDemodPhase                 斜模相位
	* @param slc                              复图像数�?
	* @return 成功返回0，否则返�?1
	*/
	int performDerampDemod(
		Mat& derampDemodPhase,
		ComplexMat& slc
	);
	/** @brief 计算DEM点投影在SAR辅图像中的位�?
	* @param slaveImageIndex                       辅图像序�?
	* @param mBurstIndex                           主图像burst序号
	* @return 成功返回0，否则返�?1
	*/
	int computeSlavePosition(
		int slaveImagesIndex,
		int mBurstIndex
	);
	/** @brief 计算辅图像偏�?
	* @param slaveAzimuthOffset                    辅图像方位向偏移
	* @param slaveRangeOffset                      辅图像距离向偏移
	* @return 成功返回0，否则返�?1
	*/
	int computeSlaveOffset(Mat& slaveAzimuthOffset, Mat& slaveRangeOffset);
	/** @brief 拟合辅图像偏移（1阶拟合，offset = a0 + a1 * x + a2 * y�?
	* @param slaveOffset                           偏移�?
	* @param a0                                    拟合系数
	* @param a1                                    拟合系数
	* @param a2                                    拟合系数
	* @return 成功返回0，否则返�?1
	*/
	int fitSlaveOffset(
		Mat& slaveOffset,
		double* a0,
		double* a1,
		double* a2
	);
	/** @brief 复图像双线性插值重采样（inplace，原地操作）
	* @param slc                                   待重采样图像（原地操作）
	* @param dstHeight                             重采样图像高�?
	* @param dstWidth                              重采样图像宽�?
	* @param a0Rg                                  距离向偏移拟合系�?
	* @param a1Rg                                  距离向偏移拟合系�?
	* @param a2Rg                                  距离向偏移拟合系�?
	* @param a0Az                                  方位向偏移拟合系�?
	* @param a1Az                                  方位向偏移拟合系�?
	* @param a2Az                                  方位向偏移拟合系�?
	* @return 成功返回0，否则返�?1
	*/
	int performBilinearResampling(
		ComplexMat& slc,
		int dstHeight,
		int dstWidth,
		double a0Rg, double a1Rg, double a2Rg,
		double a0Az, double a1Az, double a2Az
	);
	/** @brief 复图像sinc插值重采样（inplace，原地操作）
	* @param slc                                   待重采样图像（原地操作）
	* @param dstHeight                             重采样图像高�?
	* @param dstWidth                              重采样图像宽�?
	* @param a0Rg                                  距离向偏移拟合系�?
	* @param a1Rg                                  距离向偏移拟合系�?
	* @param a2Rg                                  距离向偏移拟合系�?
	* @param a0Az                                  方位向偏移拟合系�?
	* @param a1Az                                  方位向偏移拟合系�?
	* @param a2Az                                  方位向偏移拟合系�?
	* @return 成功返回0，否则返�?1
	*/
	int performSincResampling(
		ComplexMat& slc,
		int dstHeight,
		int dstWidth,
		double a0Rg, double a1Rg, double a2Rg,
		double a0Az, double a1Az, double a2Az
	);
	/** @brief 辅图像双线性插值重采样
	* @param mBurstIndex                           主图像burst序号
	* @param slaveImageIndex                       辅图像序�?
	* @param slaveSLC                              重采样后的辅图像数据
	* @return 成功返回0，否则返�?1
	*/
	// Uses Deramp -> sinc SLC/phase resampling -> Reramp.
	int slaveSincInterpolation(
		int mBurstIndex,
		int slaveImageIndex,
		ComplexMat& slaveSLC
	);
	/** @brief 计算deburst信息
	* @return 成功返回0，否则返�?1
	*/
	int deBurstConfig();
	int materializeDeburstOutput(const char* fullBurstFile, const char* deburstFile);

public:
	/** @brief 后向地理编码配准
	* @return 成功返回0，否则返�?1
	*/
	int backGeoCodingCoregistration(
		InSARDiagnosticCallback diagnosticCallback = nullptr,
		void* diagnosticUserData = nullptr
	);
	// Unified second-round entry point. When both refinement arguments are
	// supplied, initial back-geocoding and post-registration refinement run as
	// one DLL-owned workflow. Passing both as null preserves the legacy core run.
	int backGeoCodingCoregistration(
		const SentinelRefinementOptions* refinementOptions,
		SentinelRefinementResult* refinementResult,
		InSARDiagnosticCallback diagnosticCallback = nullptr,
		void* diagnosticUserData = nullptr
	);
	/**
	 * Applies ESD and optional caller-provided range offsets to full-burst output
	 * H5 files. Success is returned only after the output-group transaction is
	 * committed as complete in refinement_transaction.json.
	 */
	int applyPostRegistrationRefinement(
		const SentinelRefinementOptions& options,
		SentinelRefinementResult& result,
		InSARDiagnosticCallback callback = nullptr,
		void* userData = nullptr
	);
	int getPostRegistrationRefinementTransactionStatus(
		SentinelRefinementTransactionStatus& status) const;
	// Compares final current H5 outputs with fixed reference H5 outputs inside the DLL.
	int verifyOutputsAgainstReference(
		const vector<string>& referenceFiles,
		double coefficientTolerance,
		double offsetTolerance,
		double slcTolerance,
		SentinelBackGeocodingVerificationResult& result) const;
	int recoverPostRegistrationRefinementTransaction();
	/** Configure the callback used by subsequent task calls on this instance. */
	void setDiagnosticCallback(InSARDiagnosticCallback diagnosticCallback, void* diagnosticUserData) noexcept;
	// Restores original outputs from a failed or interrupted refinement
	// transaction, then removes its manifest and temporary artifacts. This can
	// be called after init() rejects a failed transaction because init retains
	// the validated output paths for recovery.
	/** @brief Thread-safe cooperative cancellation. Cancelled operations return -2. */
	void requestCancel() noexcept;
	void clearCancelRequest() noexcept;
	bool isCancelRequested() const noexcept;
	/** @brief 获取最近一次零多普勒求解失败的诊断信息
	* @return 有诊断信息返�?，否则返�?1
	*/
	int getLastZeroDopplerDiagnostic(SentinelZeroDopplerDiagnostic& diagnostic) const;
	/** @brief Get zero-Doppler failure counts grouped by image, burst, reason, and call path.
	* @return 0 on success, -1 when no diagnostic state exists.
	*/
	int getZeroDopplerFailureStatistics(vector<SentinelZeroDopplerFailureStatistic>& statistics) const;
	/** @brief 获取当前任务的burst几何质量状�?
	* @return 成功返回0，否则返�?1
	*/
	int getBurstQualityStatus(vector<SentinelBurstQualityStatus>& status) const;

private:

	/*影像数量*/
	int numOfImages;
	/*主影像序�?/
*/
	int masterIndex;
	/*哨兵一号数�?/
*/
	vector<Sentinel1Utils*> su;
	/*处理结果保存文件*/
	vector<string> outFiles;
	vector<string> fullBurstFiles;
	/*数字高程模型*/
	DigitalElevationModel* dem;
	/*数字高程模型路径*/
	string DEMPath;
	/*DEM投影到主图像的方位向坐标*/
	Mat masterAzimuth;
	/*DEM投影到主图像的距离向坐标*/
	Mat masterRange;
	/*DEM投影到辅图像的方位向坐标*/
	Mat slaveAzimuth;
	/*DEM投影到辅图像的距离向坐标*/
	Mat slaveRange;
	/*DEM点投影到主图像的坐标是否计算完成*/
	bool isMasterRgAzComputed;

	/*deburst参数*/
	Mat start;
	/*deburst参数*/
	Mat end;
	/*deburst参数*/
	int deburstLines;
	/*burst配置信息是否计算完成*/
	bool isdeBurstConfig;

	/*无效坐标�?1.0�?/
*/
	double invalidRgAzIndex = -1.0;
	/*无效偏移量（-9999.0�?/
*/
	double invalidOffset = -9999.0;
	bool burstOffsetComputed;
	bool deferFinalDeburstOutput;
	char error_head[256];

private:
	std::atomic<bool> cancelRequested;
	InSARDiagnosticCallback diagnosticCallback;
	void* diagnosticUserData;

};




















#endif // !__FORMATCONVERSION__H__


